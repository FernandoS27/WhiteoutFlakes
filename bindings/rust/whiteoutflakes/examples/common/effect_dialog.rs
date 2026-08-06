// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Modal Open and Save dialogs for corn effects: the storage explorer's
// thumbnail grid, asked to answer a question.
//
//   #[path = "common/theme.rs"]  mod theme;
//   #[path = "common/thumbs.rs"] mod thumbs;
//   #[path = "common/effect_dialog.rs"] mod effect_dialog;
//
// from inside a `#[cfg(windows)]` module — it needs all three.
//
// Tiles, a breadcrumb, and a folder tree — the same shape as
// `tools/model_explorer`'s Storage Explorer panel, because picking an effect
// is looking at effects, and a `.pkb` name says almost nothing about what is
// in the file:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ ‹ › ↑   root › _hd.w3mod › sharedfx › firebrazier            │
//   ├───────────────┬──────────────────────────────────────────────┤
//   │ ▾ root        │  ┌────┐    ┌────┐    ┌────┐    ┌────┐        │
//   │   ▸ _hd.w3mod │  │ ↑  │    │ 📁 │    │[fx]│    │[fx]│        │
//   │   ▾ sharedfx  │  └────┘    └────┘    └────┘    └────┘        │
//   │     firebra…  │    ..      sharedfx  brazier_  brazier_      │
//   │               │          3 effects     blue      green       │
//   ├───────────────┴──────────────────────────────────────────────┤
//   │ File name: [______________________]                          │
//   │ 12 effects                              [ Save ] [ Cancel ]  │
//   └──────────────────────────────────────────────────────────────┘
//
// Four ways down and up, none of them "climb one level at a time": the tree
// jumps anywhere in the storage without disturbing the grid's folder, every
// breadcrumb segment is a button, back/forward retrace, and the grid's first
// tile is the folder above. The tree fills in lazily — a node's children are
// read the first time it is opened, so pointing this at a large storage does
// not walk all of it up front.
//
// Every file tile plays the effect itself: `Thumbs` records a short loop
// through `readback_target` at the tile's own resolution ([`TILE`]) — the
// picture is the size it is drawn at rather than a blown-up 64² one — and a
// timer here flips the frames. A still would be a poor likeness of a thing
// whose whole character is how it moves.
//
// Folders are tiles too, captioned with how many effects are directly inside.
// That count is what makes the grid navigable in a corpus of models, where
// most folders hold no effects at all and would otherwise look identical until
// opened. It costs one listing per folder shown.
//
// Both modes are the same widget with a different bottom bar, because they are
// the same question asked twice: *which* effect. Open answers with one that
// exists; Save answers with a name in a folder, which may or may not exist
// yet. `StorageFileFilter::EffectsOnly` means the grid never shows anything
// but `.pkb`/`.pkfx`, so neither mode can return something that is not an
// effect.
//
// Modal the honest way: disable the owner, run a local message pump, re-enable
// it. The pump also drives thumbnails, so pictures keep filling in while the
// dialog is up.

#![allow(dead_code)]

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};

use windows_sys::core::w;
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, SIZE, WPARAM};
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
// The tree view, WM_MOUSELEAVE and SetScrollInfo all live over here rather
// than in WindowsAndMessaging, where the rest of the window messages are.
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    EnableWindow, SetActiveWindow, SetFocus, TrackMouseEvent, TME_LEAVE, TRACKMOUSEEVENT,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use whiteoutflakes::{StorageBrowser, StorageFileFilter, StorageKind};

use super::theme::*;
use super::thumbs::{self, Thumbs};

const ID_GRID: usize = 1;
const ID_TREE: usize = 2;
const ID_BACK: usize = 3;
const ID_FWD: usize = 4;
const ID_UP: usize = 5;
const ID_ACCEPT: usize = 6;
const ID_CANCEL: usize = 7;
const ID_NAME: usize = 8;
const ID_NAME_LBL: usize = 9;
const ID_INFO: usize = 10;
/// Breadcrumb buttons are `ID_CRUMB..ID_CRUMB + MAX_CRUMBS`, created once and
/// shown or hidden as the path gets deeper or shallower.
const ID_CRUMB: usize = 100;
const MAX_CRUMBS: usize = 12;

/// Edge of the picture in a tile — and so the resolution the host should build
/// its [`Thumbs`] at, with `Thumbs::with_size`.
pub const TILE: i32 = 128;
/// Frames in a tile's loop, and the rate they are played at. `FRAME_MS` has to
/// match what `Thumbs` records (one frame per two 1/30 s ticks) or the effects
/// run fast or slow.
pub const FRAMES: usize = 12;
const FRAME_MS: u32 = 1000 / 15;
const ID_ANIM_TIMER: usize = 1;
/// Above this many effects in one folder, tiles are stills: a frame is 64 KB,
/// and a corpus folder can hold hundreds.
const MAX_ANIMATED: usize = 64;

const DLG_W: i32 = 1080;
const DLG_H: i32 = 680;
const PAD: i32 = 10;
const BAR_H: i32 = 34;
const TREE_W: i32 = 250;
const BTN_W: i32 = 92;
const BTN_H: i32 = 28;
const NAV_W: i32 = 30;

/// Tile chrome: padding around the picture, and the two label lines under it.
const CELL_PAD: i32 = 8;
const LABEL_H: i32 = 34;
const CELL_W: i32 = TILE + CELL_PAD * 2;
const CELL_H: i32 = TILE + CELL_PAD * 2 + LABEL_H;
/// Border between the grid's edge and the first tile.
const MARGIN: i32 = 8;
/// Room for the "nothing here" note under the last row.
const NOTE_H: i32 = 40;

const VK_BACKSPACE: u16 = 8;
const VK_PAGE_UP: u16 = 33;
const VK_PAGE_DOWN: u16 = 34;
const VK_END: u16 = 35;
const VK_HOME: u16 = 36;
const VK_LEFT: u16 = 37;
const VK_UP_ARROW: u16 = 38;
const VK_RIGHT: u16 = 39;
const VK_DOWN_ARROW: u16 = 40;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Open,
    Save,
}

/// Pick an existing effect. `None` if the user cancelled.
pub fn open(owner: HWND, root: &str, thumbs: &mut Thumbs) -> Option<String> {
    run(owner, Mode::Open, root, thumbs, "")
}

/// Pick a path to write an effect to. The file need not exist; the extension
/// is forced to `.pkb`, and an existing file is confirmed before it is
/// returned. `None` if the user cancelled.
pub fn save(owner: HWND, root: &str, thumbs: &mut Thumbs, suggested: &str) -> Option<String> {
    run(owner, Mode::Save, root, thumbs, suggested)
}

/// One tile. Folders and files share the grid the way they share a folder
/// window: a tile knows what a click on it means without re-deriving it.
enum Item {
    Up,
    Folder {
        path: String,
        label: String,
        effects: usize,
    },
    File {
        name: String,
        path: String,
    },
}

struct Dlg<'a> {
    mode: Mode,
    browser: StorageBrowser,
    thumbs: &'a mut Thumbs,

    /// Display path of the folder being shown; empty is the storage root.
    cwd: String,
    items: Vec<Item>,
    sel: Option<usize>,
    hover: Option<usize>,
    scroll: i32,
    /// Which frame of every tile's loop is showing. One counter for the grid,
    /// so the effects play together.
    anim: usize,
    crumbs: Vec<String>,
    back: Vec<String>,
    fwd: Vec<String>,

    /// Tree bookkeeping. `nodes` is how a path finds its item again when the
    /// user arrives by breadcrumb, tile or history instead of by clicking the
    /// tree.
    nodes: HashMap<String, HTREEITEM>,
    populated: HashSet<String>,
    /// `lParam` on a tree item is an index into here — a tree item cannot own
    /// a `String`, and an index survives everything a pointer would not.
    node_paths: Vec<String>,
    /// Effects directly inside a folder, by path. Both panes caption folders
    /// with it and each answer costs a listing, so it is worth remembering.
    counts: HashMap<String, usize>,

    hwnd: HWND,
    grid: HWND,
    tree: HWND,
    name: HWND,
    info: HWND,
    result: Option<String>,
    done: bool,
}

