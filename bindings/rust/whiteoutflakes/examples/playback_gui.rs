// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The scene transport driven from a Rust GUI: a viewport with a dark
// transport bar under it — play, pause, stop, restart, and a timeline you
// can drag to scrub the animation.
//
//   cargo run --example playback_gui -- path/to/model.mdx
//   cargo run --example playback_gui -- path/to/effect.pkb
//
// The viewport is a real child window with its own swap chain, rendered
// directly and presented. No readback, no intermediate texture — the same
// arrangement `examples/multi_window` proves out, with the chrome laid
// beneath it instead of in a second window. A GUI toolkit that owns the GPU
// would have forced the scene through a CPU round trip instead.
//
// The chrome is drawn by hand for the same reason the viewport is a real
// window: everything here is owner-drawn GDI, so the panel is dark, the
// buttons carry vector glyphs rather than text, and the timeline is a
// control this file paints and drags itself. Stock Win32 controls cannot be
// made dark without more fighting than drawing them outright.
//
// What to watch, and why it is the point:
//
//   * One switch governs models AND corn effects. A frame advances in two
//     halves — animation clocks, then the effect simulations — and they are
//     separate calls; the transport lives on the scene so both read it.
//     Pause a Reforged model mid-swing and the geometry stops with the
//     particles hanging in the air rather than draining away.
//   * Pause versus stop. Pause holds the instant, so Play carries on from
//     there. Stop rewinds to the scene's first frame and holds; the
//     corn effects readout drops as the interrupted run's particles go.
//   * The timeline scrubs the active sequence, playing or paused.
//
// Windows-only, like the other examples.

#[cfg(not(windows))]
fn main() {
    eprintln!("examples/playback_gui is Windows-only.");
}

#[cfg(windows)]
fn main() {
    imp::run();
}

#[cfg(windows)]
#[allow(clippy::too_many_arguments)]
mod imp {
    use std::cell::RefCell;
    use std::path::PathBuf;
    use std::time::Instant;

    use windows_sys::core::w;
    use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
    use windows_sys::Win32::Graphics::Gdi::*;
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{ReleaseCapture, SetCapture};
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    use whiteoutflakes::{PlaybackState, Renderer};

    // ── Palette ──────────────────────────────────────────────────────────
    // COLORREF is 0x00BBGGRR, so these read backwards from hex web colours.
    const C_BG: u32 = 0x001E1B18; // bar background  (#181B1E)
    const C_BTN: u32 = 0x002B2724; // button face     (#24272B)
    const C_BTN_HOT: u32 = 0x003A342F;
    const C_BTN_DOWN: u32 = 0x00524738;
    const C_GLYPH: u32 = 0x00E8E4E0;
    const C_ACCENT: u32 = 0x00E0A050; // filled timeline + active state
    const C_TRACK: u32 = 0x00332E2A;
    const C_TEXT: u32 = 0x00B8B0A8;
    const C_TEXT_DIM: u32 = 0x00807870;

    /// What a button draws. Text labels would have needed a font and still
    /// looked like a dialog; these are four-to-six GDI calls each.
    #[derive(Clone, Copy, PartialEq)]
    enum Glyph {
        Play,
        Pause,
        Stop,
        Restart,
        Slower,
        Faster,
    }

    const BUTTONS: [(Glyph, &str); 6] = [
        (Glyph::Play, "Play"),
        (Glyph::Pause, "Pause"),
        (Glyph::Stop, "Stop"),
        (Glyph::Restart, "Restart"),
        (Glyph::Slower, "Slower"),
        (Glyph::Faster, "Faster"),
    ];

    const BAR_H: i32 = 132;
    const BTN: i32 = 38;
    const GAP: i32 = 10;
    const TL_H: i32 = 26;

    struct Button {
        glyph: Glyph,
        rect: RECT,
        hot: bool,
        down: bool,
    }

    struct App {
        renderer: Renderer,
        host: HWND,
        panel: HWND,
        target: u32,
        actor: u32,

        buttons: Vec<Button>,
        tl_rect: RECT,
        /// 0..1 along the active sequence.
        tl_pos: f32,
        scrubbing: bool,

