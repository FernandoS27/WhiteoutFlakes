// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Live thumbnails for a file list: load the thing, render it small, read the
// pixels back, blit them with GDI.
//
// Shared by the GUI examples — include it with
//
//   #[path = "common/thumbs.rs"]
//   mod thumbs;
//
// from inside a `#[cfg(windows)]` module. Not a library: an example that
// happens to be used twice.
//
// The renderer here is deliberately its own, separate from whatever the host
// is showing. A thumbnail pass loads and clears the scene, so running it
// through the visible renderer would evict the thing the user is looking at.
// The cost is a second device.
//
// A thumbnail is a *strip* of frames, not one picture: with `set_frames(n)` an
// effect is sampled n times as it runs, and the caller flips through them to
// animate the tile. The storage explorer gets this for free — its cells are
// live viewports drawn by the GPU every frame — but a GDI host has to read
// each frame back, so it records a short loop once instead of re-rendering
// forever. One frame (the default) is the still picture a list row wants.

#![allow(dead_code)] // each example uses a different part of this

use std::collections::HashMap;

use windows_sys::Win32::Foundation::RECT;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::UI::Controls::{DRAWITEMSTRUCT, ODS_SELECTED};

use whiteoutflakes::{GfxApi, Renderer};

use super::theme::*;

/// Default thumbnail edge, in pixels — a list row's worth.
pub const THUMB: i32 = 64;
/// Height of a list row carrying one.
pub const ROW_H: i32 = THUMB + 8;

/// Effect ticks (at 1/30 s) between one recorded frame and the next. Playing
/// the strip back at this rate is real time.
const FRAME_STEP: u32 = 2;
/// Ticks to run before the first recorded frame: an effect is nothing at t=0.
const WARMUP: u32 = 6;
/// Where a single still is sampled from — a burst is over in half a second, a
/// slow effect has barely started by then, so try three and keep the fullest.
const STILL_SAMPLES: &[u32] = &[9, 18, 33];

/// A renderer that exists only to produce thumbnails, plus the cache and the
/// queue of what still has to be rendered.
pub struct Thumbs {
    renderer: Renderer,
    target: u32,
    edge: i32,
    frames: usize,
    /// Keyed by the provider path; one entry is a strip of BGRA frames. An
    /// empty strip is a negative entry: that file was tried and did not
    /// render, so it is never queued again.
    cache: HashMap<String, Vec<Vec<u8>>>,
    queue: Vec<(String, bool)>,
}

impl Thumbs {
    pub fn new(api: GfxApi) -> Option<Thumbs> {
        Thumbs::with_size(api, THUMB)
    }

    /// Thumbnails `edge` pixels square. That is the off-screen target's size as
    /// well as the picture's, so a grid of big cells asks for big thumbnails
    /// rather than blowing up 64² ones.
    pub fn with_size(api: GfxApi, edge: i32) -> Option<Thumbs> {
        let edge = edge.max(16);
        let mut renderer = Renderer::new();
        if !renderer.use_bundled_shaders() {
            return None;
        }
        renderer.pipeline()?.init_device(api);
        if !renderer.pipeline()?.is_device_ready() {
            return None;
        }
        // The list colour, so a thumbnail sits on the row rather than in a box
        // of its own.
        renderer.settings()?.set_background_color(28, 31, 36);
        hide_grid(&mut renderer);
        let target = renderer.pipeline()?.create_offscreen_target(edge, edge);
        (target != 0).then_some(Thumbs {
            renderer,
            target,
            edge,
            frames: 1,
            cache: HashMap::new(),
            queue: Vec::new(),
        })
    }

    pub fn size(&self) -> i32 {
        self.edge
    }

    /// How many frames an effect's thumbnail should hold from here on. One is
    /// a still; more makes it a loop the caller can play. Already-rendered
    /// entries are not re-rendered until they are asked for again, so this is
    /// a per-folder decision rather than a global one — see [`Thumbs::request`].
    pub fn set_frames(&mut self, frames: usize) {
        self.frames = frames.max(1);
    }

    /// `None` while a thumbnail has not been rendered yet, `Some(&[])` once it
    /// has been tried and failed.
    pub fn get(&self, path: &str) -> Option<&[u8]> {
        self.frames(path)
            .map(|f| f.first().map_or(&[][..], |b| b.as_slice()))
    }

    /// Every frame of a thumbnail, oldest first — one entry for a still.
    pub fn frames(&self, path: &str) -> Option<&[Vec<u8>]> {
        self.cache.get(path).map(|v| v.as_slice())
    }

