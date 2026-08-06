// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The dark chrome the GUI examples share: palette, UI font, and the two bits
// of painting that stock Win32 controls will not do for themselves.
//
//   #[path = "common/theme.rs"]
//   mod theme;
//
// from inside a `#[cfg(windows)]` module.

#![allow(dead_code)] // each example uses a different part of this

use windows_sys::Win32::Foundation::{HWND, LRESULT, RECT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::UI::Controls::{DRAWITEMSTRUCT, ODS_SELECTED};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

// COLORREF is 0x00BBGGRR, so these read backwards from hex web colours.
pub const C_BG: u32 = 0x001E1B18;
pub const C_LIST: u32 = 0x00241F1C;
pub const C_BTN: u32 = 0x002B2724;
pub const C_BTN_HOT: u32 = 0x003A342F;
pub const C_TEXT: u32 = 0x00D8D0C8;
pub const C_TEXT_DIM: u32 = 0x00908880;
pub const C_ACCENT: u32 = 0x00E0A050;
/// Folder glyphs, the same gold the storage explorer draws them in — a folder
/// should not be the same colour as a selection.
pub const C_FOLDER: u32 = 0x0059B8D9;

/// A stock button cannot be made dark, so every button in these examples is
/// owner-drawn and comes through [`draw_button`]. windows-sys types the style
/// as `i32`; the create calls want `u32`.
pub const OWNERDRAW: u32 = BS_OWNERDRAW as u32;

pub unsafe fn ui_font() -> HFONT {
    unsafe {
        let mut ncm: NONCLIENTMETRICSW = std::mem::zeroed();
        ncm.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
        if SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            ncm.cbSize,
            (&mut ncm as *mut NONCLIENTMETRICSW).cast(),
            0,
        ) != 0
        {
            let f = CreateFontIndirectW(&ncm.lfMessageFont);
            if !f.is_null() {
                return f;
            }
        }
        GetStockObject(DEFAULT_GUI_FONT) as HFONT
    }
}

pub unsafe fn fill(hdc: HDC, r: &RECT, colour: u32) {
    unsafe {
        let b = CreateSolidBrush(colour);
        FillRect(hdc, r, b);
        DeleteObject(b as _);
    }
}

/// Paint one owner-drawn button using its own window text.
pub unsafe fn draw_button(d: &DRAWITEMSTRUCT) {
    unsafe {
        let hot = (d.itemState & ODS_SELECTED) != 0;
        fill(d.hDC, &d.rcItem, if hot { C_BTN_HOT } else { C_BTN });

        let mut text = [0u16; 64];
        let n = GetWindowTextW(d.hwndItem, text.as_mut_ptr(), 64);
        SetTextColor(d.hDC, if hot { C_ACCENT } else { C_TEXT });
        SetBkMode(d.hDC, TRANSPARENT as i32);
        let mut r = d.rcItem;
        DrawTextW(
            d.hDC,
            text.as_mut_ptr(),
            n,
            &mut r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE,
        );
    }
}

/// Answer `WM_CTLCOLOR*` so listboxes, statics and edits take the dark
/// palette. The brushes are cached rather than leaked: the system does not
/// free what is returned, so a fresh one each time would bleed GDI objects.
pub unsafe fn ctl_colour(msg: u32, wparam: WPARAM) -> LRESULT {
    unsafe {
        let hdc = wparam as HDC;
        let sunken = msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT;
        SetTextColor(hdc, if sunken { C_TEXT } else { C_TEXT_DIM });
        SetBkColor(hdc, if sunken { C_LIST } else { C_BG });

        static mut SUNKEN_BRUSH: HBRUSH = std::ptr::null_mut();
        static mut BG_BRUSH: HBRUSH = std::ptr::null_mut();
        let slot = if sunken {
            &raw mut SUNKEN_BRUSH
        } else {
            &raw mut BG_BRUSH
        };
        if (*slot).is_null() {
            *slot = CreateSolidBrush(if sunken { C_LIST } else { C_BG });
        }
        *slot as LRESULT
    }
}

pub unsafe fn client_size(hwnd: HWND) -> (i32, i32) {
    let mut r = RECT {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
    };
    unsafe { GetClientRect(hwnd, &mut r) };
    (r.right - r.left, r.bottom - r.top)
}