        /// Active sequence bounds in ms, and which one they came from.
        seq: (i32, i32),
        seq_index: i32,
        seq_name: String,
        seq_count: i32,

        font: HFONT,
        font_small: HFONT,
        last: Instant,
        closing: bool,
    }

    thread_local! {
        static APP: RefCell<Option<App>> = const { RefCell::new(None) };
    }

    pub fn run() {
        let model = std::env::args().nth(1).map(PathBuf::from);
        if let Err(e) = start(model) {
            eprintln!("error: {e}");
            // A message box too — a GUI launched from Explorer has no console.
            let msg: Vec<u16> = format!("{e}\0").encode_utf16().collect();
            unsafe {
                MessageBoxW(
                    std::ptr::null_mut(),
                    msg.as_ptr(),
                    w!("playback_gui"),
                    MB_ICONERROR,
                )
            };
            return;
        }
        pump();
    }

    fn start(model: Option<PathBuf>) -> Result<(), Box<dyn std::error::Error>> {
        let (host, panel) = unsafe { create_windows()? };

        let mut renderer = Renderer::new();
        if !renderer.use_bundled_shaders() {
            return Err("no bundled shader pack — rebuild with a backend feature".into());
        }
        let api = renderer
            .preferred_backend()
            .ok_or("no usable gfx backend in this build")?;
        renderer.pipeline().expect("live").init_device(api);
        if !renderer.pipeline().expect("live").is_device_ready() {
            return Err(format!("InitDevice failed for {api:?}").into());
        }
        println!("backend: {api:?}");
        renderer
            .settings()
            .expect("live")
            .set_background_color(20, 22, 26);

        let mut actor = 0u32;
        match &model {
            Some(path) => actor = load(&mut renderer, path)?,
            None => println!("no model given — the transport runs, there is nothing to watch"),
        }

        // Give the panel its real rect first: the swap chain is built at the
        // size it sees here, and a later resize does not re-aim the viewport.
        let (hw, hh) = unsafe { client_size(host) };
        let view_h = (hh - BAR_H).max(60);
        unsafe { MoveWindow(panel, 0, 0, hw, view_h, 1) };

        let (w, h) = unsafe { client_size(panel) };
        // SAFETY: the child window outlives the target — the renderer is torn
        // down in WM_CLOSE, before the windows are destroyed.
        let target = unsafe {
            renderer.pipeline().expect("live").create_swap_chain_target(
                panel as usize,
                w.max(1),
                h.max(1),
            )
        };
        if target == 0 {
            return Err("CreateSwapChainTarget failed".into());
        }
        renderer
            .pipeline()
            .expect("live")
            .set_primary_target(target);
        if let Some(mut cam) = renderer.camera_at(0) {
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            cam.set_distance(340.0);
            cam.set_pitch(0.28);
        }
        println!("viewport: target={target}");

        let (font, font_small) = unsafe { (ui_font(16), ui_font(13)) };
        let mut app = App {
            renderer,
            host,
            panel,
            target,
            actor,
            buttons: BUTTONS
                .iter()
                .map(|(g, _)| Button {
                    glyph: *g,
                    rect: rect(0, 0, 0, 0),
                    hot: false,
                    down: false,
                })
                .collect(),
            tl_rect: rect(0, 0, 0, 0),
            tl_pos: 0.0,
            scrubbing: false,
            seq: (0, 0),
            seq_index: -1,
            seq_name: String::new(),
            seq_count: 0,
            font,
            font_small,
            last: Instant::now(),
            closing: false,
        };
        refresh_sequence(&mut app);
        APP.with(|a| *a.borrow_mut() = Some(app));
        unsafe { layout(host) };
        Ok(())
    }