    /// Ask for a thumbnail. Cheap to call for a whole directory: already
    /// rendered or already queued paths are ignored — except one held as a
    /// still when a strip is wanted, which is queued again to be filled in.
    pub fn request(&mut self, path: &str, effect: bool) {
        if path.is_empty() {
            return;
        }
        if let Some(have) = self.cache.get(path) {
            // Empty is a failure, and a failure is not retried.
            let want = if effect { self.frames } else { 1 };
            if have.is_empty() || have.len() >= want {
                return;
            }
        }
        if self.queue.iter().any(|(p, _)| p == path) {
            return;
        }
        self.queue.push((path.to_string(), effect));
    }

    /// Drop what has not been rendered yet — for leaving a directory, so the
    /// new one is not stuck behind the old one's backlog. The pictures stay, so
    /// walking back in is instant.
    ///
    /// Strips do not: a frame is 64 KB at grid size, and a corpus folder can
    /// hold hundreds of effects, so everything is collapsed to its best single
    /// frame. Only the folder on screen is worth animating, and re-entering one
    /// shows the still while the loop is recorded again.
    pub fn forget_queue(&mut self) {
        self.queue.clear();
        for strip in self.cache.values_mut() {
            if strip.len() > 1 {
                let best = fullest(strip);
                let keep = strip.swap_remove(best);
                strip.clear();
                strip.push(keep);
            }
        }
    }

    pub fn queued(&self) -> usize {
        self.queue.len()
    }

    /// Render the next queued thumbnail, if there is one. Returns the path it
    /// finished, so the caller can repaint. At most one per call: a thumbnail
    /// costs a load and a GPU sync, and the host has to keep running.
    pub fn tick(&mut self) -> Option<String> {
        if self.queue.is_empty() {
            return None;
        }
        let (path, effect) = self.queue.remove(0);
        let bits = self.render(&path, effect).unwrap_or_default();
        self.cache.insert(path.clone(), bits);
        Some(path)
    }

