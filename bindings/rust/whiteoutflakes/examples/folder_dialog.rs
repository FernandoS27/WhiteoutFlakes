// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// `StorageBrowser` used the way it is meant to be used: as an open dialog.
// A folder on the left, the thing you picked rendering on the right.
//
//   cargo run --example folder_dialog -- path/to/models
//   cargo run --example folder_dialog                  (starts in the cwd)
//
// Folder mode specifically — `StorageKind::Folder` walks a directory and its
// subdirectories, and the paths it hands back are absolute, so nothing has to
// be told where the storage is. The same code drives a CASC install or an MPQ
// by swapping the `open` call; `examples/storage_browse` shows all three from
// the command line.
//
// The list is one control holding both folders and files: `..` and folder
// rows navigate, file rows load. That is what makes it a dialog rather than
// two panes to keep in sync — the browser already models exactly this.
//
// Every file row carries a live thumbnail of the thing itself, from
// `common/thumbs.rs`. For the same widget as a *modal* Open/Save dialog, see
// `examples/effect_dialogs`.
//
// Windows-only, like the other GUI examples.

#[cfg(not(windows))]
fn main() {
    eprintln!("examples/folder_dialog is Windows-only.");
}

#[cfg(windows)]
fn main() {
    imp::run();
}

// At file scope, not inside `imp`: a `#[path]` module declared inside another
// module is looked up under a directory named after it.
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
    use windows_sys::Win32::UI::Controls::{DRAWITEMSTRUCT, ODT_LISTBOX};
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    use whiteoutflakes::{Renderer, StorageBrowser, StorageFileFilter, StorageKind};

    use crate::theme::*;
    use crate::thumbs::{self, Thumbs, ROW_H};

    const ID_LIST: usize = 1;
    const ID_FILTER: usize = 2;
    const ID_UP: usize = 3;
    const ID_STATUS: usize = 100;
    const ID_PANEL: usize = 200;

    const LIST_W: i32 = 330;
    const BAR_H: i32 = 34;
    const STATUS_H: i32 = 22;

    // Listbox messages and styles windows-sys does not re-export.
    const LB_ADDSTRING: u32 = 0x0180;
    const LB_RESETCONTENT: u32 = 0x0184;
    const LB_GETCURSEL: u32 = 0x0188;
    const LB_SETITEMHEIGHT: u32 = 0x01A0;
    const LBS_NOTIFY: u32 = 0x0001;
    const LBS_OWNERDRAWFIXED: u32 = 0x0010;
    const LBS_HASSTRINGS: u32 = 0x0040;
    const LBN_SELCHANGE: u16 = 1;
    const LBN_DBLCLK: u16 = 2;

    /// One row in the list. The browser gives names; this remembers what a
    /// row means so a click does not have to re-derive it — and, for files,
    /// what to key the thumbnail cache on.
    enum Row {
        Up,
        Folder(String),
        File {
            name: String,
            path: String,
            effect: bool,
        },
    }

    struct App {
        renderer: Renderer,
        browser: StorageBrowser,
        rows: Vec<Row>,
        thumbs: Option<Thumbs>,
        /// Set while a directory's thumbnails are still arriving, so the tally
        /// is reported once when they finish rather than on every frame.
        counting: bool,
        list: HWND,
        filter_btn: HWND,
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
                    w!("folder_dialog"),
                    MB_ICONERROR,
                )
            };
            return;
        }
        pump();
    }

    fn start(root: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut browser = StorageBrowser::new();
        // Folder mode explicitly — `open_auto` would pick CASC if the user
        // happened to point at a game install, and this example is about the
        // directory walk.
        if !browser.open(root, StorageKind::Folder) {
            return Err(format!("{}: {}", root, browser.last_error()).into());
        }
        println!("opened {} as {:?}", browser.root(), browser.kind());

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

        // Thumbnails are a nicety, not a requirement: if the second device
        // will not come up the dialog still browses and previews.
        let thumbs = Thumbs::new(api);
        if thumbs.is_none() {
            eprintln!("warning: no thumbnail renderer — rows will be text only");
        }

        let mut app = App {
            renderer,
            browser,
            rows: Vec::new(),
            thumbs,
            counting: false,
            list: host.list,
            filter_btn: host.filter_btn,
            status: host.status,
            panel: host.panel,
            target,
            font: host.font,
            last: Instant::now(),
            closing: false,
        };
        refill(&mut app);
        APP.with(|a| *a.borrow_mut() = Some(app));
        Ok(())
    }

    // ── Browsing ─────────────────────────────────────────────────────────

    /// Rebuild the list from the browser's current directory, and ask for a
    /// thumbnail for every file in it. The cache is kept across directories,
    /// so walking back into one is instant.
    fn refill(app: &mut App) {
        app.rows.clear();
        unsafe { SendMessageW(app.list, LB_RESETCONTENT, 0, 0) };
        if let Some(t) = app.thumbs.as_mut() {
            // Leaving a directory drops what has not been drawn yet, so the
            // new one is not stuck behind the old one's backlog.
            t.forget_queue();
        }

        let add = |text: &str| {
            let wide: Vec<u16> = format!("{text}\0").encode_utf16().collect();
            unsafe { SendMessageW(app.list, LB_ADDSTRING, 0, wide.as_ptr() as isize) };
        };

        if !app.browser.current_path().is_empty() {
            add("..");
            app.rows.push(Row::Up);
        }
        for f in app.browser.folders() {
            add(&f);
            app.rows.push(Row::Folder(f));
        }
        for name in app.browser.files() {
            add(&name);
            let path = app.browser.child_path(&name);
            let effect = app.browser.is_effect(&name);
            if let Some(t) = app.thumbs.as_mut() {
                t.request(&path, effect);
            }
            app.rows.push(Row::File { name, path, effect });
        }
        app.counting = app.thumbs.as_ref().is_some_and(|t| t.queued() > 0);
        set_status(app, None);
    }

    fn set_status(app: &App, extra: Option<&str>) {
        let here = app.browser.current_path();
        let shown = app.browser.files().len();
        let total = app.browser.unfiltered_file_count();
        let mut s = format!(
            "{}  —  {} shown{}",
            if here.is_empty() { "<root>" } else { &here },
            shown,
            if shown as i32 == total {
                String::new()
            } else {
                format!(" of {total}")
            }
        );
        if let Some(e) = extra {
            s.push_str("   ");
            s.push_str(e);
        }
        s.push('\0');
        let wide: Vec<u16> = s.encode_utf16().collect();
        unsafe { SetWindowTextW(app.status, wide.as_ptr()) };
    }

    fn selected(app: &App) -> Option<usize> {
        let sel = unsafe { SendMessageW(app.list, LB_GETCURSEL, 0, 0) };
        (sel >= 0 && (sel as usize) < app.rows.len()).then_some(sel as usize)
    }

    /// Act on the selected row: navigate, or load.
    fn activate(app: &mut App) {
        let Some(row) = selected(app).and_then(|i| app.rows.get(i)) else {
            return;
        };
        match row {
            Row::Up => {
                app.browser.ascend();
                refill(app);
            }
            Row::Folder(name) => {
                let name = name.clone();
                app.browser.descend(&name);
                refill(app);
            }
            Row::File { name, .. } => {
                let name = name.clone();
                load(app, &name);
            }
        }
    }

    /// Preview whatever the selection landed on. Folders do nothing until
    /// they are opened, so arrowing through the list previews files as it
    /// passes them.
    fn preview_selection(app: &mut App) {
        if let Some(Row::File { name, .. }) = selected(app).and_then(|i| app.rows.get(i)) {
            let name = name.clone();
            load(app, &name);
        }
    }

    fn load(app: &mut App, file: &str) {
        // The browser hands back what a provider reads. In folder mode that
        // is an absolute path, so nothing else has to be configured — only
        // the base path, for textures and child models sitting beside it.
        let path = app.browser.child_path(file);
        let is_effect = app.browser.is_effect(file);
        if path.is_empty() {
            return;
        }

        app.renderer.loader().expect("live").request_clear_all();
        if let Some(dir) = std::path::Path::new(&path).parent() {
            app.renderer
                .scene()
                .expect("live")
                .set_pe1_base_path(&dir.to_string_lossy());
        }

        let mut loader = app.renderer.loader().expect("live");
        let handle = if is_effect {
            loader.spawn_effect(&path)
        } else {
            loader.spawn_unit(&path)
        };
        drop(loader);
        if handle == 0 {
            set_status(app, Some("load failed"));
            return;
        }

        let mode = if is_effect {
            whiteoutflakes::RenderMode::HD
        } else {
            app.renderer
                .actor(handle)
                .expect("live")
                .preferred_render_mode()
        };
        app.renderer.settings().expect("live").set_render_mode(mode);

        if let Some(mut cam) = app.renderer.camera_at(0) {
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            cam.set_distance(if is_effect { 260.0 } else { 400.0 });
            cam.set_pitch(0.25);
        }
        // Replay from the top so an effect's one-shot burst is not missed.
        if let Some(mut p) = app.renderer.playback() {
            p.restart();
        }
        set_status(app, Some(&format!("loaded {file}")));
    }

    fn cycle_filter(app: &mut App) {
        let next = match app.browser.filter() {
            StorageFileFilter::All => StorageFileFilter::ModelsOnly,
            StorageFileFilter::ModelsOnly => StorageFileFilter::EffectsOnly,
            StorageFileFilter::EffectsOnly => StorageFileFilter::All,
        };
        app.browser.set_filter(next);
        let label = match next {
            StorageFileFilter::All => "Filter: All\0",
            StorageFileFilter::ModelsOnly => "Filter: Models\0",
            StorageFileFilter::EffectsOnly => "Filter: Effects\0",
        };
        let wide: Vec<u16> = label.encode_utf16().collect();
        unsafe { SetWindowTextW(app.filter_btn, wide.as_ptr()) };
        refill(app);
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
        drop(pipeline);

        let Some(t) = app.thumbs.as_mut() else {
            return;
        };
        if t.tick().is_none() {
            return;
        }
        unsafe { InvalidateRect(app.list, std::ptr::null(), 0) };
        if app.counting && app.thumbs.as_ref().is_some_and(|t| t.queued() == 0) {
            app.counting = false;
            report_thumbnails(app);
        }
    }

    fn report_thumbnails(app: &App) {
        let Some(t) = app.thumbs.as_ref() else {
            return;
        };
        let (ok, blank, bad) = t.tally(app.rows.iter().filter_map(|r| match r {
            Row::File { path, .. } => Some(path.as_str()),
            _ => None,
        }));
        let mut note = format!("{ok} thumbnails");
        if blank > 0 {
            note.push_str(&format!(", {blank} blank"));
        }
        if bad > 0 {
            note.push_str(&format!(", {bad} failed"));
        }
        println!("{}: {note}", app.browser.current_path());
        set_status(app, Some(&note));
    }

    // ── Win32 ────────────────────────────────────────────────────────────

    struct Host {
        list: HWND,
        filter_btn: HWND,
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
            lpszClassName: w!("WfsDlgPanel"),
        };
        unsafe { RegisterClassW(&panel_class) };
        let host_class = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            lpszClassName: w!("WfsFolderDialog"),
            hbrBackground: unsafe { CreateSolidBrush(C_BG) },
            ..panel_class
        };
        unsafe { RegisterClassW(&host_class) };

        let host = unsafe {
            CreateWindowExW(
                0,
                w!("WfsFolderDialog"),
                w!("WhiteoutFlakes - open from folder"),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1080,
                680,
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

        let filter_btn = child(w!("BUTTON"), w!("Filter: All"), OWNERDRAW, ID_FILTER);
        let up = child(w!("BUTTON"), w!("Up"), OWNERDRAW, ID_UP);
        // LBS_NOTIFY so clicks arrive, OWNERDRAWFIXED so the rows can hold a
        // thumbnail, HASSTRINGS so keyboard type-ahead still finds files.
        // The vertical scrollbar is not automatic on a plain listbox.
        let list = child(
            w!("LISTBOX"),
            w!(""),
            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | 0x0200000, // | WS_VSCROLL
            ID_LIST,
        );
        unsafe { SendMessageW(list, LB_SETITEMHEIGHT, 0, ROW_H as isize) };
        let status = child(w!("STATIC"), w!(""), 0, ID_STATUS);
        let panel = child(w!("WfsDlgPanel"), w!(""), 0, ID_PANEL);

        let font = unsafe { ui_font() };
        for h in [filter_btn, up, list, status] {
            unsafe { SendMessageW(h, WM_SETFONT, font as WPARAM, 1) };
        }

        unsafe {
            layout(host);
            ShowWindow(host, SW_SHOW);
        }
        Ok(Host {
            list,
            filter_btn,
            status,
            panel,
            font,
        })
    }

    /// Browser column on the left, viewport filling the rest.
    unsafe fn layout(host: HWND) {
        let (w, h) = unsafe { client_size(host) };
        if w <= 0 || h <= 0 {
            return;
        }
        let g = 8;
        let place = |id: usize, x: i32, y: i32, cw: i32, ch: i32| {
            let c = unsafe { GetDlgItem(host, id as i32) };
            if !c.is_null() {
                unsafe { MoveWindow(c, x, y, cw.max(1), ch.max(1), 1) };
            }
        };
        place(ID_FILTER, g, g, 110, BAR_H - 6);
        place(ID_UP, g + 118, g, 60, BAR_H - 6);
        place(
            ID_LIST,
            g,
            g + BAR_H,
            LIST_W - g * 2,
            h - BAR_H - STATUS_H - g * 2,
        );
        place(ID_STATUS, g, h - STATUS_H - 2, LIST_W - g * 2, STATUS_H);
        place(ID_PANEL, LIST_W, 0, w - LIST_W, h);

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

    unsafe extern "system" fn wndproc(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        match msg {
            WM_COMMAND => {
                let id = wparam & 0xFFFF;
                let code = ((wparam >> 16) & 0xFFFF) as u16;
                APP.with(|a| {
                    let mut guard = a.borrow_mut();
                    let Some(app) = guard.as_mut() else {
                        return;
                    };
                    match id {
                        ID_FILTER => cycle_filter(app),
                        ID_UP => {
                            app.browser.ascend();
                            refill(app);
                        }
                        // Double-click opens folders; a single click previews
                        // the file it landed on, so arrowing through the list
                        // shows each one in the viewport.
                        ID_LIST if code == LBN_DBLCLK => activate(app),
                        ID_LIST if code == LBN_SELCHANGE => preview_selection(app),
                        _ => {}
                    }
                });
                0
            }
            // The listbox and the status text take their colours from the
            // parent; without these they stay system-light against a dark
            // viewport.
            WM_CTLCOLORLISTBOX | WM_CTLCOLORSTATIC => unsafe { ctl_colour(msg, wparam) },
            WM_DRAWITEM => {
                let d = unsafe { &*(lparam as *const DRAWITEMSTRUCT) };
                if d.CtlType != ODT_LISTBOX {
                    unsafe { draw_button(d) };
                    return 1;
                }
                // try_borrow: a repaint can land while the app is mid-update,
                // and a blank row that fills in on the next paint beats a
                // panic.
                APP.with(|a| {
                    let Ok(guard) = a.try_borrow() else {
                        return;
                    };
                    let Some(app) = guard.as_ref() else {
                        return;
                    };
                    match app.rows.get(d.itemID as usize) {
                        Some(Row::Up) => unsafe {
                            thumbs::draw_row(d, "..", thumbs::Cell::Folder, false)
                        },
                        Some(Row::Folder(name)) => unsafe {
                            thumbs::draw_row(d, name, thumbs::Cell::Folder, false)
                        },
                        Some(Row::File {
                            name, path, effect, ..
                        }) => {
                            let cell = match app.thumbs.as_ref().and_then(|t| t.get(path)) {
                                Some(b) if !b.is_empty() => thumbs::Cell::Image(b),
                                Some(_) => thumbs::Cell::Failed,
                                None => thumbs::Cell::Pending,
                            };
                            // Effects read in the accent colour instead of a
                            // text tag, now the row has room to show what it
                            // is.
                            unsafe { thumbs::draw_row(d, name, cell, *effect) };
                        }
                        None => {}
                    }
                });
                1
            }
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
            _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
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