fn run(
    owner: HWND,
    mode: Mode,
    root: &str,
    thumbs: &mut Thumbs,
    suggested: &str,
) -> Option<String> {
    let mut browser = StorageBrowser::new();
    if !browser.open(root, StorageKind::Folder) {
        let msg = format!("{}: {}", root, browser.last_error());
        unsafe { message_box(owner, &msg, "Effect dialog", MB_ICONERROR) };
        return None;
    }
    // The whole point of this widget: nothing but effects is ever listed, so
    // neither mode can hand back something that is not one.
    browser.set_filter(StorageFileFilter::EffectsOnly);

    // A `RefCell` rather than a bare pointer: the grid is a child window whose
    // messages come back into the dialog while the dialog is already being
    // mutated (navigating, say). `try_borrow_mut` turns that re-entrancy into a
    // skipped message instead of aliasing.
    let cell = RefCell::new(Dlg {
        mode,
        browser,
        thumbs,
        cwd: String::new(),
        items: Vec::new(),
        sel: None,
        hover: None,
        scroll: 0,
        anim: 0,
        crumbs: Vec::new(),
        back: Vec::new(),
        fwd: Vec::new(),
        nodes: HashMap::new(),
        populated: HashSet::new(),
        node_paths: Vec::new(),
        counts: HashMap::new(),
        hwnd: std::ptr::null_mut(),
        grid: std::ptr::null_mut(),
        tree: std::ptr::null_mut(),
        name: std::ptr::null_mut(),
        info: std::ptr::null_mut(),
        result: None,
        done: false,
    });

    let font = unsafe { ui_font() };
    // SAFETY: the pointer is installed on a window created and destroyed
    // inside this function, so it cannot outlive `cell` on the stack.
    if !unsafe { create(owner, &cell, font, suggested) } {
        unsafe { DeleteObject(font as _) };
        return None;
    }

    let hwnd = cell.borrow().hwnd;
    unsafe {
        EnableWindow(owner, 0);
        pump(&cell);
        // Re-enable before destroying, or Windows hands focus to some other
        // application on the way out.
        EnableWindow(owner, 1);
        SetActiveWindow(owner);
        DestroyWindow(hwnd);
        DeleteObject(font as _);
    }
    let result = cell.borrow_mut().result.take();
    result
}

unsafe fn create(owner: HWND, cell: &RefCell<Dlg>, font: HFONT, suggested: &str) -> bool {
    unsafe {
        let hinst = GetModuleHandleW(std::ptr::null());

        // SysTreeView32 lives in comctl32 and is not registered until asked
        // for.
        let icc = INITCOMMONCONTROLSEX {
            dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
            dwICC: ICC_TREEVIEW_CLASSES,
        };
        InitCommonControlsEx(&icc);

        let class = WNDCLASSW {
            style: 0,
            lpfnWndProc: Some(wndproc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: hinst,
            hIcon: std::ptr::null_mut(),
            hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
            hbrBackground: CreateSolidBrush(C_BG),
            lpszMenuName: std::ptr::null(),
            lpszClassName: w!("WfsEffectDialog"),
        };
        // Harmless if it is already registered from an earlier call.
        RegisterClassW(&class);
        // CS_DBLCLKS: a grid without double-click is a grid you cannot open a
        // folder from. Nothing is erased for it — the paint is buffered.
        let grid_class = WNDCLASSW {
            style: CS_DBLCLKS,
            lpfnWndProc: Some(grid_proc),
            hbrBackground: std::ptr::null_mut(),
            lpszClassName: w!("WfsEffectGrid"),
            ..class
        };
        RegisterClassW(&grid_class);

        let style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        let mut r = RECT {
            left: 0,
            top: 0,
            right: DLG_W,
            bottom: DLG_H,
        };
        AdjustWindowRectEx(&mut r, style, 0, WS_EX_DLGMODALFRAME);
        let (ww, wh) = (r.right - r.left, r.bottom - r.top);
        let (x, y) = centre_on(owner, ww, wh);

        let mode = cell.borrow().mode;
        let hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            w!("WfsEffectDialog"),
            match mode {
                Mode::Open => w!("Open effect"),
                Mode::Save => w!("Save effect as"),
            },
            style,
            x,
            y,
            ww,
            wh,
            owner,
            std::ptr::null_mut(),
            hinst,
            std::ptr::null(),
        );
        if hwnd.is_null() {
            return false;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, cell as *const RefCell<Dlg> as isize);

        let child = |class: *const u16, text: *const u16, style: u32, id: usize| -> HWND {
            CreateWindowExW(
                0,
                class,
                text,
                WS_CHILD | WS_VISIBLE | style,
                0,
                0,
                10,
                10,
                hwnd,
                id as _,
                hinst,
                std::ptr::null(),
            )
        };

        let back = child(w!("BUTTON"), w!("<"), OWNERDRAW, ID_BACK);
        let fwd = child(w!("BUTTON"), w!(">"), OWNERDRAW, ID_FWD);
        let up = child(w!("BUTTON"), w!("^"), OWNERDRAW, ID_UP);
        let mut controls = vec![back, fwd, up];
        for i in 0..MAX_CRUMBS {
            // Created hidden: `update_crumbs` shows exactly as many as the
            // current path has segments.
            let c = CreateWindowExW(
                0,
                w!("BUTTON"),
                w!(""),
                WS_CHILD | OWNERDRAW,
                0,
                0,
                10,
                10,
                hwnd,
                (ID_CRUMB + i) as _,
                hinst,
                std::ptr::null(),
            );
            controls.push(c);
        }

        let tree = child(
            WC_TREEVIEWW,
            w!(""),
            TVS_HASBUTTONS
                | TVS_HASLINES
                | TVS_LINESATROOT
                | TVS_SHOWSELALWAYS
                | TVS_FULLROWSELECT
                | 0x0200000,
            ID_TREE,
        );
        // The tree is the one control here that can be made dark without
        // owner-drawing it.
        SendMessageW(tree, TVM_SETBKCOLOR, 0, C_LIST as isize);
        SendMessageW(tree, TVM_SETTEXTCOLOR, 0, C_TEXT as isize);
        SendMessageW(tree, TVM_SETLINECOLOR, 0, C_TEXT_DIM as isize);

        let grid = child(
            w!("WfsEffectGrid"),
            w!(""),
            WS_VSCROLL | WS_TABSTOP,
            ID_GRID,
        );
        let info = child(w!("STATIC"), w!(""), 0, ID_INFO);

        let accept = child(
            w!("BUTTON"),
            match mode {
                Mode::Open => w!("Open"),
                Mode::Save => w!("Save"),
            },
            OWNERDRAW,
            ID_ACCEPT,
        );
        let cancel = child(w!("BUTTON"), w!("Cancel"), OWNERDRAW, ID_CANCEL);
        controls.extend([tree, grid, info, accept, cancel]);

        if mode == Mode::Save {
            let label = child(w!("STATIC"), w!("File name:"), 0, ID_NAME_LBL);
            // ES_AUTOHSCROLL so a long name scrolls rather than stopping.
            let name = child(w!("EDIT"), w!(""), 0x0080 | WS_BORDER, ID_NAME);
            set_text(name, suggested);
            cell.borrow_mut().name = name;
            controls.extend([label, name]);
        }
        for c in controls {
            SendMessageW(c, WM_SETFONT, font as WPARAM, 1);
        }

        {
            let mut dlg = cell.borrow_mut();
            dlg.hwnd = hwnd;
            dlg.grid = grid;
            dlg.tree = tree;
            dlg.info = info;
            layout(&dlg);
            seed_tree(&mut dlg);
            navigate(&mut dlg, String::new(), false);
        }
        ShowWindow(hwnd, SW_SHOW);
        SetTimer(grid, ID_ANIM_TIMER, FRAME_MS, None);
        SetFocus(if mode == Mode::Save {
            cell.borrow().name
        } else {
            grid
        });
        true
    }
}