    /// How the given paths came out: rendered, blank, failed. A thumbnail that
    /// read back fine but drew nothing counts as blank, not as done —
    /// otherwise the tally reports on the readback rather than the pictures.
    pub fn tally<'a>(&self, paths: impl Iterator<Item = &'a str>) -> (usize, usize, usize) {
        let (mut ok, mut blank, mut bad) = (0, 0, 0);
        for p in paths {
            match self.get(p) {
                Some(b) if b.is_empty() => bad += 1,
                Some(b) if coverage(b) < 0.005 => blank += 1,
                Some(_) => ok += 1,
                None => {}
            }
        }
        (ok, blank, bad)
    }

    /// Load one file, render it, and hand back its frames as BGRA — the order
    /// GDI wants. `None` if it did not load or did not read back.
    fn render(&mut self, path: &str, effect: bool) -> Option<Vec<Vec<u8>>> {
        self.renderer.loader()?.request_clear_all();
        if let Some(dir) = std::path::Path::new(path).parent() {
            self.renderer
                .scene()?
                .set_pe1_base_path(&dir.to_string_lossy());
        }

        let mut loader = self.renderer.loader()?;
        let handle = if effect {
            loader.spawn_effect(path)
        } else {
            loader.spawn_unit(path)
        };
        drop(loader);
        if handle == 0 {
            return None;
        }

        let mode = if effect {
            whiteoutflakes::RenderMode::HD
        } else {
            self.renderer.actor(handle)?.preferred_render_mode()
        };
        self.renderer.settings()?.set_render_mode(mode);
        if let Some(mut cam) = self.renderer.camera_at(0) {
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            // Closer than a viewport uses: at 64px the subject has to fill the
            // frame or there is nothing to recognise.
            cam.set_distance(if effect { 210.0 } else { 340.0 });
            cam.set_pitch(0.25);
        }
        if let Some(mut p) = self.renderer.playback() {
            p.restart();
        }

        // HD composites post-tonemap in *linear* and leans on an sRGB
        // render-target view to encode it. An offscreen target is plain UNORM
        // (`kSdSceneFormat`), so what comes back is linear and GDI — which
        // assumes sRGB — draws it far too dark. SD writes gamma bytes straight
        // out and must be passed through untouched.
        let encode = mode == whiteoutflakes::RenderMode::HD;

        // A strip runs the effect and keeps every FRAME_STEPth frame, so the
        // caller can loop it. A model has no burst to catch and a list row has
        // nowhere to play one, so both take the single-still path.
        if effect && self.frames > 1 {
            let last = WARMUP + FRAME_STEP * (self.frames as u32 - 1);
            let mut strip = Vec::with_capacity(self.frames);
            for step in 1..=last {
                self.renderer.advance(1.0 / 30.0);
                self.renderer.pipeline()?.render_viewport(self.target, 0);
                if step >= WARMUP && (step - WARMUP) % FRAME_STEP == 0 {
                    strip.push(self.grab(encode)?);
                }
            }
            return (!strip.is_empty()).then_some(strip);
        }

        // An effect is nothing at t=0 — it has to run before there is anything
        // worth a picture, and *when* it peaks varies. A model only needs its
        // first pose.
        let samples: &[u32] = if effect { STILL_SAMPLES } else { &[5] };
        let last = *samples.last().unwrap_or(&1);
        let mut best: Option<(f32, Vec<u8>)> = None;
        for step in 1..=last {
            self.renderer.advance(1.0 / 30.0);
            self.renderer.pipeline()?.render_viewport(self.target, 0);
            if !samples.contains(&step) {
                continue;
            }
            let bgra = self.grab(encode)?;
            let c = coverage(&bgra);
            if best.as_ref().map_or(true, |(bc, _)| c > *bc) {
                best = Some((c, bgra));
            }
        }
        best.map(|(_, bgra)| vec![bgra])
    }

    /// Read the target back as BGRA, gamma-encoding it when the frame was
    /// composited in linear.
    fn grab(&mut self, encode: bool) -> Option<Vec<u8>> {
        let (rgba, w, h) = self.renderer.pipeline()?.readback_target(self.target)?;
        if w != self.edge || h != self.edge {
            return None;
        }
        let lut = srgb_lut();
        let mut bgra = Vec::with_capacity(rgba.len());
        for px in rgba.chunks_exact(4) {
            if encode {
                bgra.extend_from_slice(&[
                    lut[px[2] as usize],
                    lut[px[1] as usize],
                    lut[px[0] as usize],
                    255,
                ]);
            } else {
                bgra.extend_from_slice(&[px[2], px[1], px[0], 255]);
            }
        }
        Some(bgra)
    }

    pub fn shutdown(&mut self) {
        if let Some(mut p) = self.renderer.pipeline() {
            if p.is_device_ready() {
                p.shutdown();
            }
        }
    }
}

/// Linear → sRGB, one byte in, one byte out. A table because the alternative
/// is a `powf` per channel per pixel, on the message pump.
fn srgb_lut() -> &'static [u8; 256] {
    static LUT: std::sync::OnceLock<[u8; 256]> = std::sync::OnceLock::new();
    LUT.get_or_init(|| {
        let mut t = [0u8; 256];
        for (i, v) in t.iter_mut().enumerate() {
            let c = i as f32 / 255.0;
            let s = if c <= 0.003_130_8 {
                c * 12.92
            } else {
                1.055 * c.powf(1.0 / 2.4) - 0.055
            };
            *v = (s * 255.0 + 0.5).clamp(0.0, 255.0) as u8;
        }
        t
    })
}

/// Index of the frame with the most of the subject in it.
fn fullest(strip: &[Vec<u8>]) -> usize {
    strip
        .iter()
        .enumerate()
        .max_by(|a, b| {
            coverage(a.1)
                .partial_cmp(&coverage(b.1))
                .unwrap_or(std::cmp::Ordering::Equal)
        })
        .map_or(0, |(i, _)| i)
}

/// A picker shows the thing, not the studio it is standing in.
pub fn hide_grid(renderer: &mut Renderer) {
    let Some(mut settings) = renderer.settings() else {
        return;
    };
    if let Some(mut flags) = settings.display_flags() {
        flags.set_show_grid(false);
        settings.set_display_flags(&flags);
    }
}

/// Fraction of a thumbnail that is not the background, taking the corner pixel
/// as the background.
pub fn coverage(bgra: &[u8]) -> f32 {
    let Some(bg) = bgra.get(..3) else {
        return 0.0;
    };
    let n = bgra.len() / 4;
    let hits = bgra
        .chunks_exact(4)
        .filter(|px| (0..3).any(|c| px[c].abs_diff(bg[c]) > 12))
        .count();
    hits as f32 / n.max(1) as f32
}

