// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Modal Open and Save dialogs for corn effects, built on `StorageBrowser` and
// live thumbnails.
//
//   #[path = "common/theme.rs"]  mod theme;
//   #[path = "common/thumbs.rs"] mod thumbs;
//   #[path = "common/effect_dialog.rs"] mod effect_dialog;
//
// from inside a `#[cfg(windows)]` module — it needs all three.
//
// Laid out for getting around a deep tree rather than for one folder at a
// time: a folder tree on the left, effects with thumbnails on the right, and
// above them a breadcrumb whose every segment is a button, plus back/forward
// history. Four ways up and down, and none of them is "climb one level at a
// time through `..`":
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ ‹ › ↑   root › _hd.w3mod › sharedfx › firebrazier            │
//   ├──────────────────┬───────────────────────────────────────────┤
//   │ ▾ root           │  [img]  firebrazier_blue.pkb              │
//   │   ▸ _hd.w3mod    │  [img]  firebrazier_green.pkb             │
//   │   ▾ sharedfx     │  [img]  firebrazier_red.pkb               │
//   ├──────────────────┴───────────────────────────────────────────┤
//   │ File name: [______________________]      [ Save ] [ Cancel ] │
//   └──────────────────────────────────────────────────────────────┘
//
// The tree fills in lazily — a node's children are read the first time it is
// opened, so pointing this at a large storage does not walk all of it up
// front. Only folders live in the tree and only files live in the list, which
// is what makes both panes readable.
//
// Both modes are the same widget with a different bottom bar, because they are
// the same question asked twice: *which* effect. Open answers with one that
// exists; Save answers with a name in a folder, which may or may not exist
// yet. `StorageFileFilter::EffectsOnly` means the list never shows anything
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
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, SIZE, WPARAM};
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{EnableWindow, SetActiveWindow, SetFocus};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use whiteoutflakes::{StorageBrowser, StorageFileFilter, StorageKind};

use super::theme::*;
use super::thumbs::{self, Thumbs, ROW_H};

const ID_TREE: usize = 1;
const ID_LIST: usize = 2;
const ID_BACK: usize = 3;
const ID_FWD: usize = 4;
const ID_UP: usize = 5;
const ID_ACCEPT: usize = 6;
const ID_CANCEL: usize = 7;
const ID_NAME: usize = 8;
const ID_NAME_LBL: usize = 9;
/// Breadcrumb buttons are `ID_CRUMB..ID_CRUMB + MAX_CRUMBS`, created once and
/// shown or hidden as the path gets deeper or shallower.
const ID_CRUMB: usize = 100;
const MAX_CRUMBS: usize = 12;

const LB_ADDSTRING: u32 = 0x0180;
const LB_RESETCONTENT: u32 = 0x0184;
const LB_GETCURSEL: u32 = 0x0188;
const LB_SETITEMHEIGHT: u32 = 0x01A0;
const LBS_NOTIFY: u32 = 0x0001;
const LBS_OWNERDRAWFIXED: u32 = 0x0010;
const LBS_HASSTRINGS: u32 = 0x0040;
const LBN_SELCHANGE: u16 = 1;
const LBN_DBLCLK: u16 = 2;

const DLG_W: i32 = 880;
const DLG_H: i32 = 600;
const PAD: i32 = 10;
const BAR_H: i32 = 34;
const TREE_W: i32 = 300;
const BTN_W: i32 = 92;
const BTN_H: i32 = 28;
const NAV_W: i32 = 30;

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

struct FileRow {
    name: String,
    path: String,
}