unsafe fn centre_on(owner: HWND, w: i32, h: i32) -> (i32, i32) {
    unsafe {
        let mut r = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        if owner.is_null() || GetWindowRect(owner, &mut r) == 0 {
            return (CW_USEDEFAULT, CW_USEDEFAULT);
        }
        (
            r.left + ((r.right - r.left) - w) / 2,
            r.top + ((r.bottom - r.top) - h) / 2,
        )
    }
}

unsafe fn layout(dlg: &Dlg) {
    unsafe {
        let (w, h) = client_size(dlg.hwnd);
        if w <= 0 || h <= 0 {
            return;
        }
        let place = |id: usize, x: i32, y: i32, cw: i32, ch: i32| {
            let c = GetDlgItem(dlg.hwnd, id as i32);
            if !c.is_null() {
                MoveWindow(c, x, y, cw.max(1), ch.max(1), 1);
            }
        };
        let bar = BTN_H + PAD * 2;
        let name_row = if dlg.mode == Mode::Save { 34 } else { 0 };
        let top = PAD + BAR_H;

        let panes_h = h - top - bar - name_row;
        place(ID_BACK, PAD, PAD, NAV_W, 26);
        place(ID_FWD, PAD + NAV_W + 4, PAD, NAV_W, 26);
        place(ID_UP, PAD + (NAV_W + 4) * 2, PAD, NAV_W, 26);
        place(ID_TREE, PAD, top, TREE_W, panes_h);
        place(
            ID_GRID,
            PAD + TREE_W + 8,
            top,
            w - PAD * 2 - TREE_W - 8,
            panes_h,
        );
        if dlg.mode == Mode::Save {
            let y = h - bar - name_row + 4;
            place(ID_NAME_LBL, PAD, y + 4, 66, 20);
            place(ID_NAME, PAD + 70, y, w - PAD * 2 - 70, 24);
        }
        place(
            ID_INFO,
            PAD + 2,
            h - PAD - BTN_H + 6,
            (w - PAD * 2 - BTN_W * 2 - 24).max(10),
            18,
        );
        place(
            ID_ACCEPT,
            w - PAD - BTN_W * 2 - 8,
            h - PAD - BTN_H,
            BTN_W,
            BTN_H,
        );
        place(ID_CANCEL, w - PAD - BTN_W, h - PAD - BTN_H, BTN_W, BTN_H);
        place_crumbs(dlg, w);
    }
}

// ── Navigation ───────────────────────────────────────────────────────────

/// Go to `path` (a display path; empty is the root) and bring every pane into
/// agreement with it.
unsafe fn navigate(dlg: &mut Dlg, path: String, push: bool) {
    unsafe {
        if push && path != dlg.cwd {
            let from = std::mem::replace(&mut dlg.cwd, path.clone());
            dlg.back.push(from);
            dlg.fwd.clear();
        }
        dlg.cwd = path;
        dlg.browser.navigate_to(&dlg.cwd);
        dlg.crumbs = dlg.browser.breadcrumb();
        refill(dlg);
        update_crumbs(dlg);
        reveal(dlg);
        enable_nav(dlg);
    }
}

unsafe fn go_back(dlg: &mut Dlg) {
    if let Some(prev) = dlg.back.pop() {
        let here = std::mem::take(&mut dlg.cwd);
        dlg.fwd.push(here);
        unsafe { navigate(dlg, prev, false) };
    }
}

unsafe fn go_forward(dlg: &mut Dlg) {
    if let Some(next) = dlg.fwd.pop() {
        let here = std::mem::take(&mut dlg.cwd);
        dlg.back.push(here);
        unsafe { navigate(dlg, next, false) };
    }
}

unsafe fn go_up(dlg: &mut Dlg) {
    if dlg.cwd.is_empty() {
        return;
    }
    let mut segs = split(&dlg.cwd);
    segs.pop();
    unsafe { navigate(dlg, segs.join("\\"), true) };
}

unsafe fn enable_nav(dlg: &Dlg) {
    unsafe {
        EnableWindow(
            GetDlgItem(dlg.hwnd, ID_BACK as i32),
            i32::from(!dlg.back.is_empty()),
        );
        EnableWindow(
            GetDlgItem(dlg.hwnd, ID_FWD as i32),
            i32::from(!dlg.fwd.is_empty()),
        );
        EnableWindow(
            GetDlgItem(dlg.hwnd, ID_UP as i32),
            i32::from(!dlg.cwd.is_empty()),
        );
    }
}

fn split(path: &str) -> Vec<String> {
    path.split('\\')
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .collect()
}

fn join(base: &str, seg: &str) -> String {
    if base.is_empty() {
        seg.to_string()
    } else {
        format!("{base}\\{seg}")
    }
}

/// What is directly under `path`: subfolder names, and how many effects.
/// Navigating is how the browser answers this, so it is put back where it was
/// afterwards.
fn contents_at(dlg: &mut Dlg, path: &str) -> (Vec<String>, usize) {
    dlg.browser.navigate_to(path);
    let folders = dlg.browser.folders();
    let effects = dlg.browser.files().len();
    dlg.browser.navigate_to(&dlg.cwd.clone());
    dlg.counts.insert(path.to_string(), effects);
    (folders, effects)
}

/// How many effects sit directly in `path`, from the cache when it is known.
fn effects_in(dlg: &mut Dlg, path: &str) -> usize {
    if let Some(&n) = dlg.counts.get(path) {
        return n;
    }
    contents_at(dlg, path).1
}

/// Rebuild the tiles from the browser's current folder and queue a thumbnail
/// for every effect in it. The thumbnail cache is kept across folders, so
/// walking back into one is instant.
unsafe fn refill(dlg: &mut Dlg) {
    unsafe {
        dlg.items.clear();
        dlg.sel = None;
        dlg.hover = None;
        dlg.scroll = 0;
        // Leaving a folder drops what has not been drawn yet, so the new one is
        // not stuck behind the old one's backlog.
        dlg.thumbs.forget_queue();

        if !dlg.cwd.is_empty() {
            dlg.items.push(Item::Up);
        }
        for name in dlg.browser.folders() {
            let path = join(&dlg.cwd, &name);
            let effects = effects_in(dlg, &path);
            dlg.items.push(Item::Folder {
                path,
                label: name,
                effects,
            });
        }
        let files = dlg.browser.files();
        // Animate a folder you can hold in memory; past that, stills.
        let frames = if files.len() <= MAX_ANIMATED {
            FRAMES
        } else {
            1
        };
        dlg.thumbs.set_frames(frames);
        for name in files {
            let path = dlg.browser.child_path(&name);
            dlg.thumbs.request(&path, true);
            dlg.items.push(Item::File { name, path });
        }

        // Opening lands on the first effect so Enter means something and the
        // info line has something to say. Not in Save mode: selecting a file
        // there types its name into the box, which would clobber what the user
        // came in with.
        if dlg.mode == Mode::Open {
            dlg.sel = dlg
                .items
                .iter()
                .position(|i| matches!(i, Item::File { .. }));
        }
        update_scroll(dlg);
        set_info(dlg);
        InvalidateRect(dlg.grid, std::ptr::null(), 0);
    }
}

