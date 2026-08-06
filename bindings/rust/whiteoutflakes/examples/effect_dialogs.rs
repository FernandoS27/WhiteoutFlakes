// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Open and Save dialogs for corn effects, and a host to try them in.
//
//   cargo run --example effect_dialogs -- path/to/effects
//   cargo run --example effect_dialogs                     (starts in the cwd)
//
// The dialogs themselves are `common/effect_dialog.rs` — a widget, not part of
// this example: `effect_dialog::open` and `effect_dialog::save` each put up a
// modal window and hand back a path. This file is what a host looks like
// around them: a viewport, two buttons, and somewhere to put the answer.
//
// Both dialogs list effects and nothing else (`StorageFileFilter::EffectsOnly`
// over a `StorageBrowser`), shown as a grid of tiles like the storage
// explorer's — every tile a live thumbnail of the effect itself, rendered
// through `readback_target`.
//
// Save is a real save, not a mock: it copies the open effect to the chosen
// path. There is no effect *authoring* in this API, so writing the file the
// user picked is the honest thing for the host to do with the answer — and it
// means the new file turns up, with a thumbnail, next time the dialog opens.
//
// Windows-only, like the other GUI examples.

#[cfg(not(windows))]
fn main() {
    eprintln!("examples/effect_dialogs is Windows-only.");
}

#[cfg(windows)]
fn main() {
    imp::run();
}

// At file scope, not inside `imp`: a `#[path]` module declared inside another
// module is looked up under a directory named after it.
#[cfg(windows)]
#[path = "common/effect_dialog.rs"]
mod effect_dialog;
#[cfg(windows)]
#[path = "common/theme.rs"]
mod theme;
#[cfg(windows)]
#[path = "common/thumbs.rs"]
mod thumbs;

#[cfg(windows)]
mod imp {
    use std::cell::RefCell;
    use std::time::Instant;

    use windows_sys::core::w;
    use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::Graphics::Gdi::*;
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::Controls::DRAWITEMSTRUCT;
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    use whiteoutflakes::Renderer;

    use crate::effect_dialog;
    use crate::theme::*;
    use crate::thumbs::{self, Thumbs};

    const ID_OPEN: usize = 1;
    const ID_SAVE: usize = 2;
    const ID_STATUS: usize = 3;
    const ID_PANEL: usize = 200;

    const BAR_H: i32 = 44;
    const STATUS_H: i32 = 22;

    struct App {
        renderer: Renderer,
        thumbs: Option<Thumbs>,
        root: String,
        /// What is loaded in the viewport — the thing Save copies.
        current: Option<String>,
        host: HWND,
        status: HWND,
        panel: HWND,
        target: u32,
        font: HFONT,
        last: Instant,
        closing: bool,
    }

    thread_local! {
        static APP: RefCell<Option<App>> = const { RefCell::new(None) };
    }

    pub fn run() {
        let root = std::env::args().nth(1).unwrap_or_else(|| ".".to_string());
        if let Err(e) = start(&root) {
            eprintln!("error: {e}");
            let msg: Vec<u16> = format!("{e}\0").encode_utf16().collect();
            unsafe {
                MessageBoxW(
                    std::ptr::null_mut(),
                    msg.as_ptr(),
                    w!("effect_dialogs"),
                    MB_ICONERROR,
                )
            };
            return;
        }
        pump();
    }