    fn load(
        renderer: &mut Renderer,
        path: &std::path::Path,
    ) -> Result<u32, Box<dyn std::error::Error>> {
        let dir = path.parent().unwrap_or(std::path::Path::new("."));
        renderer
            .scene()
            .expect("live")
            .set_pe1_base_path(&dir.to_string_lossy());

        // A .pkb / .pkfx is one corn effect with no model around it, so it
        // spawns through its own entry point rather than the model loader.
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let effect = ext == "pkb" || ext == "pkfx";

        let mut loader = renderer.loader().expect("live");
        let handle = if effect {
            loader.spawn_effect(&path.to_string_lossy())
        } else {
            loader.spawn_unit(&path.to_string_lossy())
        };
        drop(loader);
        if handle == 0 {
            let what = if effect { "SpawnEffect" } else { "SpawnUnit" };
            return Err(format!("{what} failed for {}", path.display()).into());
        }

        // SD and HD want different pipelines; the template knows which, and
        // getting it wrong mis-blends multi-layer materials. A bare effect has
        // no materials to judge from and its emitters are HD-path.
        let preferred = if effect {
            whiteoutflakes::RenderMode::HD
        } else {
            renderer
                .actor(handle)
                .expect("live")
                .preferred_render_mode()
        };
        renderer
            .settings()
            .expect("live")
            .set_render_mode(preferred);
        Ok(handle)
    }

    // ── Sequence / timeline ──────────────────────────────────────────────

    /// Re-read the active sequence's bounds. The cursor only means anything
    /// relative to them, so the timeline has to follow a sequence change.
    fn refresh_sequence(app: &mut App) {
        if app.actor == 0 {
            return;
        }
        let Some(actor) = app.renderer.actor(app.actor) else {
            return;
        };
        let idx = actor.active_sequence_index();
        let Some(list) = actor.sequences() else {
            return;
        };
        app.seq_count = list.len() as i32;
        if idx != app.seq_index {
            app.seq_index = idx;
            match list.get(idx.max(0) as usize) {
                Some(s) => {
                    app.seq_name = s.name();
                    app.seq = (s.start_ms(), s.end_ms());
                }
                None => {
                    app.seq_name = String::from("—");
                    app.seq = (0, 0);
                }
            }
        }
    }

    fn scrub_to(app: &mut App, frac: f32) {
        let frac = frac.clamp(0.0, 1.0);
        app.tl_pos = frac;
        let (lo, hi) = app.seq;
        if hi <= lo || app.actor == 0 {
            return;
        }
        let ms = lo + ((hi - lo) as f32 * frac) as i32;
        if let Some(mut actor) = app.renderer.actor(app.actor) {
            actor.set_animation_time_ms(ms);
            // Evaluate now rather than waiting for the next tick, so the pose
            // follows the drag even with playback paused.
            actor.evaluate_and_apply();
        }
    }

    // ── Frame ────────────────────────────────────────────────────────────

    fn frame(app: &mut App) {
        if app.closing {
            return;
        }
        let now = Instant::now();
        // Clamped: dragging the window stalls the loop, and a half-second dt
        // would teleport the animation on release.
        let dt = (now - app.last).as_secs_f32().min(0.1);
        app.last = now;

        // Both halves of the frame, and both obey the transport — with
        // playback paused this still runs, it just advances nothing.
        app.renderer.advance(dt);

        app.renderer
            .pipeline()
            .expect("live")
            .render_viewport(app.target, 0);
        app.renderer.pipeline().expect("live").present(app.target);

        refresh_sequence(app);
        if !app.scrubbing {
            let (lo, hi) = app.seq;
            if hi > lo {
                if let Some(actor) = app.renderer.actor(app.actor) {
                    let t = actor.animation_time_ms();
                    app.tl_pos = ((t - lo) as f32 / (hi - lo) as f32).clamp(0.0, 1.0);
                }
            }
        }
        // Only the bar repaints; the viewport is the swap chain's business.
        let bar = bar_rect(app.host);
        unsafe { InvalidateRect(app.host, &bar, 0) };
    }

    // ── Painting ─────────────────────────────────────────────────────────

    fn rect(l: i32, t: i32, r: i32, b: i32) -> RECT {
        RECT {
            left: l,
            top: t,
            right: r,
            bottom: b,
        }
    }

    fn bar_rect(host: HWND) -> RECT {
        let (w, h) = unsafe { client_size(host) };
        rect(0, (h - BAR_H).max(0), w, h)
    }

    unsafe fn fill(hdc: HDC, r: &RECT, color: u32) {
        unsafe {
            let brush = CreateSolidBrush(color);
            FillRect(hdc, r, brush);
            DeleteObject(brush as _);
        }
    }