// ── Folder tree ──────────────────────────────────────────────────────────

unsafe fn seed_tree(dlg: &mut Dlg) {
    unsafe {
        let root = insert_node(dlg, TVI_ROOT, "root", String::new(), true);
        dlg.nodes.insert(String::new(), root);
        populate(dlg, "");
        SendMessageW(dlg.tree, TVM_EXPAND, TVE_EXPAND as usize, root);
    }
}

unsafe fn insert_node(
    dlg: &mut Dlg,
    parent: HTREEITEM,
    label: &str,
    path: String,
    has_children: bool,
) -> HTREEITEM {
    unsafe {
        dlg.node_paths.push(path);
        let param = (dlg.node_paths.len() - 1) as LPARAM;
        let mut text: Vec<u16> = format!("{label}\0").encode_utf16().collect();

        let mut ins: TVINSERTSTRUCTW = std::mem::zeroed();
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.Anonymous.item = TVITEMW {
            mask: TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN,
            hItem: 0,
            state: 0,
            stateMask: 0,
            pszText: text.as_mut_ptr(),
            cchTextMax: 0,
            iImage: 0,
            iSelectedImage: 0,
            cChildren: i32::from(has_children),
            lParam: param,
        };
        SendMessageW(
            dlg.tree,
            TVM_INSERTITEMW,
            0,
            &ins as *const TVINSERTSTRUCTW as isize,
        )
    }
}

/// Read one level of children into the tree. Cheap to call twice: a folder is
/// only ever filled in once.
unsafe fn populate(dlg: &mut Dlg, path: &str) {
    unsafe {
        if dlg.populated.contains(path) {
            return;
        }
        let Some(&parent) = dlg.nodes.get(path) else {
            return;
        };
        dlg.populated.insert(path.to_string());
        for name in contents_at(dlg, path).0 {
            let child = join(path, &name);
            if dlg.nodes.contains_key(&child) {
                continue;
            }
            // The count is what makes the tree worth reading: in a corpus of
            // models, most folders hold no effects at all, and without it
            // every one of them looks the same until it is opened.
            let (subfolders, effects) = contents_at(dlg, &child);
            let label = if effects > 0 {
                format!("{name}  ({effects})")
            } else {
                name
            };
            let item = insert_node(dlg, parent, &label, child.clone(), !subfolders.is_empty());
            dlg.nodes.insert(child, item);
        }
    }
}

/// Make sure the current folder has a tree item, opening every ancestor on the
/// way down, then select it. This is what keeps the tree in step when the user
/// arrives by tile, breadcrumb, history or Up rather than by clicking it.
unsafe fn reveal(dlg: &mut Dlg) {
    unsafe {
        let mut acc = String::new();
        populate(dlg, "");
        for seg in split(&dlg.cwd.clone()) {
            let parent = acc.clone();
            populate(dlg, &parent);
            acc = join(&parent, &seg);
            if let Some(&item) = dlg.nodes.get(&parent) {
                SendMessageW(dlg.tree, TVM_EXPAND, TVE_EXPAND as usize, item);
            }
        }
        if let Some(&item) = dlg.nodes.get(&dlg.cwd) {
            // The notification this raises re-enters the window procedure,
            // where the borrow is already held and the message is skipped —
            // which is what we want, since we are already where it would
            // navigate to.
            SendMessageW(dlg.tree, TVM_SELECTITEM, TVGN_CARET as usize, item);
        }
    }
}

fn tree_path(dlg: &Dlg, param: LPARAM) -> Option<String> {
    dlg.node_paths.get(param as usize).cloned()
}

/// The folder the tree is currently on, read from the control rather than
/// remembered from a notification.
///
/// The dialog manipulates its own tree (opening ancestors, re-selecting the
/// current node), and any message raised while it is doing so is skipped —
/// which would silently lose a click that arrived at the wrong moment. Asking
/// the control what is selected cannot be missed that way, so navigation is
/// driven from the pump instead of from `TVN_SELCHANGED`.
unsafe fn tree_selection(dlg: &Dlg) -> Option<String> {
    unsafe {
        let item = SendMessageW(dlg.tree, TVM_GETNEXTITEM, TVGN_CARET as usize, 0);
        if item == 0 {
            return None;
        }
        let mut tv: TVITEMW = std::mem::zeroed();
        tv.mask = TVIF_PARAM | TVIF_HANDLE;
        tv.hItem = item;
        if SendMessageW(dlg.tree, TVM_GETITEMW, 0, &mut tv as *mut TVITEMW as isize) == 0 {
            return None;
        }
        tree_path(dlg, tv.lParam)
    }
}

// ── Breadcrumb ───────────────────────────────────────────────────────────

/// Segment `i` of the breadcrumb is the path of the first `i` segments, so
/// clicking one jumps straight there instead of stepping up to it.
unsafe fn update_crumbs(dlg: &Dlg) {
    unsafe {
        for i in 0..MAX_CRUMBS {
            let c = GetDlgItem(dlg.hwnd, (ID_CRUMB + i) as i32);
            if c.is_null() {
                continue;
            }
            // Slot 0 is the root itself; the rest are path segments.
            let label = if i == 0 {
                Some("root".to_string())
            } else {
                dlg.crumbs.get(i - 1).cloned()
            };
            match label {
                Some(text) => {
                    set_text(c, &text);
                    ShowWindow(c, SW_SHOW);
                }
                None => {
                    ShowWindow(c, SW_HIDE);
                }
            }
        }
        let (w, _) = client_size(dlg.hwnd);
        place_crumbs(dlg, w);
        InvalidateRect(dlg.hwnd, std::ptr::null(), 1);
    }
}

unsafe fn place_crumbs(dlg: &Dlg, w: i32) {
    unsafe {
        let mut x = PAD + (NAV_W + 4) * 3 + 8;
        let hdc = GetDC(dlg.hwnd);
        // Measure in the font the buttons actually draw in, not the DC's stock
        // one, or every crumb comes out the wrong width.
        let font = SendMessageW(GetDlgItem(dlg.hwnd, ID_BACK as i32), WM_GETFONT, 0, 0) as HFONT;
        let old = if font.is_null() {
            std::ptr::null_mut()
        } else {
            SelectObject(hdc, font as _)
        };
        for i in 0..MAX_CRUMBS {
            let c = GetDlgItem(dlg.hwnd, (ID_CRUMB + i) as i32);
            if c.is_null() || !shown(c) {
                continue;
            }
            let mut text = [0u16; 64];
            let n = GetWindowTextW(c, text.as_mut_ptr(), 64);
            let mut size = SIZE { cx: 0, cy: 0 };
            GetTextExtentPoint32W(hdc, text.as_ptr(), n, &mut size);
            let cw = (size.cx + 18).min(160);
            if x + cw > w - PAD {
                ShowWindow(c, SW_HIDE);
                continue;
            }
            MoveWindow(c, x, PAD, cw, 26, 1);
            x += cw + 12; // room for the separator drawn in WM_PAINT
        }
        if !old.is_null() {
            SelectObject(hdc, old);
        }
        ReleaseDC(dlg.hwnd, hdc);
    }
}

/// Has this control been shown? `IsWindowVisible` is not the question: it also
/// answers for the ancestors, so during `create` — parent not shown yet — it
/// says no for every crumb, and the crumbs are laid out there.
unsafe fn shown(hwnd: HWND) -> bool {
    (unsafe { GetWindowLongW(hwnd, GWL_STYLE) } as u32 & WS_VISIBLE) != 0
}

fn crumb_target(dlg: &Dlg, i: usize) -> String {
    if i == 0 {
        return String::new();
    }
    dlg.crumbs[..i.min(dlg.crumbs.len())].join("\\")
}