    fn start(root: &str) -> Result<(), Box<dyn std::error::Error>> {
        let root = std::fs::canonicalize(root)
            .map(|p| {
                // Strip the \\?\ prefix canonicalize adds; it confuses nothing
                // here but reads badly in a title bar.
                let s = p.to_string_lossy().into_owned();
                s.strip_prefix(r"\\?\").map(str::to_string).unwrap_or(s)
            })
            .unwrap_or_else(|_| root.to_string());

        let host = unsafe { create_windows()? };

        let mut renderer = Renderer::new();
        if !renderer.use_bundled_shaders() {
            return Err("no bundled shader pack".into());
        }
        let api = renderer
            .preferred_backend()
            .ok_or("no usable gfx backend")?;
        renderer.pipeline().expect("live").init_device(api);
        if !renderer.pipeline().expect("live").is_device_ready() {
            return Err(format!("InitDevice failed for {api:?}").into());
        }
        renderer
            .settings()
            .expect("live")
            .set_background_color(24, 26, 32);
        thumbs::hide_grid(&mut renderer);

        let (w, h) = unsafe { client_size(host.panel) };
        // SAFETY: the child window outlives the target — the renderer is torn
        // down in WM_CLOSE, before the windows are destroyed.
        let target = unsafe {
            renderer.pipeline().expect("live").create_swap_chain_target(
                host.panel as usize,
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

        // The dialogs share one thumbnail renderer, so the device cost is paid
        // once for the process rather than once per dialog. Rendered at the
        // dialog's tile size, so the pictures are the size they are drawn at.
        let mut thumbs = Thumbs::with_size(api, effect_dialog::TILE);
        match thumbs.as_mut() {
            // Tiles animate: each effect is recorded as a short loop rather
            // than one picture.
            Some(t) => t.set_frames(effect_dialog::FRAMES),
            None => eprintln!("warning: no thumbnail renderer — dialog tiles will be text only"),
        }

        let app = App {
            renderer,
            thumbs,
            root,
            current: None,
            host: host.host,
            status: host.status,
            panel: host.panel,
            target,
            font: host.font,
            last: Instant::now(),
            closing: false,
        };
        APP.with(|a| *a.borrow_mut() = Some(app));
        APP.with(|a| {
            if let Some(app) = a.borrow_mut().as_mut() {
                set_status(app, "no effect open");
            }
        });
        Ok(())
    }

    fn set_status(app: &App, text: &str) {
        let wide: Vec<u16> = format!("{text}\0").encode_utf16().collect();
        unsafe { SetWindowTextW(app.status, wide.as_ptr()) };
    }

    // ── The two dialogs ──────────────────────────────────────────────────

    /// Put up a dialog with the app's borrow released.
    ///
    /// A modal dialog runs its own message pump, and those messages come back
    /// through this window — so holding the `RefCell` across it would panic
    /// the first time the host repainted. The thumbnail renderer is lent to
    /// the dialog and handed back afterwards.
    fn lend() -> Option<(Thumbs, String, HWND, Option<String>)> {
        APP.with(|a| {
            let mut guard = a.borrow_mut();
            let app = guard.as_mut()?;
            Some((
                app.thumbs.take()?,
                app.root.clone(),
                app.host,
                app.current.clone(),
            ))
        })
    }

    fn give_back(thumbs: Thumbs) {
        APP.with(|a| {
            if let Some(app) = a.borrow_mut().as_mut() {
                app.thumbs = Some(thumbs);
            }
        });
    }

    /// Run `f` against the app once the dialog is gone and the borrow is free.
    fn after<F: FnOnce(&mut App)>(f: F) {
        APP.with(|a| {
            if let Some(app) = a.borrow_mut().as_mut() {
                f(app);
            }
        });
    }

    fn on_open() {
        let Some((mut thumbs, root, host, _)) = lend() else {
            return;
        };
        let picked = effect_dialog::open(host, &root, &mut thumbs);
        give_back(thumbs);
        if let Some(path) = picked {
            after(|app| load(app, path));
        }
    }

    fn on_save() {
        let Some((mut thumbs, root, host, current)) = lend() else {
            return;
        };
        let Some(source) = current else {
            give_back(thumbs);
            after(|app| set_status(app, "open an effect first — there is nothing to save"));
            return;
        };
        let suggested = std::path::Path::new(&source)
            .file_stem()
            .map(|s| format!("{}_copy", s.to_string_lossy()))
            .unwrap_or_default();
        let picked = effect_dialog::save(host, &root, &mut thumbs, &suggested);
        give_back(thumbs);
        let Some(target) = picked else {
            return;
        };

        // The dialog already confirmed an overwrite, so this is allowed to
        // clobber. It is a copy because the API has no effect authoring to
        // save *from* — the file the user picked is the deliverable.
        let note = match std::fs::copy(&source, &target) {
            Ok(n) => {
                println!("wrote {target} ({n} bytes)");
                format!("saved {target}")
            }
            Err(e) => format!("could not write {target}: {e}"),
        };
        after(|app| set_status(app, &note));
    }

    fn load(app: &mut App, path: String) {
        app.renderer.loader().expect("live").request_clear_all();
        if let Some(dir) = std::path::Path::new(&path).parent() {
            app.renderer
                .scene()
                .expect("live")
                .set_pe1_base_path(&dir.to_string_lossy());
        }
        let handle = app.renderer.loader().expect("live").spawn_effect(&path);
        if handle == 0 {
            set_status(app, &format!("could not load {path}"));
            return;
        }
        app.renderer
            .settings()
            .expect("live")
            .set_render_mode(whiteoutflakes::RenderMode::HD);
        if let Some(mut cam) = app.renderer.camera_at(0) {
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            cam.set_distance(260.0);
            cam.set_pitch(0.25);
        }
        if let Some(mut p) = app.renderer.playback() {
            p.restart();
        }
        set_status(app, &path);
        app.current = Some(path);
    }

    // ── Frame ────────────────────────────────────────────────────────────

    fn frame(app: &mut App) {
        if app.closing {
            return;
        }
        let now = Instant::now();
        let dt = (now - app.last).as_secs_f32().min(0.1);
        app.last = now;
        app.renderer.advance(dt);
        let mut pipeline = app.renderer.pipeline().expect("live");
        pipeline.render_viewport(app.target, 0);
        pipeline.present(app.target);
    }

    // ── Win32 ────────────────────────────────────────────────────────────

    struct Host {
        host: HWND,
        status: HWND,
        panel: HWND,
        font: HFONT,
    }

    unsafe fn create_windows() -> Result<Host, Box<dyn std::error::Error>> {
        let hinst = unsafe { GetModuleHandleW(std::ptr::null()) };

        // The viewport paints nothing: the swap chain presents into it.
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
            lpszClassName: w!("WfsEffectPanel"),
        };
        unsafe { RegisterClassW(&panel_class) };
        let host_class = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            lpszClassName: w!("WfsEffectHost"),
            hbrBackground: unsafe { CreateSolidBrush(C_BG) },
            ..panel_class
        };
        unsafe { RegisterClassW(&host_class) };

        let host = unsafe {
            CreateWindowExW(
                0,
                w!("WfsEffectHost"),
                w!("WhiteoutFlakes - effect dialogs"),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                980,
                660,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                hinst,
                std::ptr::null(),
            )
        };
        if host.is_null() {
            return Err("CreateWindowExW failed".into());
        }

        let child = |class: *const u16, text: *const u16, style: u32, id: usize| -> HWND {
            unsafe {
                CreateWindowExW(
                    0,
                    class,
                    text,
                    WS_CHILD | WS_VISIBLE | style,
                    0,
                    0,
                    10,
                    10,
                    host,
                    id as _,
                    hinst,
                    std::ptr::null(),
                )
            }
        };

        let open = child(w!("BUTTON"), w!("Open effect..."), OWNERDRAW, ID_OPEN);
        let save = child(w!("BUTTON"), w!("Save a copy..."), OWNERDRAW, ID_SAVE);
        let status = child(w!("STATIC"), w!(""), 0, ID_STATUS);
        let panel = child(w!("WfsEffectPanel"), w!(""), 0, ID_PANEL);

        let font = unsafe { ui_font() };
        for h in [open, save, status] {
            unsafe { SendMessageW(h, WM_SETFONT, font as WPARAM, 1) };
        }

        unsafe {
            layout(host);
            ShowWindow(host, SW_SHOW);
        }
        Ok(Host {
            host,
            status,
            panel,
            font,
        })
    }

    /// Buttons along the top, status along the bottom, viewport between.
    unsafe fn layout(host: HWND) {
        let (w, h) = unsafe { client_size(host) };
        if w <= 0 || h <= 0 {
            return;
        }
        let g = 10;
        let place = |id: usize, x: i32, y: i32, cw: i32, ch: i32| {
            let c = unsafe { GetDlgItem(host, id as i32) };
            if !c.is_null() {
                unsafe { MoveWindow(c, x, y, cw.max(1), ch.max(1), 1) };
            }
        };
        place(ID_OPEN, g, g, 130, 28);
        place(ID_SAVE, g + 140, g, 130, 28);
        place(ID_STATUS, g, h - STATUS_H, w - g * 2, STATUS_H - 2);
        place(ID_PANEL, 0, BAR_H, w, h - BAR_H - STATUS_H);

        APP.with(|a| {
            let mut guard = a.borrow_mut();
            let Some(app) = guard.as_mut() else {
                return;
            };
            if app.closing {
                return;
            }
            let (pw, ph) = unsafe { client_size(app.panel) };
            let mut pipeline = app.renderer.pipeline().expect("live");
            pipeline.set_primary_target(app.target);
            pipeline.resize_primary_target(pw.max(1), ph.max(1));
        });
    }

    unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        match msg {
            WM_COMMAND => {
                // Only a click. An owner-drawn button also notifies when it
                // gains or loses focus, and the dialog closing hands focus
                // back here — which would re-open it on the way out.
                if ((wp >> 16) & 0xFFFF) as u32 != BN_CLICKED {
                    return 0;
                }
                // A dialog runs its own message pump, so the borrow has to be
                // released before it opens — otherwise the host's frame tick
                // would re-enter it.
                match wp & 0xFFFF {
                    ID_OPEN => on_open(),
                    ID_SAVE => on_save(),
                    _ => {}
                }
                0
            }
            WM_DRAWITEM => {
                unsafe { draw_button(&*(lp as *const DRAWITEMSTRUCT)) };
                1
            }
            WM_CTLCOLORSTATIC | WM_CTLCOLORBTN => unsafe { ctl_colour(msg, wp) },
            WM_SIZE => {
                unsafe { layout(hwnd) };
                0
            }
            WM_CLOSE => {
                APP.with(|a| {
                    if let Some(app) = a.borrow_mut().as_mut() {
                        app.closing = true;
                        let mut pipeline = app.renderer.pipeline().expect("live");
                        if pipeline.is_device_ready() {
                            pipeline.shutdown();
                        }
                        if let Some(t) = app.thumbs.as_mut() {
                            t.shutdown();
                        }
                        unsafe { DeleteObject(app.font as _) };
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
            _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
        }
    }

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