    unsafe fn poly(hdc: HDC, pts: &[POINT], color: u32) {
        unsafe {
            let brush = CreateSolidBrush(color);
            let pen = CreatePen(PS_NULL, 0, 0);
            let ob = SelectObject(hdc, brush as _);
            let op = SelectObject(hdc, pen as _);
            Polygon(hdc, pts.as_ptr(), pts.len() as i32);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(brush as _);
            DeleteObject(pen as _);
        }
    }

    /// Vector glyphs, sized from the button rect so they scale with it.
    unsafe fn draw_glyph(hdc: HDC, g: Glyph, r: &RECT, color: u32) {
        let cx = (r.left + r.right) / 2;
        let cy = (r.top + r.bottom) / 2;
        let s = ((r.right - r.left).min(r.bottom - r.top) as f32 * 0.30) as i32;
        let p = |x: i32, y: i32| POINT { x, y };
        unsafe {
            match g {
                Glyph::Play => poly(
                    hdc,
                    &[p(cx - s / 2, cy - s), p(cx + s, cy), p(cx - s / 2, cy + s)],
                    color,
                ),
                Glyph::Pause => {
                    let w = (s * 2 / 3).max(2);
                    fill(hdc, &rect(cx - s, cy - s, cx - s + w, cy + s), color);
                    fill(hdc, &rect(cx + s - w, cy - s, cx + s, cy + s), color);
                }
                Glyph::Stop => fill(hdc, &rect(cx - s, cy - s, cx + s, cy + s), color),
                Glyph::Restart => {
                    // Skip-to-start: a bar with a triangle running back into
                    // it. A circular arrow needs curve detail this size does
                    // not have, and came out as a squiggle.
                    let w = (s / 3).max(2);
                    fill(hdc, &rect(cx - s, cy - s, cx - s + w, cy + s), color);
                    poly(
                        hdc,
                        &[p(cx + s, cy - s), p(cx + s, cy + s), p(cx - s + w + 1, cy)],
                        color,
                    );
                }
                Glyph::Slower | Glyph::Faster => {
                    let dir = if g == Glyph::Faster { 1 } else { -1 };
                    for k in 0..2 {
                        let ox = cx + dir * (k * s - s / 2);
                        poly(
                            hdc,
                            &[
                                p(ox - dir * s / 2, cy - s),
                                p(ox + dir * s / 2, cy),
                                p(ox - dir * s / 2, cy + s),
                            ],
                            color,
                        );
                    }
                }
            }
        }
    }

    unsafe fn text(hdc: HDC, s: &str, r: &RECT, color: u32, font: HFONT, flags: u32) {
        let mut wide: Vec<u16> = s.encode_utf16().collect();
        let mut rr = *r;
        unsafe {
            let of = SelectObject(hdc, font as _);
            SetTextColor(hdc, color);
            SetBkMode(hdc, TRANSPARENT as i32);
            DrawTextW(hdc, wide.as_mut_ptr(), wide.len() as i32, &mut rr, flags);
            SelectObject(hdc, of);
        }
    }