struct Dlg<'a> {
    mode: Mode,
    browser: StorageBrowser,
    thumbs: &'a mut Thumbs,

    /// Display path of the folder being shown; empty is the storage root.
    cwd: String,
    files: Vec<FileRow>,
    crumbs: Vec<String>,
    back: Vec<String>,
    fwd: Vec<String>,

    /// Tree bookkeeping. `nodes` is how a path finds its item again when the
    /// user arrives by breadcrumb or history instead of by clicking the tree.
    nodes: HashMap<String, HTREEITEM>,
    populated: HashSet<String>,
    /// `lParam` on a tree item is an index into here — a tree item cannot own
    /// a `String`, and an index survives everything a pointer would not.
    node_paths: Vec<String>,

    hwnd: HWND,
    tree: HWND,
    list: HWND,
    name: HWND,
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

    // A `RefCell` rather than a bare pointer: the tree sends notifications
    // back into the window procedure while the dialog is already being
    // mutated (expanding a node during navigation, say). `try_borrow_mut`
    // turns that re-entrancy into a skipped message instead of aliasing.
    let cell = RefCell::new(Dlg {
        mode,
        browser,
        thumbs,
        cwd: String::new(),
        files: Vec::new(),
        crumbs: Vec::new(),
        back: Vec::new(),
        fwd: Vec::new(),
        nodes: HashMap::new(),
        populated: HashSet::new(),
        node_paths: Vec::new(),
        hwnd: std::ptr::null_mut(),
        tree: std::ptr::null_mut(),
        list: std::ptr::null_mut(),
        name: std::ptr::null_mut(),
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

        let list = child(
            w!("LISTBOX"),
            w!(""),
            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | 0x0200000, // | WS_VSCROLL
            ID_LIST,
        );
        SendMessageW(list, LB_SETITEMHEIGHT, 0, ROW_H as isize);

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
        controls.extend([tree, list, accept, cancel]);

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
            dlg.tree = tree;
            dlg.list = list;
            layout(&dlg);
            seed_tree(&mut dlg);
            navigate(&mut dlg, String::new(), false);
        }
        ShowWindow(hwnd, SW_SHOW);
        SetFocus(if mode == Mode::Save {
            cell.borrow().name
        } else {
            list
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
            ID_LIST,
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
        refill_files(dlg);
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

unsafe fn refill_files(dlg: &mut Dlg) {
    unsafe {
        dlg.files.clear();
        SendMessageW(dlg.list, LB_RESETCONTENT, 0, 0);
        // Leaving a folder drops what has not been drawn yet, so the new one
        // is not stuck behind the old one's backlog.
        dlg.thumbs.forget_queue();

        for name in dlg.browser.files() {
            let wide: Vec<u16> = format!("{name}\0").encode_utf16().collect();
            SendMessageW(dlg.list, LB_ADDSTRING, 0, wide.as_ptr() as isize);
            let path = dlg.browser.child_path(&name);
            dlg.thumbs.request(&path, true);
            dlg.files.push(FileRow { name, path });
        }
        if dlg.files.is_empty() {
            // One row so the pane can say why it is empty. Without it a
            // folder with no effects is indistinguishable from a broken
            // dialog — which is exactly how it read.
            SendMessageW(dlg.list, LB_ADDSTRING, 0, w!("") as isize);
        }
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
    }
}

unsafe fn place_crumbs(dlg: &Dlg, w: i32) {
    unsafe {
        let mut x = PAD + (NAV_W + 4) * 3 + 8;
        let hdc = GetDC(dlg.hwnd);
        for i in 0..MAX_CRUMBS {
            let c = GetDlgItem(dlg.hwnd, (ID_CRUMB + i) as i32);
            if c.is_null() || IsWindowVisible(c) == 0 {
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
        ReleaseDC(dlg.hwnd, hdc);
    }
}

fn crumb_target(dlg: &Dlg, i: usize) -> String {
    if i == 0 {
        return String::new();
    }
    dlg.crumbs[..i.min(dlg.crumbs.len())].join("\\")
}

// ── Folder tree ──────────────────────────────────────────────────────────

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
    (folders, effects)
}

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

/// Make sure the current folder has a tree item, opening every ancestor on
/// the way down, then select it. This is what keeps the tree in step when the
/// user arrives by breadcrumb, history or Up rather than by clicking it.
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

unsafe fn tree_path(dlg: &Dlg, param: LPARAM) -> Option<String> {
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

/// The "this folder has no effects" row. Not selectable in any meaningful
/// sense — `selected` bounds-checks against `files`, which is empty.
unsafe fn empty_row(d: &DRAWITEMSTRUCT) {
    unsafe {
        fill(d.hDC, &d.rcItem, C_LIST);
        SetBkMode(d.hDC, TRANSPARENT as i32);
        SetTextColor(d.hDC, C_TEXT_DIM);
        let mut text: Vec<u16> = "No effects in this folder".encode_utf16().collect();
        let mut r = RECT {
            left: d.rcItem.left + 14,
            ..d.rcItem
        };
        DrawTextW(
            d.hDC,
            text.as_mut_ptr(),
            text.len() as i32,
            &mut r,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE,
        );
    }
}

// ── Accepting ────────────────────────────────────────────────────────────

fn selected(dlg: &Dlg) -> Option<usize> {
    let sel = unsafe { SendMessageW(dlg.list, LB_GETCURSEL, 0, 0) };
    (sel >= 0 && (sel as usize) < dlg.files.len()).then_some(sel as usize)
}

unsafe fn accept(dlg: &mut Dlg) {
    unsafe {
        match dlg.mode {
            Mode::Open => {
                if let Some(f) = selected(dlg).and_then(|i| dlg.files.get(i)) {
                    dlg.result = Some(f.path.clone());
                    dlg.done = true;
                } else {
                    // Nothing picked: put the user back in the list rather
                    // than closing on an empty answer.
                    SetFocus(dlg.list);
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
            // the loop sleeps on input instead of spinning.
            let (drew, list) = {
                let mut dlg = cell.borrow_mut();
                let drew = dlg.thumbs.tick().is_some();
                (drew, dlg.list)
            };
            if drew {
                InvalidateRect(list, std::ptr::null(), 0);
            } else {
                WaitMessage();
            }
        }
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    // SAFETY: set right after creation and never cleared; the cell outlives
    // every message because `run` owns it across the pump.
    let ptr = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *const RefCell<Dlg>;
    if ptr.is_null() {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    }
    let cell = unsafe { &*ptr };
    // A message arriving while the dialog is mid-update is one the dialog
    // raised itself (expanding or selecting a tree node); skipping it is
    // correct, and it is what makes the `RefCell` sound rather than merely
    // convenient.
    let Ok(mut dlg) = cell.try_borrow_mut() else {
        return unsafe { DefWindowProcW(hwnd, msg, wp, lp) };
    };

    match msg {
        WM_COMMAND => {
            let id = wp & 0xFFFF;
            let code = ((wp >> 16) & 0xFFFF) as u16;
            unsafe {
                match id {
                    ID_ACCEPT => accept(&mut dlg),
                    ID_CANCEL => dlg.done = true,
                    ID_BACK => go_back(&mut dlg),
                    ID_FWD => go_forward(&mut dlg),
                    ID_UP => go_up(&mut dlg),
                    ID_LIST if code == LBN_DBLCLK => accept(&mut dlg),
                    // Clicking a file in Save mode fills the name in, the way
                    // every save dialog does.
                    ID_LIST if code == LBN_SELCHANGE && dlg.mode == Mode::Save => {
                        if let Some(f) = selected(&dlg).and_then(|i| dlg.files.get(i)) {
                            let name = f.name.clone();
                            set_text(dlg.name, &name);
                        }
                    }
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
                // expansion is, because a node has to have its children
                // before the control draws them.
                if hdr.code == TVN_ITEMEXPANDINGW && nm.action == TVE_EXPAND {
                    if let Some(path) = unsafe { tree_path(&dlg, nm.itemNew.lParam) } {
                        unsafe { populate(&mut dlg, &path) };
                    }
                }
            }
            0
        }
        WM_DRAWITEM => {
            let d = unsafe { &*(lp as *const DRAWITEMSTRUCT) };
            unsafe {
                if d.CtlType != ODT_LISTBOX {
                    draw_button(d);
                    return 1;
                }
                match dlg.files.get(d.itemID as usize) {
                    Some(f) => {
                        let cell = match dlg.thumbs.get(&f.path) {
                            Some(b) if !b.is_empty() => thumbs::Cell::Image(b),
                            Some(_) => thumbs::Cell::Failed,
                            None => thumbs::Cell::Pending,
                        };
                        thumbs::draw_row(d, &f.name, cell, false);
                    }
                    None => empty_row(d),
                }
            }
            1
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
                SetTextColor(hdc, C_TEXT_DIM);
                SetBkMode(hdc, TRANSPARENT as i32);
                let mut sep: Vec<u16> = ">".encode_utf16().collect();
                for i in 1..MAX_CRUMBS {
                    let c = GetDlgItem(hwnd, (ID_CRUMB + i) as i32);
                    if c.is_null() || IsWindowVisible(c) == 0 {
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
                    DrawTextW(
                        hdc,
                        sep.as_mut_ptr(),
                        sep.len() as i32,
                        &mut gap,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                    );
                }
                EndPaint(hwnd, &ps);
            }
            0
        }
        WM_CTLCOLORLISTBOX | WM_CTLCOLORSTATIC | WM_CTLCOLOREDIT | WM_CTLCOLORBTN => unsafe {
            ctl_colour(msg, wp)
        },
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