/// What sits at the left of a list row.
pub enum Cell<'a> {
    Folder,
    /// Queued, not rendered yet.
    Pending,
    /// Tried and could not be rendered.
    Failed,
    Image(&'a [u8]),
}

/// Paint one owner-drawn list row: picture on the left, name beside it. The
/// caller owns the row model; this only knows how a row looks.
pub unsafe fn draw_row(d: &DRAWITEMSTRUCT, label: &str, cell: Cell, accent: bool) {
    unsafe {
        let sel = (d.itemState & ODS_SELECTED) != 0;
        fill(d.hDC, &d.rcItem, if sel { C_BTN_HOT } else { C_LIST });
        if d.itemID == u32::MAX {
            return;
        }
        SetBkMode(d.hDC, TRANSPARENT as i32);
        let (x, y) = (d.rcItem.left + 4, d.rcItem.top + 4);
        let box_ = RECT {
            left: x,
            top: y,
            right: x + THUMB,
            bottom: y + THUMB,
        };
        match cell {
            Cell::Image(bits) => blit(d.hDC, &box_, bits),
            Cell::Folder => folder_glyph(d.hDC, x, y),
            Cell::Pending => placeholder(d.hDC, x, y, "..."),
            Cell::Failed => placeholder(d.hDC, x, y, "x"),
        }

        SetTextColor(
            d.hDC,
            match (sel, accent) {
                (true, _) => C_ACCENT,
                (false, true) => C_ACCENT,
                (false, false) => C_TEXT,
            },
        );
        let mut text = RECT {
            left: d.rcItem.left + THUMB + 14,
            right: d.rcItem.right - 4,
            ..d.rcItem
        };
        let mut wide: Vec<u16> = label.encode_utf16().collect();
        DrawTextW(
            d.hDC,
            wide.as_mut_ptr(),
            wide.len() as i32,
            &mut text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
        );
    }
}

unsafe fn placeholder(hdc: HDC, x: i32, y: i32, mark: &str) {
    unsafe {
        let mut r = RECT {
            left: x,
            top: y,
            right: x + THUMB,
            bottom: y + THUMB,
        };
        let b = CreateSolidBrush(C_BTN);
        FrameRect(hdc, &r, b);
        DeleteObject(b as _);
        let mut wide: Vec<u16> = mark.encode_utf16().collect();
        SetTextColor(hdc, C_TEXT_DIM);
        DrawTextW(
            hdc,
            wide.as_mut_ptr(),
            wide.len() as i32,
            &mut r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE,
        );
    }
}

unsafe fn folder_glyph(hdc: HDC, x: i32, y: i32) {
    unsafe {
        let b = CreateSolidBrush(C_ACCENT);
        let mid = y + THUMB / 2;
        let tab = RECT {
            left: x + 12,
            top: mid - 16,
            right: x + 30,
            bottom: mid - 10,
        };
        FillRect(hdc, &tab, b);
        let body = RECT {
            left: x + 12,
            top: mid - 12,
            right: x + THUMB - 12,
            bottom: mid + 14,
        };
        FillRect(hdc, &body, b);
        DeleteObject(b as _);
    }
}

/// Blit one cached thumbnail into `dst`. Top-down DIB (negative height)
/// because that is the row order readback hands back.
///
/// The source edge is read off the buffer rather than assumed, so a caller
/// drawing 128² cells and one drawing 64² rows can share this — and neither
/// can silently disagree with the `Thumbs` that produced the pixels.
pub unsafe fn blit(hdc: HDC, dst: &RECT, bgra: &[u8]) {
    unsafe {
        let edge = edge_of(bgra);
        if edge == 0 {
            return;
        }
        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmiHeader.biWidth = edge;
        bmi.bmiHeader.biHeight = -edge;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        // Halftone rather than the default nearest-neighbour: a cell is rarely
        // exactly the size it was rendered at, and dropped rows read as a
        // broken picture.
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, std::ptr::null_mut());
        StretchDIBits(
            hdc,
            dst.left,
            dst.top,
            dst.right - dst.left,
            dst.bottom - dst.top,
            0,
            0,
            edge,
            edge,
            bgra.as_ptr().cast(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY,
        );
    }
}

/// Edge of a square BGRA buffer, or 0 if it is not one.
fn edge_of(bgra: &[u8]) -> i32 {
    let px = bgra.len() / 4;
    let edge = (px as f64).sqrt() as i32;
    if edge > 0 && (edge as usize) * (edge as usize) == px {
        edge
    } else {
        0
    }
}