    fn paint(app: &mut App, hdc: HDC) {
        let bar = bar_rect(app.host);
        unsafe {
            fill(hdc, &bar, C_BG);

            let state = app
                .renderer
                .playback()
                .map(|p| p.state())
                .unwrap_or(PlaybackState::Playing);

            for (i, b) in app.buttons.iter().enumerate() {
                // The button matching the current state carries the accent, so
                // the transport reads at a glance without a label.
                let active = matches!(
                    (b.glyph, state),
                    (Glyph::Play, PlaybackState::Playing)
                        | (Glyph::Pause, PlaybackState::Paused)
                        | (Glyph::Stop, PlaybackState::Stopped)
                );
                let face = if b.down {
                    C_BTN_DOWN
                } else if b.hot {
                    C_BTN_HOT
                } else {
                    C_BTN
                };
                fill(hdc, &b.rect, face);
                if active {
                    // A 2px underline rather than a filled face: the glyph
                    // stays legible and the accent still reads.
                    fill(
                        hdc,
                        &rect(b.rect.left, b.rect.bottom - 2, b.rect.right, b.rect.bottom),
                        C_ACCENT,
                    );
                }
                draw_glyph(
                    hdc,
                    b.glyph,
                    &b.rect,
                    if active { C_ACCENT } else { C_GLYPH },
                );
                let _ = i;
            }

            // Timeline: track, filled portion, thumb.
            let t = app.tl_rect;
            let mid = (t.top + t.bottom) / 2;
            fill(hdc, &rect(t.left, mid - 3, t.right, mid + 3), C_TRACK);
            let x = t.left + ((t.right - t.left) as f32 * app.tl_pos) as i32;
            fill(hdc, &rect(t.left, mid - 3, x, mid + 3), C_ACCENT);
            let tr = if app.scrubbing { 8 } else { 6 };
            fill(hdc, &rect(x - tr, mid - tr, x + tr, mid + tr), C_GLYPH);

            // Two text rows under the timeline.
            let cursor = app
                .renderer
                .actor(app.actor)
                .map(|a| a.animation_time_ms())
                .unwrap_or(0);
            let scale = app
                .renderer
                .playback()
                .map(|p| p.time_scale())
                .unwrap_or(1.0);
            let (geosets, pe2, corn) = app
                .renderer
                .pipeline()
                .and_then(|p| p.frame_stats())
                .map(|s| (s.geosets(), s.particles(), s.corn_particles()))
                .unwrap_or((0, 0, 0));

            let row = rect(t.left, t.bottom + 6, t.right, t.bottom + 26);
            let left = format!(
                "{}   [{}/{}]   {} / {} ms",
                app.seq_name,
                app.seq_index.max(0),
                app.seq_count.max(1) - 1,
                cursor,
                app.seq.1
            );
            text(
                hdc,
                &left,
                &row,
                C_TEXT,
                app.font_small,
                DT_LEFT | DT_SINGLELINE,
            );

            // The corn-effects count is what shows the transport reaching the
            // effects: held across a pause, dropped by a stop.
            let right = format!(
                "{state:?}   x{scale:.2}   geosets {geosets}   PE2 {pe2}   corn effects {corn}"
            );
            text(
                hdc,
                &right,
                &row,
                C_TEXT_DIM,
                app.font_small,
                DT_RIGHT | DT_SINGLELINE,
            );
        }
    }

    // ── Win32 ────────────────────────────────────────────────────────────

    unsafe fn client_size(hwnd: HWND) -> (i32, i32) {
        let mut r = rect(0, 0, 0, 0);
        unsafe { GetClientRect(hwnd, &mut r) };
        (r.right - r.left, r.bottom - r.top)
    }

    /// The shell's UI font at a given pixel height.
    unsafe fn ui_font(px: i32) -> HFONT {
        unsafe {
            let mut lf: LOGFONTW = std::mem::zeroed();
            let mut ncm: NONCLIENTMETRICSW = std::mem::zeroed();
            ncm.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
            if SystemParametersInfoW(
                SPI_GETNONCLIENTMETRICS,
                ncm.cbSize,
                (&mut ncm as *mut NONCLIENTMETRICSW).cast(),
                0,
            ) != 0
            {
                lf = ncm.lfMessageFont;
            }
            lf.lfHeight = -px;
            let f = CreateFontIndirectW(&lf);
            if f.is_null() {
                GetStockObject(DEFAULT_GUI_FONT) as HFONT
            } else {
                f
            }
        }
    }

    unsafe fn create_windows() -> Result<(HWND, HWND), Box<dyn std::error::Error>> {
        let hinst = unsafe { GetModuleHandleW(std::ptr::null()) };

        // The viewport class paints nothing: the swap chain presents into it,
        // so a background brush would only fight the renderer.
        let panel_class = WNDCLASSW {
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(DefWindowProcW),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: hinst,
            hIcon: std::ptr::null_mut(),
            hCursor: unsafe { LoadCursorW(std::ptr::null_mut(), IDC_ARROW) },
            hbrBackground: std::ptr::null_mut(),
            lpszMenuName: std::ptr::null(),
            lpszClassName: w!("WfsPanel"),
        };
        unsafe { RegisterClassW(&panel_class) };

        let host_class = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            lpszClassName: w!("WfsPlaybackHost"),
            ..panel_class
        };
        unsafe { RegisterClassW(&host_class) };