// ── Grid geometry ────────────────────────────────────────────────────────

/// How the tiles fall out of the grid's current width. Recomputed rather than
/// stored: the only thing it depends on is the client size and the item count.
struct Metrics {
    cols: i32,
    cell_w: i32,
    rows: i32,
    view_h: i32,
    total_h: i32,
}

unsafe fn metrics(dlg: &Dlg) -> Metrics {
    let (view_w, view_h) = unsafe { client_size(dlg.grid) };
    let usable = (view_w - MARGIN * 2).max(CELL_W);
    // Round to the nearest column rather than down: two thirds of a spare tile
    // is better spent squeezing the pictures a few pixels than left as margin.
    let cols = ((usable + CELL_W * 2 / 3) / CELL_W).max(1);
    // Tiles then share out what is left instead of leaving a ragged edge.
    let cell_w = usable / cols;
    let rows = (dlg.items.len() as i32 + cols - 1) / cols;
    let mut total_h = MARGIN * 2 + rows * CELL_H;
    if !has_files(dlg) {
        total_h += NOTE_H;
    }
    Metrics {
        cols,
        cell_w,
        rows,
        view_h,
        total_h,
    }
}

fn has_files(dlg: &Dlg) -> bool {
    dlg.items.iter().any(|i| matches!(i, Item::File { .. }))
}

/// Is anything on show a loop rather than a still?
fn animating(dlg: &Dlg) -> bool {
    dlg.items.iter().any(|i| match i {
        Item::File { path, .. } => dlg.thumbs.frames(path).is_some_and(|f| f.len() > 1),
        _ => false,
    })
}

fn cell_rect(m: &Metrics, i: usize, scroll: i32) -> RECT {
    let i = i as i32;
    let (col, row) = (i % m.cols, i / m.cols);
    let x = MARGIN + col * m.cell_w;
    let y = MARGIN + row * CELL_H - scroll;
    RECT {
        left: x,
        top: y,
        right: x + m.cell_w,
        bottom: y + CELL_H,
    }
}

fn hit_test(dlg: &Dlg, m: &Metrics, x: i32, y: i32) -> Option<usize> {
    let (gx, gy) = (x - MARGIN, y + dlg.scroll - MARGIN);
    if gx < 0 || gy < 0 {
        return None;
    }
    let col = gx / m.cell_w;
    if col >= m.cols {
        return None;
    }
    let i = ((gy / CELL_H) * m.cols + col) as usize;
    (i < dlg.items.len()).then_some(i)
}

unsafe fn update_scroll(dlg: &mut Dlg) {
    unsafe {
        let m = metrics(dlg);
        dlg.scroll = dlg.scroll.clamp(0, (m.total_h - m.view_h).max(0));
        let mut si: SCROLLINFO = std::mem::zeroed();
        si.cbSize = std::mem::size_of::<SCROLLINFO>() as u32;
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMax = (m.total_h - 1).max(0);
        si.nPage = m.view_h.max(0) as u32;
        si.nPos = dlg.scroll;
        SetScrollInfo(dlg.grid, SB_VERT, &si, 1);
    }
}

unsafe fn scroll_to(dlg: &mut Dlg, pos: i32) {
    unsafe {
        let m = metrics(dlg);
        let pos = pos.clamp(0, (m.total_h - m.view_h).max(0));
        if pos == dlg.scroll {
            return;
        }
        dlg.scroll = pos;
        update_scroll(dlg);
        InvalidateRect(dlg.grid, std::ptr::null(), 0);
    }
}

/// Scroll the selected tile into view — what makes arrow keys usable.
unsafe fn reveal_selection(dlg: &mut Dlg) {
    let Some(i) = dlg.sel else {
        return;
    };
    unsafe {
        let m = metrics(dlg);
        let top = MARGIN + (i as i32 / m.cols) * CELL_H;
        if top < dlg.scroll {
            scroll_to(dlg, top);
        } else if top + CELL_H > dlg.scroll + m.view_h {
            scroll_to(dlg, top + CELL_H - m.view_h);
        }
    }
}

unsafe fn invalidate_cell(dlg: &Dlg, i: usize) {
    unsafe {
        let m = metrics(dlg);
        let r = cell_rect(&m, i, dlg.scroll);
        InvalidateRect(dlg.grid, &r, 0);
    }
}

// ── Grid behaviour ───────────────────────────────────────────────────────

unsafe fn select(dlg: &mut Dlg, i: Option<usize>) {
    if dlg.sel == i {
        return;
    }
    unsafe {
        if let Some(old) = dlg.sel {
            invalidate_cell(dlg, old);
        }
        dlg.sel = i;
        if let Some(new) = i {
            invalidate_cell(dlg, new);
        }
        // Clicking a file in Save mode fills the name in, the way every save
        // dialog does.
        if dlg.mode == Mode::Save {
            if let Some(Item::File { name, .. }) = i.and_then(|i| dlg.items.get(i)) {
                let name = name.clone();
                set_text(dlg.name, &name);
            }
        }
        set_info(dlg);
    }
}

/// Move the selection by whole tiles. `step` is in items, so ±1 walks the row
/// and ±cols walks the column.
unsafe fn move_selection(dlg: &mut Dlg, step: i32) {
    if dlg.items.is_empty() {
        return;
    }
    let n = dlg.items.len() as i32;
    let from = dlg.sel.map_or(0, |i| i as i32);
    let to = (from + step).clamp(0, n - 1);
    unsafe {
        select(dlg, Some(to as usize));
        reveal_selection(dlg);
    }
}

/// What a double-click, or Enter, means for the tile under it.
unsafe fn activate(dlg: &mut Dlg, i: usize) {
    unsafe {
        match dlg.items.get(i) {
            Some(Item::Up) => go_up(dlg),
            Some(Item::Folder { path, .. }) => {
                let path = path.clone();
                navigate(dlg, path, true);
            }
            Some(Item::File { .. }) => accept(dlg),
            None => {}
        }
    }
}

fn selected_file<'a>(dlg: &'a Dlg<'_>) -> Option<&'a str> {
    match dlg.sel.and_then(|i| dlg.items.get(i)) {
        Some(Item::File { path, .. }) => Some(path),
        _ => None,
    }
}

unsafe fn set_info(dlg: &Dlg) {
    let shown = dlg.items.iter().filter(|i| has_file(i)).count();
    let total = dlg.browser.unfiltered_file_count();
    let mut s = match shown {
        0 => "no effects here".to_string(),
        1 => "1 effect".to_string(),
        n => format!("{n} effects"),
    };
    // Say what the filter hid, so a near-empty folder does not read as a
    // broken dialog.
    if total > shown as i32 {
        s.push_str(&format!("  ·  {} other files hidden", total - shown as i32));
    }
    if let Some(Item::File { name, .. }) = dlg.sel.and_then(|i| dlg.items.get(i)) {
        s = format!("{name}   —   {s}");
    }
    unsafe { set_text(dlg.info, &s) };
}

fn has_file(i: &Item) -> bool {
    matches!(i, Item::File { .. })
}

// ── Grid painting ────────────────────────────────────────────────────────

unsafe fn paint_grid(hwnd: HWND, dlg: &Dlg) {
    unsafe {
        let mut ps: PAINTSTRUCT = std::mem::zeroed();
        let hdc = BeginPaint(hwnd, &mut ps);
        let (w, h) = client_size(hwnd);
        if w <= 0 || h <= 0 {
            EndPaint(hwnd, &ps);
            return;
        }

        // Buffered: a thumbnail landing repaints its tile many times a second
        // while a folder fills in, and unbuffered tiles flicker.
        let mem = CreateCompatibleDC(hdc);
        let bmp = CreateCompatibleBitmap(hdc, w, h);
        let old_bmp = SelectObject(mem, bmp as _);
        let font = SendMessageW(hwnd, WM_GETFONT, 0, 0) as HFONT;
        let old_font = if font.is_null() {
            std::ptr::null_mut()
        } else {
            SelectObject(mem, font as _)
        };
        SetBkMode(mem, TRANSPARENT as i32);

        let all = RECT {
            left: 0,
            top: 0,
            right: w,
            bottom: h,
        };
        fill(mem, &all, C_LIST);

        let m = metrics(dlg);
        for i in 0..dlg.items.len() {
            let r = cell_rect(&m, i, dlg.scroll);
            if r.bottom < 0 || r.top > h {
                continue;
            }
            draw_cell(mem, dlg, i, &r);
        }
        if !has_files(dlg) {
            let mut note = RECT {
                left: 0,
                top: MARGIN + m.rows * CELL_H - dlg.scroll,
                right: w,
                bottom: MARGIN + m.rows * CELL_H - dlg.scroll + NOTE_H,
            };
            draw_text(
                mem,
                &mut note,
                "No effects in this folder",
                C_TEXT_DIM,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
            );
        }

        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if !old_font.is_null() {
            SelectObject(mem, old_font);
        }
        SelectObject(mem, old_bmp);
        DeleteObject(bmp as _);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }
}

unsafe fn draw_cell(hdc: HDC, dlg: &Dlg, i: usize, r: &RECT) {
    unsafe {
        let sel = dlg.sel == Some(i);
        let hot = dlg.hover == Some(i);
        let plate = RECT {
            left: r.left + 2,
            top: r.top + 2,
            right: r.right - 2,
            bottom: r.bottom - 2,
        };
        if sel || hot {
            round_fill(hdc, &plate, if sel { C_BTN_HOT } else { C_BTN }, 10);
        }
        if sel {
            round_frame(hdc, &plate, C_ACCENT, 10, 2);
        }

        let edge = TILE.min(r.right - r.left - CELL_PAD * 2).max(16);
        let x = r.left + (r.right - r.left - edge) / 2;
        let pic = RECT {
            left: x,
            top: r.top + CELL_PAD,
            right: x + edge,
            bottom: r.top + CELL_PAD + edge,
        };
        let mut label = RECT {
            left: r.left + 5,
            top: pic.bottom + 5,
            right: r.right - 5,
            bottom: r.bottom - 4,
        };
        let ink = if sel { C_ACCENT } else { C_TEXT };

        match &dlg.items[i] {
            Item::Up => {
                folder_tile(hdc, &pic, C_FOLDER);
                // The arrow, not the caption, is what makes this tile read as
                // "out of here" at a glance.
                up_arrow(hdc, &pic, C_LIST);
                let mut line = RECT {
                    bottom: label.top + 17,
                    ..label
                };
                draw_text(
                    hdc,
                    &mut line,
                    "..",
                    ink,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX,
                );
                let mut hint = RECT {
                    top: label.top + 17,
                    ..label
                };
                draw_text(
                    hdc,
                    &mut hint,
                    "up one level",
                    C_TEXT_DIM,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX,
                );
            }
            Item::Folder {
                label: name,
                effects,
                ..
            } => {
                folder_tile(hdc, &pic, C_FOLDER);
                let mut line = RECT {
                    bottom: label.top + 17,
                    ..label
                };
                draw_text(
                    hdc,
                    &mut line,
                    name,
                    ink,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
                );
                if *effects > 0 {
                    let mut count = RECT {
                        top: label.top + 17,
                        ..label
                    };
                    draw_text(
                        hdc,
                        &mut count,
                        &format!("{effects} effect{}", if *effects == 1 { "" } else { "s" }),
                        C_TEXT_DIM,
                        DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX,
                    );
                }
            }
            Item::File { name, path } => {
                match dlg.thumbs.frames(path) {
                    // The loop runs off one grid-wide counter, so a tile whose
                    // strip is shorter simply repeats sooner.
                    Some(strip) if !strip.is_empty() => {
                        thumbs::blit(hdc, &pic, &strip[dlg.anim % strip.len()])
                    }
                    Some(_) => placeholder(hdc, &pic, "×"),
                    None => placeholder(hdc, &pic, "…"),
                }
                // The extension is noise when every tile in the grid is an
                // effect; dropping it buys back a line of name.
                let stem = name.rsplit_once('.').map_or(name.as_str(), |(s, _)| s);
                draw_text(
                    hdc,
                    &mut label,
                    stem,
                    ink,
                    DT_CENTER
                        | DT_TOP
                        | DT_WORDBREAK
                        | DT_EDITCONTROL
                        | DT_END_ELLIPSIS
                        | DT_NOPREFIX,
                );
            }
        }
    }
}

/// A tile with no picture yet, or none coming.
unsafe fn placeholder(hdc: HDC, r: &RECT, mark: &str) {
    unsafe {
        round_fill(hdc, r, C_BTN, 6);
        let mut rr = *r;
        draw_text(
            hdc,
            &mut rr,
            mark,
            C_TEXT_DIM,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
        );
    }
}

/// The explorer's folder: a body and a tab, in the tile's own proportions.
unsafe fn folder_tile(hdc: HDC, r: &RECT, colour: u32) {
    unsafe {
        let s = (r.right - r.left) as f32;
        let px = |f: f32| r.left + (s * f) as i32;
        let py = |f: f32| r.top + (s * f) as i32;
        let tab = RECT {
            left: px(0.16),
            top: py(0.26),
            right: px(0.46),
            bottom: py(0.38),
        };
        let body = RECT {
            left: px(0.16),
            top: py(0.34),
            right: px(0.84),
            bottom: py(0.72),
        };
        round_fill(hdc, &tab, colour, (s * 0.08) as i32);
        round_fill(hdc, &body, colour, (s * 0.10) as i32);
    }
}

/// An arrow cut out of the folder body, for the tile that leaves the folder.
unsafe fn up_arrow(hdc: HDC, r: &RECT, colour: u32) {
    unsafe {
        let s = (r.right - r.left) as f32;
        let cx = (r.left + r.right) / 2;
        let f = |v: f32| r.top + (s * v) as i32;
        let (top, waist, bottom) = (f(0.40), f(0.53), f(0.66));
        let (head, stem) = ((s * 0.14) as i32, (s * 0.05) as i32);
        let pts = [
            POINT { x: cx, y: top },
            POINT {
                x: cx + head,
                y: waist,
            },
            POINT {
                x: cx + stem,
                y: waist,
            },
            POINT {
                x: cx + stem,
                y: bottom,
            },
            POINT {
                x: cx - stem,
                y: bottom,
            },
            POINT {
                x: cx - stem,
                y: waist,
            },
            POINT {
                x: cx - head,
                y: waist,
            },
        ];
        let brush = CreateSolidBrush(colour);
        let old_b = SelectObject(hdc, brush as _);
        let old_p = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, pts.as_ptr(), pts.len() as i32);
        SelectObject(hdc, old_p);
        SelectObject(hdc, old_b);
        DeleteObject(brush as _);
    }
}

unsafe fn round_fill(hdc: HDC, r: &RECT, colour: u32, radius: i32) {
    unsafe {
        let brush = CreateSolidBrush(colour);
        let old_b = SelectObject(hdc, brush as _);
        let old_p = SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(hdc, old_p);
        SelectObject(hdc, old_b);
        DeleteObject(brush as _);
    }
}

unsafe fn round_frame(hdc: HDC, r: &RECT, colour: u32, radius: i32, width: i32) {
    unsafe {
        let pen = CreatePen(PS_SOLID, width, colour);
        let old_p = SelectObject(hdc, pen as _);
        let old_b = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(hdc, old_b);
        SelectObject(hdc, old_p);
        DeleteObject(pen as _);
    }
}