        let host = unsafe {
            CreateWindowExW(
                0,
                w!("WfsPlaybackHost"),
                w!("WhiteoutFlakes - scene transport"),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1024,
                720,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                hinst,
                std::ptr::null(),
            )
        };
        if host.is_null() {
            return Err("CreateWindowExW failed for the host window".into());
        }
        let panel = unsafe {
            CreateWindowExW(
                0,
                w!("WfsPanel"),
                w!(""),
                WS_CHILD | WS_VISIBLE,
                0,
                0,
                10,
                10,
                host,
                std::ptr::null_mut(),
                hinst,
                std::ptr::null(),
            )
        };
        unsafe { ShowWindow(host, SW_SHOW) };
        Ok((host, panel))
    }

    /// Viewport on top, transport bar beneath. Hit rects live in `App`
    /// because the bar is drawn, not built from controls.
    unsafe fn layout(host: HWND) {
        let (w, h) = unsafe { client_size(host) };
        if w <= 0 || h <= 0 {
            return;
        }
        let view_h = (h - BAR_H).max(60);
        APP.with(|a| {
            let mut guard = a.borrow_mut();
            let Some(app) = guard.as_mut() else {
                return;
            };
            unsafe { MoveWindow(app.panel, 0, 0, w, view_h, 1) };

            let y = view_h + GAP + 4;
            for (i, b) in app.buttons.iter_mut().enumerate() {
                let x = GAP + i as i32 * (BTN + 6);
                b.rect = rect(x, y, x + BTN, y + BTN);
            }
            let tl_top = y + BTN + GAP + 2;
            app.tl_rect = rect(GAP + 8, tl_top, (w - GAP - 8).max(GAP + 40), tl_top + TL_H);

            if app.closing {
                return;
            }
            let (pw, ph) = unsafe { client_size(app.panel) };
            let mut pipeline = app.renderer.pipeline().expect("live");
            pipeline.set_primary_target(app.target);
            pipeline.resize_primary_target(pw.max(1), ph.max(1));
        });
    }

    fn hit_button(app: &App, x: i32, y: i32) -> Option<usize> {
        app.buttons.iter().position(|b| {
            x >= b.rect.left && x < b.rect.right && y >= b.rect.top && y < b.rect.bottom
        })
    }

    fn on_button(app: &mut App, g: Glyph) {
        let Some(mut p) = app.renderer.playback() else {
            return;
        };
        match g {
            Glyph::Play => p.play(),
            Glyph::Pause => p.pause(),
            Glyph::Stop => p.stop(),
            Glyph::Restart => p.restart(),
            // Time scale is independent of the transport, so slow motion
            // survives a pause and resume.
            Glyph::Slower => {
                let s = (p.time_scale() * 0.5).max(0.05);
                p.set_time_scale(s);
            }
            Glyph::Faster => {
                let s = (p.time_scale() * 2.0).min(8.0);
                p.set_time_scale(s);
            }
        }
    }

    unsafe extern "system" fn wndproc(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        let mx = (lparam & 0xFFFF) as i16 as i32;
        let my = ((lparam >> 16) & 0xFFFF) as i16 as i32;
        match msg {
            WM_ERASEBKGND => 1, // WM_PAINT covers the bar; the panel is the swap chain's.
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = unsafe { std::mem::zeroed() };
                let hdc = unsafe { BeginPaint(hwnd, &mut ps) };
                APP.with(|a| {
                    if let Some(app) = a.borrow_mut().as_mut() {
                        // Draw into a bitmap and blit it in one go. Painting
                        // the bar directly meant every frame cleared it and
                        // redrew, which the eye sees as flicker.
                        let bar = bar_rect(app.host);
                        let (bw, bh) = (bar.right - bar.left, bar.bottom - bar.top);
                        unsafe {
                            let mem = CreateCompatibleDC(hdc);
                            let bmp = CreateCompatibleBitmap(hdc, bw, bh);
                            let old = SelectObject(mem, bmp as _);
                            // The backing bitmap is bar-sized and origin-based,
                            // so shift drawing up by the bar's top.
                            SetViewportOrgEx(mem, 0, -bar.top, std::ptr::null_mut());
                            paint(app, mem);
                            SetViewportOrgEx(mem, 0, 0, std::ptr::null_mut());
                            BitBlt(hdc, bar.left, bar.top, bw, bh, mem, 0, 0, SRCCOPY);
                            SelectObject(mem, old);
                            DeleteObject(bmp as _);
                            DeleteDC(mem);
                        }
                    }
                });
                unsafe { EndPaint(hwnd, &ps) };
                0
            }
            WM_LBUTTONDOWN => {
                APP.with(|a| {
                    let mut guard = a.borrow_mut();
                    let Some(app) = guard.as_mut() else {
                        return;
                    };
                    if let Some(i) = hit_button(app, mx, my) {
                        app.buttons[i].down = true;
                        unsafe { SetCapture(hwnd) };
                    } else if my >= app.tl_rect.top - 6 && my <= app.tl_rect.bottom + 6 {
                        app.scrubbing = true;
                        unsafe { SetCapture(hwnd) };
                        let w = (app.tl_rect.right - app.tl_rect.left).max(1);
                        let f = (mx - app.tl_rect.left) as f32 / w as f32;
                        scrub_to(app, f);
                    }
                });
                0
            }
            WM_MOUSEMOVE => {
                APP.with(|a| {
                    let mut guard = a.borrow_mut();
                    let Some(app) = guard.as_mut() else {
                        return;
                    };
                    if app.scrubbing {
                        let w = (app.tl_rect.right - app.tl_rect.left).max(1);
                        let f = (mx - app.tl_rect.left) as f32 / w as f32;
                        scrub_to(app, f);
                        return;
                    }
                    let hot = hit_button(app, mx, my);
                    for (i, b) in app.buttons.iter_mut().enumerate() {
                        b.hot = Some(i) == hot;
                    }
                });
                0
            }
            WM_LBUTTONUP => {
                APP.with(|a| {
                    let mut guard = a.borrow_mut();
                    let Some(app) = guard.as_mut() else {
                        return;
                    };
                    unsafe { ReleaseCapture() };
                    if app.scrubbing {
                        app.scrubbing = false;
                        // Particles and corn effects are forward simulations with
                        // no seek. Without this they keep whatever state they
                        // had before the drag, which reads as the effects
                        // ignoring the timeline.
                        if let Some(mut p) = app.renderer.playback() {
                            p.resync_effects();
                        }
                        return;
                    }
                    // Fire only if the release lands on the button that was
                    // pressed, the usual click contract.
                    let released = hit_button(app, mx, my);
                    let pressed = app.buttons.iter().position(|b| b.down);
                    for b in app.buttons.iter_mut() {
                        b.down = false;
                    }
                    if let (Some(p), Some(r)) = (pressed, released) {
                        if p == r {
                            let g = app.buttons[p].glyph;
                            on_button(app, g);
                        }
                    }
                });
                0
            }
            WM_SIZE => {
                unsafe { layout(hwnd) };
                0
            }
            WM_CLOSE => {
                // Tear the renderer down while the child window is still
                // alive: the swap chain holds the panel's HWND.
                APP.with(|a| {
                    if let Some(app) = a.borrow_mut().as_mut() {
                        app.closing = true;
                        let mut pipeline = app.renderer.pipeline().expect("live");
                        if pipeline.is_device_ready() {
                            pipeline.shutdown();
                        }
                        unsafe {
                            DeleteObject(app.font as _);
                            DeleteObject(app.font_small as _);
                        }
                    }
                    *a.borrow_mut() = None;
                });
                unsafe { DestroyWindow(hwnd) };
                0
            }
            WM_DESTROY => {
                unsafe { PostQuitMessage(0) };
                0
            }
            _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
        }
    }

    /// Render whenever the queue is empty — the scene animates with or
    /// without input, so waiting on messages would stall it.
    fn pump() {
        let mut msg = unsafe { std::mem::zeroed::<MSG>() };
        loop {
            unsafe {
                while PeekMessageW(&mut msg, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
                    if msg.message == WM_QUIT {
                        return;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            let alive = APP.with(|a| match a.borrow_mut().as_mut() {
                Some(app) => {
                    frame(app);
                    true
                }
                None => false,
            });
            if !alive {
                unsafe {
                    while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                return;
            }
        }
    }
}