unsafe fn draw_text(hdc: HDC, r: &mut RECT, s: &str, colour: u32, fmt: u32) {
    if s.is_empty() {
        return;
    }
    unsafe {
        SetTextColor(hdc, colour);
        let mut wide: Vec<u16> = s.encode_utf16().collect();
        DrawTextW(hdc, wide.as_mut_ptr(), wide.len() as i32, r, fmt);
    }
}

// ── Grid window ──────────────────────────────────────────────────────────

unsafe extern "system" fn grid_proc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    // The dialog owns the state; the grid is a view onto it. Reaching through
    // the parent keeps one pointer rather than two to keep in step.
    let parent = unsafe { GetParent(hwnd) };
    let ptr = unsafe { GetWindowLongPtrW(parent, GWLP_USERDATA) } as *const RefCell<Dlg>;
    if ptr.is_null() {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    }
    let cell = unsafe { &*ptr };
    let Ok(mut dlg) = cell.try_borrow_mut() else {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    };

    match msg {
        // The buffered paint covers every pixel, so erasing first only flickers.
        WM_ERASEBKGND => 1,
        WM_PAINT => {
            unsafe { paint_grid(hwnd, &dlg) };
            0
        }
        WM_SIZE => {
            unsafe { update_scroll(&mut dlg) };
            unsafe { InvalidateRect(hwnd, std::ptr::null(), 0) };
            0
        }
        // Next frame of every tile's loop. Nothing to do for a folder of
        // stills — repainting 15 times a second to draw the same picture would
        // just keep the pump awake.
        WM_TIMER => {
            dlg.anim = dlg.anim.wrapping_add(1);
            if animating(&dlg) {
                unsafe { InvalidateRect(hwnd, std::ptr::null(), 0) };
            }
            0
        }
        WM_VSCROLL => {
            let m = unsafe { metrics(&dlg) };
            let pos = match (wp & 0xFFFF) as i32 {
                SB_LINEUP => dlg.scroll - 40,
                SB_LINEDOWN => dlg.scroll + 40,
                SB_PAGEUP => dlg.scroll - m.view_h,
                SB_PAGEDOWN => dlg.scroll + m.view_h,
                SB_THUMBTRACK | SB_THUMBPOSITION => ((wp >> 16) & 0xFFFF) as i32,
                _ => dlg.scroll,
            };
            unsafe { scroll_to(&mut dlg, pos) };
            0
        }
        WM_MOUSEWHEEL => {
            let delta = ((wp >> 16) & 0xFFFF) as i16 as i32;
            let pos = dlg.scroll - delta * 40 * 3 / 120;
            unsafe { scroll_to(&mut dlg, pos) };
            0
        }
        WM_MOUSEMOVE => {
            let (x, y) = (lo_word(lp), hi_word(lp));
            let m = unsafe { metrics(&dlg) };
            let hover = hit_test(&dlg, &m, x, y);
            if hover != dlg.hover {
                if let Some(old) = dlg.hover {
                    unsafe { invalidate_cell(&dlg, old) };
                }
                dlg.hover = hover;
                if let Some(new) = hover {
                    unsafe { invalidate_cell(&dlg, new) };
                }
            }
            // Without this the last hovered tile stays lit after the pointer
            // leaves the grid.
            let mut tme: TRACKMOUSEEVENT = unsafe { std::mem::zeroed() };
            tme.cbSize = std::mem::size_of::<TRACKMOUSEEVENT>() as u32;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            unsafe { TrackMouseEvent(&mut tme) };
            0
        }
        WM_MOUSELEAVE => {
            if let Some(old) = dlg.hover.take() {
                unsafe { invalidate_cell(&dlg, old) };
            }
            0
        }
        WM_LBUTTONDOWN => {
            unsafe { SetFocus(hwnd) };
            let m = unsafe { metrics(&dlg) };
            let hit = hit_test(&dlg, &m, lo_word(lp), hi_word(lp));
            unsafe { select(&mut dlg, hit) };
            0
        }
        WM_LBUTTONDBLCLK => {
            let m = unsafe { metrics(&dlg) };
            if let Some(i) = hit_test(&dlg, &m, lo_word(lp), hi_word(lp)) {
                unsafe {
                    select(&mut dlg, Some(i));
                    activate(&mut dlg, i);
                }
            }
            0
        }
        WM_KEYDOWN => {
            let m = unsafe { metrics(&dlg) };
            let n = dlg.items.len() as i32;
            unsafe {
                match wp as u16 {
                    VK_LEFT => move_selection(&mut dlg, -1),
                    VK_RIGHT => move_selection(&mut dlg, 1),
                    VK_UP_ARROW => move_selection(&mut dlg, -m.cols),
                    VK_DOWN_ARROW => move_selection(&mut dlg, m.cols),
                    VK_PAGE_UP => move_selection(&mut dlg, -m.cols * (m.view_h / CELL_H).max(1)),
                    VK_PAGE_DOWN => move_selection(&mut dlg, m.cols * (m.view_h / CELL_H).max(1)),
                    VK_HOME => move_selection(&mut dlg, -n),
                    VK_END => move_selection(&mut dlg, n),
                    VK_BACKSPACE => go_up(&mut dlg),
                    _ => {}
                }
            }
            0
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

fn lo_word(lp: LPARAM) -> i32 {
    (lp & 0xFFFF) as i16 as i32
}

fn hi_word(lp: LPARAM) -> i32 {
    ((lp >> 16) & 0xFFFF) as i16 as i32
}

// ── Accepting ────────────────────────────────────────────────────────────

unsafe fn accept(dlg: &mut Dlg) {
    unsafe {
        // Enter on a folder means "go there", not "answer with it" — the same
        // thing double-clicking it does.
        match dlg.sel.and_then(|i| dlg.items.get(i)) {
            Some(Item::Up) => return go_up(dlg),
            Some(Item::Folder { path, .. }) => {
                let path = path.clone();
                return navigate(dlg, path, true);
            }
            _ => {}
        }
        match dlg.mode {
            Mode::Open => {
                if let Some(path) = selected_file(dlg) {
                    dlg.result = Some(path.to_string());
                    dlg.done = true;
                } else {
                    // Nothing picked: put the user back in the grid rather than
                    // closing on an empty answer.
                    SetFocus(dlg.grid);
                }
            }
            Mode::Save => {
                let Some(target) = save_target(dlg) else {
                    // No name typed — there is nowhere to save to yet.
                    SetFocus(dlg.name);
                    return;
                };
                if std::path::Path::new(&target).exists() {
                    let msg = format!(
                        "{} already exists.\n\nReplace it?",
                        std::path::Path::new(&target)
                            .file_name()
                            .map(|n| n.to_string_lossy().into_owned())
                            .unwrap_or_else(|| target.clone())
                    );
                    if message_box(dlg.hwnd, &msg, "Save effect as", MB_YESNO | MB_ICONWARNING)
                        != IDYES
                    {
                        return;
                    }
                }
                dlg.result = Some(target);
                dlg.done = true;
            }
        }
    }
}

/// Where a Save would write: the folder being browsed, the typed name, and a
/// `.pkb` extension whether or not the user supplied one.
unsafe fn save_target(dlg: &Dlg) -> Option<String> {
    let typed = unsafe { text_of(dlg.name) };
    let typed = typed.trim();
    if typed.is_empty() {
        return None;
    }
    let mut p = std::path::PathBuf::from(dlg.browser.root());
    for seg in split(&dlg.cwd) {
        p.push(seg);
    }
    p.push(typed);
    if p.extension()
        .map_or(true, |e| !e.eq_ignore_ascii_case("pkb"))
    {
        p.set_extension("pkb");
    }
    Some(p.to_string_lossy().into_owned())
}

// ── Message loop ─────────────────────────────────────────────────────────

unsafe fn pump(cell: &RefCell<Dlg>) {
    unsafe {
        let mut msg: MSG = std::mem::zeroed();
        loop {
            if cell.borrow().done {
                return;
            }
            while PeekMessageW(&mut msg, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
                if msg.message == WM_QUIT {
                    // The host is closing; hand the quit back and give up on
                    // the dialog rather than swallowing it.
                    PostQuitMessage(msg.wParam as i32);
                    cell.borrow_mut().done = true;
                    return;
                }
                // No dialog template, so Enter and Escape are handled here
                // rather than by IsDialogMessage.
                if msg.message == WM_KEYDOWN {
                    match msg.wParam as u16 {
                        13 => {
                            accept(&mut cell.borrow_mut());
                            continue;
                        }
                        27 => {
                            cell.borrow_mut().done = true;
                            return;
                        }
                        _ => {}
                    }
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if cell.borrow().done {
                return;
            }
            // Follow the tree. Done here rather than in the notification so a
            // click can never be lost to a re-entrant message.
            let moved = {
                let dlg = cell.borrow();
                tree_selection(&dlg).filter(|p| *p != dlg.cwd)
            };
            if let Some(path) = moved {
                navigate(&mut cell.borrow_mut(), path, true);
                continue;
            }
            // Thumbnails advance one per iteration; when there are none left
            // the loop sleeps on input instead of spinning. Only the tile that
            // just arrived is repainted — a folder of a hundred effects would
            // otherwise redraw itself a hundred times.
            let mut dlg = cell.borrow_mut();
            match dlg.thumbs.tick() {
                Some(path) => {
                    match dlg.items.iter().position(|i| match i {
                        Item::File { path: p, .. } => *p == path,
                        _ => false,
                    }) {
                        Some(i) => invalidate_cell(&dlg, i),
                        None => {
                            InvalidateRect(dlg.grid, std::ptr::null(), 0);
                        }
                    }
                    drop(dlg);
                }
                None => {
                    drop(dlg);
                    WaitMessage();
                }
            }
        }
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    // Answered without the dialog's state, and so before the borrow: a button
    // repainting must not depend on the dialog being idle.
    match msg {
        WM_DRAWITEM => {
            unsafe { draw_button(&*(lp as *const DRAWITEMSTRUCT)) };
            return 1;
        }
        WM_CTLCOLORSTATIC | WM_CTLCOLOREDIT | WM_CTLCOLORBTN => {
            return unsafe { ctl_colour(msg, wp) };
        }
        // The wheel goes to the focused control, which may be the name box.
        // The grid is the only thing here that scrolls.
        WM_MOUSEWHEEL => {
            let grid = unsafe { GetDlgItem(hwnd, ID_GRID as i32) };
            if !grid.is_null() {
                unsafe { SendMessageW(grid, WM_MOUSEWHEEL, wp, lp) };
                return 0;
            }
        }
        _ => {}
    }

    // SAFETY: set right after creation and never cleared; the cell outlives
    // every message because `run` owns it across the pump.
    let ptr = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *const RefCell<Dlg>;
    if ptr.is_null() {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    }
    let cell = unsafe { &*ptr };
    // A message arriving while the dialog is mid-update is one the dialog
    // raised itself (moving crumb buttons, refilling the grid); skipping it is
    // correct, and it is what makes the `RefCell` sound rather than merely
    // convenient.
    let Ok(mut dlg) = cell.try_borrow_mut() else {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    };

    match msg {
        WM_COMMAND => {
            let id = wp & 0xFFFF;
            // An owner-drawn button notifies on focus changes as well as
            // clicks (BN_SETFOCUS / BN_KILLFOCUS, sent whether or not it has
            // BS_NOTIFY), and they arrive here as the same WM_COMMAND. Acting
            // on the id alone turns tabbing between buttons into pressing
            // them — Cancel among them, which closed the dialog on sight.
            if ((wp >> 16) & 0xFFFF) as u32 != BN_CLICKED {
                return 0;
            }
            unsafe {
                match id {
                    ID_ACCEPT => accept(&mut dlg),
                    ID_CANCEL => dlg.done = true,
                    ID_BACK => go_back(&mut dlg),
                    ID_FWD => go_forward(&mut dlg),
                    ID_UP => go_up(&mut dlg),
                    _ if (ID_CRUMB..ID_CRUMB + MAX_CRUMBS).contains(&id) => {
                        let target = crumb_target(&dlg, id - ID_CRUMB);
                        navigate(&mut dlg, target, true);
                    }
                    _ => {}
                }
            }
            0
        }
        WM_NOTIFY => {
            let hdr = unsafe { &*(lp as *const NMHDR) };
            if hdr.idFrom == ID_TREE {
                let nm = unsafe { &*(lp as *const NMTREEVIEWW) };
                // Selection is not handled here — see `tree_selection`. Only
                // expansion is, because a node has to have its children before
                // the control draws them.
                if hdr.code == TVN_ITEMEXPANDINGW && nm.action == TVE_EXPAND {
                    if let Some(path) = tree_path(&dlg, nm.itemNew.lParam) {
                        unsafe { populate(&mut dlg, &path) };
                    }
                }
            }
            0
        }
        // Separators between breadcrumb buttons, drawn on the bar itself.
        WM_PAINT => {
            unsafe {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let hdc = BeginPaint(hwnd, &mut ps);
                let (w, _) = client_size(hwnd);
                let bar = RECT {
                    left: 0,
                    top: 0,
                    right: w,
                    bottom: PAD + BAR_H,
                };
                fill(hdc, &bar, C_BG);
                SetBkMode(hdc, TRANSPARENT as i32);
                for i in 1..MAX_CRUMBS {
                    let c = GetDlgItem(hwnd, (ID_CRUMB + i) as i32);
                    if c.is_null() || !shown(c) {
                        continue;
                    }
                    let mut r = RECT {
                        left: 0,
                        top: 0,
                        right: 0,
                        bottom: 0,
                    };
                    GetWindowRect(c, &mut r);
                    MapWindowPoints(std::ptr::null_mut(), hwnd, (&mut r as *mut RECT).cast(), 2);
                    let mut gap = RECT {
                        left: r.left - 12,
                        top: r.top,
                        right: r.left,
                        bottom: r.bottom,
                    };
                    draw_text(
                        hdc,
                        &mut gap,
                        ">",
                        C_TEXT_DIM,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                    );
                }
                EndPaint(hwnd, &ps);
            }
            0
        }
        WM_CLOSE => {
            dlg.done = true;
            0
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

// ── Win32 text helpers ───────────────────────────────────────────────────

unsafe fn set_text(hwnd: HWND, text: &str) {
    let wide: Vec<u16> = format!("{text}\0").encode_utf16().collect();
    unsafe { SetWindowTextW(hwnd, wide.as_ptr()) };
}

unsafe fn text_of(hwnd: HWND) -> String {
    unsafe {
        if hwnd.is_null() {
            return String::new();
        }
        let n = GetWindowTextLengthW(hwnd);
        if n <= 0 {
            return String::new();
        }
        let mut buf = vec![0u16; n as usize + 1];
        let got = GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
        String::from_utf16_lossy(&buf[..got.max(0) as usize])
    }
}

unsafe fn message_box(owner: HWND, text: &str, title: &str, flags: MESSAGEBOX_STYLE) -> i32 {
    let t: Vec<u16> = format!("{text}\0").encode_utf16().collect();
    let c: Vec<u16> = format!("{title}\0").encode_utf16().collect();
    unsafe { MessageBoxW(owner, t.as_ptr(), c.as_ptr(), flags) }
}
