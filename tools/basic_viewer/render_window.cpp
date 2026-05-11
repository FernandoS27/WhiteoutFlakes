#include "render_window.h"
#include "whiteout/flakes/types.h"
#include "renderer/render_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/frame_ticker.h"
#include "settings_ini.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/particle/splat_service.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "whiteout/flakes/util/replaceable_paths.h"
#include "resource.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#pragma comment(lib, "comctl32.lib")

#pragma comment(lib, "comdlg32.lib")

namespace whiteout::flakes {

using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

static const wchar_t* WINDOW_CLASS   = L"WhiteoutDexRendererClass";
static const wchar_t* RENDER_CLASS   = L"WhiteoutDexRenderSurface";
static const wchar_t* SETTINGS_CLASS = L"WhiteoutDexSettingsWindow";
static const wchar_t* WINDOW_TITLE   = L"WhiteoutFlakes";

RenderWindow::RenderWindow(RenderService& service) : service_(service) {}

RenderWindow::~RenderWindow() { Close(); Destroy(); }

bool RenderWindow::Open(i32 w, i32 h, gfx::GfxApi api) {
    if (running_) return true;

    if (renderThread_.joinable()) renderThread_.join();
    running_ = true;
    initialized_ = false;
    renderThread_ = std::thread(&RenderWindow::ThreadFunc, this, w, h, api);
    for (i32 i = 0; i < 500 && !initialized_ && running_; ++i) Sleep(10);
    return initialized_;
}

void RenderWindow::Close() {
    running_ = false;
    if (renderThread_.joinable()) {
        if (hwnd_) PostMessage(hwnd_, WM_CLOSE, 0, 0);
        renderThread_.join();
    }
}

bool RenderWindow::IsOpen() const { return running_ && initialized_; }

void RenderWindow::ThreadFunc(i32 w, i32 h, gfx::GfxApi api) {
    if (!Create(w, h))              { running_ = false; return; }
    if (!service_.Pipeline().InitDevice(api))  { running_ = false; Destroy(); return; }

    targetId_ = service_.Pipeline().CreateSwapChainTarget(static_cast<void*>(hwndRender_), w, h);
    if (targetId_ == 0)           { running_ = false; service_.Pipeline().Shutdown(); Destroy(); return; }
    service_.Pipeline().SetPrimaryTarget(targetId_);

    Show();
    initialized_ = true;

    LARGE_INTEGER freq, lastTime, now, fpsTimer;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    fpsTimer = lastTime;
    i32 frameCount = 0;
    i32 lastParentTimeMs = service_.Scene().GetAnimationTime();

    while (running_) {
        if (!PumpMessages()) { running_ = false; break; }

        QueryPerformanceCounter(&now);
        lastTime = now;

        i32 curParentMs = service_.Scene().GetAnimationTime();
        i32 parentDtMs  = curParentMs - lastParentTimeMs;
        if (parentDtMs < 0)   parentDtMs = 0;
        if (parentDtMs > 100) parentDtMs = 100;
        lastParentTimeMs = curParentMs;
        f32 parentDt = (f32)parentDtMs / 1000.0f;

        UpdateCameraPresetAnimator();
        if (service_.Replaceables().ConsumeDirty()) InvalidateTeamColorSwatch();

        (void)service_.Settings().ConsumeRenderModeDirty();

        service_.Ticker().Tick(parentDt);
        service_.Pipeline().RenderFrame(targetId_);
        service_.Pipeline().Present(targetId_);
        frameCount++;

        f64 fpsDt = (f64)(now.QuadPart - fpsTimer.QuadPart) / freq.QuadPart;
        if (fpsDt >= 1.0) {
            i32 nGeo = 0, nTex = 0, nNodes = 0, nParts = 0, nSegs = 0;
            service_.Pipeline().GetFrameStats(nGeo, nTex, nNodes, nParts, nSegs);
            wchar_t title[300];
            swprintf_s(title,
                L"WhiteoutFlakes \u2014 %d FPS | %d geo, %d tex, %d nodes, %d parts, %d segs",
                frameCount, nGeo, nTex, nNodes, nParts, nSegs
            );
            SetTitle(title);
            frameCount = 0;
            fpsTimer = now;
        }
    }

    service_.Pipeline().Shutdown();
    Destroy();
    initialized_ = false;
}

bool RenderWindow::Create(i32 w, i32 h) {

    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)&RenderWindow::WndProc, &hMod);
    HINSTANCE hInst = hMod ? (HINSTANCE)hMod : GetModuleHandle(nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    icon_ = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WHITEOUT_ICON));

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = RenderWindow::WndProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon          = icon_;
    wc.hIconSm        = icon_;
    wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName  = WINDOW_CLASS;
    if (!RegisterClassExW(&wc))
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW rc = {};
    rc.cbSize        = sizeof(rc);
    rc.style         = CS_HREDRAW | CS_VREDRAW;
    rc.lpfnWndProc   = RenderWindow::RenderWndProc;
    rc.hInstance      = hInst;
    rc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    rc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    rc.lpszClassName  = RENDER_CLASS;
    if (!RegisterClassExW(&rc))
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    DisplayFlags df = service_.Settings().GetDisplayFlags();
    hMenuBar_      = CreateMenu();
    hMenuView_     = CreatePopupMenu();
    hMenuDebug_    = CreatePopupMenu();
    hMenuDebugVis_ = CreatePopupMenu();
    hMenuLod_      = CreatePopupMenu();

    auto addToggle = [](HMENU m, UINT id, const wchar_t* label, bool checked) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id, label);
    };
    addToggle(hMenuView_, IDM_VIEW_GRID,      L"Grid",      df.showGrid);
    addToggle(hMenuView_, IDM_VIEW_PARTICLES, L"Particles", df.showParticles);
    addToggle(hMenuView_, IDM_VIEW_RIBBONS,   L"Ribbons",   df.showRibbons);
    addToggle(hMenuView_, IDM_VIEW_EVENTS,    L"Event Objects", df.showEvents);
    AppendMenuW(hMenuView_, MF_SEPARATOR, 0, nullptr);

    hMenuTileset_ = CreatePopupMenu();
    {
        const i32 n = static_cast<i32>(whiteout::flakes::io::Tileset::Count);
        for (i32 i = 0; i < n; ++i) {
            const char* nm = whiteout::flakes::io::TilesetName(
                static_cast<whiteout::flakes::io::Tileset>(i));
            wchar_t wbuf[64];
            MultiByteToWideChar(CP_UTF8, 0, nm, -1, wbuf, 64);
            AppendMenuW(hMenuTileset_, MF_STRING, IDM_TILESET_BASE + i, wbuf);
        }
        const i32 curIdx = static_cast<i32>(whiteout::flakes::io::GetCurrentTileset());
        CheckMenuRadioItem(hMenuTileset_,
                           IDM_TILESET_BASE, IDM_TILESET_LAST,
                           IDM_TILESET_BASE + curIdx, MF_BYCOMMAND);
    }
    AppendMenuW(hMenuView_, MF_POPUP | MF_STRING,
                (UINT_PTR)hMenuTileset_, L"Tileset");

    addToggle(hMenuDebug_, IDM_DBG_COLLISIONS, L"Collision Markers", df.showCollisions);
    addToggle(hMenuDebug_, IDM_DBG_LIGHTS,     L"Light Markers",     df.showLights);
    AppendMenuW(hMenuDebug_, MF_SEPARATOR, 0, nullptr);

    static const wchar_t* const kDebugVisLabels[8] = {
        L"Off",
        L"Albedo",
        L"World Normal",
        L"LOD Heatmap",
        L"Light Count",
        L"Shading Only (white albedo)",
        L"Shading Only (grey albedo)",
        L"Specular Only (black albedo)",
    };
    for (i32 i = 0; i < 8; ++i)
        AppendMenuW(hMenuDebugVis_, MF_STRING, IDM_DBGVIS_BASE + i, kDebugVisLabels[i]);
    const i32 initDbg = service_.Settings().HdDebugMode();
    CheckMenuRadioItem(hMenuDebugVis_,
                       IDM_DBGVIS_BASE, IDM_DBGVIS_BASE + 7,
                       IDM_DBGVIS_BASE + (initDbg >= 0 && initDbg < 8 ? initDbg : 0),
                       MF_BYCOMMAND);
    AppendMenuW(hMenuDebug_, MF_POPUP | MF_STRING, (UINT_PTR)hMenuDebugVis_, L"Debug View");

    static const wchar_t* const kLodLabels[5] = {
        L"Auto (screen size)",
        L"Force LOD 0 (base)",
        L"Force LOD 1",
        L"Force LOD 2",
        L"Force LOD 3 (lowest)",
    };
    for (i32 i = 0; i < 5; ++i)
        AppendMenuW(hMenuLod_, MF_STRING, IDM_LOD_BASE + i, kLodLabels[i]);
    const i32 initLod = service_.Settings().LodOverride();
    const i32 lodCheckIdx = (initLod < 0) ? 0 : (1 + std::clamp(initLod, 0, 3));
    CheckMenuRadioItem(hMenuLod_,
                       IDM_LOD_BASE, IDM_LOD_LAST,
                       IDM_LOD_BASE + lodCheckIdx, MF_BYCOMMAND);
    AppendMenuW(hMenuDebug_, MF_POPUP | MF_STRING, (UINT_PTR)hMenuLod_, L"LOD");

    AppendMenuW(hMenuBar_, MF_POPUP | MF_STRING, (UINT_PTR)hMenuView_,  L"&View");
    AppendMenuW(hMenuBar_, MF_POPUP | MF_STRING, (UINT_PTR)hMenuDebug_, L"&Debug");

    AppendMenuW(hMenuBar_, MF_STRING, IDM_SETTINGS, L"&Settings");

    RECT adj = {0, 0, w, h + kToolbarH};
    AdjustWindowRect(&adj, WS_OVERLAPPEDWINDOW, TRUE);
    hwnd_ = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            adj.right - adj.left, adj.bottom - adj.top,
                            nullptr, hMenuBar_, hInst, this);
    if (!hwnd_) return false;

    hwndRender_ = CreateWindowExW(0, RENDER_CLASS, L"", WS_CHILD | WS_VISIBLE,
                                   0, kToolbarH, w, h,
                                   hwnd_, nullptr, hInst, this);
    if (!hwndRender_) return false;

    i32 x = 8;
    lblSequence_ = CreateWindowW(L"STATIC", L"Animation:",
        WS_CHILD | SS_CENTERIMAGE,
        x, 4, 64, 20, hwnd_, nullptr, hInst, nullptr);
    x += 66;
    cmbSequence_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, 2, 180, 300, hwnd_, (HMENU)(INT_PTR)IDC_SEQUENCE, hInst, nullptr);
    x += 188;

    CreateWindowW(L"STATIC", L"Camera:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        x, 4, 50, 20, hwnd_, nullptr, hInst, nullptr);
    x += 52;

    cmbCamera_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, 2, 110, 200, hwnd_, (HMENU)(INT_PTR)IDC_CAMERA, hInst, nullptr);
    SendMessageW(cmbCamera_, CB_ADDSTRING, 0, (LPARAM)L"Free Camera");
    SendMessageW(cmbCamera_, CB_SETCURSEL, 0, 0);
    x += 118;

    CreateWindowW(L"STATIC", L"Team:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        x, 4, 36, 20, hwnd_, nullptr, hInst, nullptr);
    x += 38;
    btnTeamColor_ = CreateWindowW(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, 4, 22, 20, hwnd_, (HMENU)(INT_PTR)IDC_TEAMCOLOR, hInst, nullptr);
    x += 30;

    CreateWindowW(L"STATIC", L"Lighting:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        x, 4, 56, 20, hwnd_, nullptr, hInst, nullptr);
    x += 58;
    cmbLighting_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, 2, 100, 200, hwnd_, (HMENU)(INT_PTR)IDC_LIGHTING, hInst, nullptr);
    SendMessageW(cmbLighting_, CB_ADDSTRING, 0, (LPARAM)L"InGame");
    SendMessageW(cmbLighting_, CB_ADDSTRING, 0, (LPARAM)L"Glue");
    SendMessageW(cmbLighting_, CB_ADDSTRING, 0, (LPARAM)L"Dynamic");
    SendMessageW(cmbLighting_, CB_SETCURSEL,
                 static_cast<WPARAM>(service_.Settings().GetLightingMode()), 0);
    x += 108;

    return true;
}

void RenderWindow::Destroy() {

    hwndSettings_ = nullptr;
    btnBgColor_   = nullptr;
    sldExposure_  = nullptr;
    lblExposure_  = nullptr;
    sldSndVolume_ = nullptr;
    lblSndVolume_ = nullptr;
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    hwndRender_ = nullptr;
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    UnregisterClassW(WINDOW_CLASS,   GetModuleHandle(nullptr));
    UnregisterClassW(RENDER_CLASS,   GetModuleHandle(nullptr));
    UnregisterClassW(SETTINGS_CLASS, GetModuleHandle(nullptr));
}

void RenderWindow::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
}

bool RenderWindow::PumpMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

LRESULT CALLBACK RenderWindow::RenderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<RenderWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RenderWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEMOVE:   case WM_MOUSEWHEEL:
        return RenderWindow::WndProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK RenderWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<RenderWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RenderWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK RenderWindow::SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<RenderWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RenderWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleSettingsMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT RenderWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        i32 w = LOWORD(lParam), h = HIWORD(lParam);
        i32 renderH = h - kToolbarH;
        if (w > 0 && renderH > 0) {
            if (hwndRender_) MoveWindow(hwndRender_, 0, kToolbarH, w, renderH, TRUE);
            if (service_.Pipeline().IsDeviceReady()) service_.Pipeline().ResizePrimaryTarget(w, renderH);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        i32 mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        i32 vcHit = service_.Debug().HitTestViewCube(mx, my);
        if (vcHit >= 0) {
            if (vcHit == 6) service_.Scene().Camera().Reset();
            else            service_.Scene().Camera().SnapToViewCubeFace(vcHit);
            return 0;
        }
        lmbDown_ = true;
        lastMouse_ = {mx, my};
        SetCapture(hwnd); return 0;
    }
    case WM_LBUTTONUP:
        lmbDown_ = false;
        if (!rmbDown_ && !mmbDown_) ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        rmbDown_ = true;
        lastMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd); return 0;
    case WM_RBUTTONUP:
        rmbDown_ = false;
        if (!lmbDown_ && !mmbDown_) ReleaseCapture(); return 0;
    case WM_MBUTTONDOWN:
        mmbDown_ = true;
        lastMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd); return 0;
    case WM_MBUTTONUP:
        mmbDown_ = false;
        if (!lmbDown_ && !rmbDown_) ReleaseCapture(); return 0;
    case WM_MOUSEMOVE: {
        POINT cur = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        i32 dx = cur.x - lastMouse_.x, dy = cur.y - lastMouse_.y;
        lastMouse_ = cur;

        auto vcr = service_.Debug().GetViewCubeRect();
        service_.Debug().SetViewCubeHovered(
            cur.x >= vcr.left && cur.x <= vcr.right &&
            cur.y >= vcr.top  && cur.y <= vcr.bottom);
        bool locked;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            locked = cameraLocked_;
        }
        if (!locked) {
            auto& cam = service_.Scene().Camera();
            if (lmbDown_) cam.Rotate(dx, dy);
            if (rmbDown_) cam.Pan(-dx, dy);
            if (mmbDown_) cam.ZoomSmooth(f32(dy) * cam.GetDistance() / whiteout::flakes::renderer::Camera::kFactorRelDist);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        bool locked;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            locked = cameraLocked_;
        }
        if (!locked) {
            i32 delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            service_.Scene().Camera().Zoom(delta * 30);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        return 0;
    }
    case WM_DRAWITEM: {

        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == IDC_TEAMCOLOR) {
            auto* focus = service_.Scene().Actors().Find(
                focusActor_.load(std::memory_order_relaxed));
            COLORREF tc = focus ? (COLORREF)(focus->teamColor & 0x00FFFFFFu)
                                : RGB(255, 0, 0);
            HBRUSH brush = CreateSolidBrush(tc);
            FillRect(dis->hDC, &dis->rcItem, brush);
            DeleteObject(brush);
            DrawEdge(dis->hDC, &dis->rcItem, EDGE_SUNKEN, BF_RECT);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        i32 id = LOWORD(wParam);
        i32 code = HIWORD(wParam);

        auto toggleView = [&](UINT menuId, bool DisplayFlags::*field) {
            DisplayFlags df = service_.Settings().GetDisplayFlags();
            bool& v = df.*field;
            v = !v;
            CheckMenuItem(hMenuBar_, menuId, MF_BYCOMMAND | (v ? MF_CHECKED : MF_UNCHECKED));
            service_.Settings().SetDisplayFlags(df);
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
        };

        if (id == IDM_SETTINGS) {
            EnsureSettingsWindow();
            if (hwndSettings_) {
                ShowWindow(hwndSettings_, SW_SHOW);
                SetForegroundWindow(hwndSettings_);
            }
            return 0;
        }
        if (id == IDM_VIEW_GRID)        { toggleView(id, &DisplayFlags::showGrid);       return 0; }
        if (id == IDM_VIEW_PARTICLES)   { toggleView(id, &DisplayFlags::showParticles);  return 0; }
        if (id == IDM_VIEW_RIBBONS)     { toggleView(id, &DisplayFlags::showRibbons);    return 0; }
        if (id == IDM_VIEW_EVENTS)      { toggleView(id, &DisplayFlags::showEvents);     return 0; }
        if (id == IDM_DBG_COLLISIONS)   { toggleView(id, &DisplayFlags::showCollisions); return 0; }
        if (id == IDM_DBG_LIGHTS)       { toggleView(id, &DisplayFlags::showLights);     return 0; }

        if (id >= (i32)IDM_TILESET_BASE && id <= (i32)IDM_TILESET_LAST) {
            const i32 idx = id - IDM_TILESET_BASE;
            const i32 n   = static_cast<i32>(whiteout::flakes::io::Tileset::Count);
            if (idx < 0 || idx >= n) return 0;
            CheckMenuRadioItem(hMenuTileset_, IDM_TILESET_BASE, IDM_TILESET_LAST,
                               id, MF_BYCOMMAND);
            service_.Replaceables().SetTileset(static_cast<whiteout::flakes::io::Tileset>(idx));
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }

        if (id >= (i32)IDM_DBGVIS_BASE && id <= (i32)IDM_DBGVIS_LAST) {
            const i32 mode = id - IDM_DBGVIS_BASE;
            CheckMenuRadioItem(hMenuDebugVis_, IDM_DBGVIS_BASE, IDM_DBGVIS_LAST, id, MF_BYCOMMAND);
            service_.Settings().SetHdDebugMode(mode);
            return 0;
        }

        if (id >= (i32)IDM_LOD_BASE && id <= (i32)IDM_LOD_LAST) {
            const i32 idx = id - IDM_LOD_BASE;
            CheckMenuRadioItem(hMenuLod_, IDM_LOD_BASE, IDM_LOD_LAST, id, MF_BYCOMMAND);
            service_.Settings().SetLodOverride(idx == 0 ? -1 : (idx - 1));
            return 0;
        }

        switch (id) {
            case IDC_TEAMCOLOR: {
                auto* focus = service_.Scene().Actors().Find(
                    focusActor_.load(std::memory_order_relaxed));
                if (!focus) break;
                CHOOSECOLORW cc = {};
                static COLORREF customColors[16] = {};
                cc.lStructSize = sizeof(cc);
                cc.hwndOwner = hwnd_;
                cc.rgbResult = (COLORREF)(focus->teamColor & 0x00FFFFFFu);
                cc.lpCustColors = customColors;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) {
                    focus->SetTeamColor(GetRValue(cc.rgbResult),
                                        GetGValue(cc.rgbResult),
                                        GetBValue(cc.rgbResult));
                    InvalidateRect(btnTeamColor_, nullptr, TRUE);
                }
                break;
            }
            case IDC_CAMERA: {
                if (code == CBN_SELCHANGE) {
                    i32 sel = (i32)SendMessageW(cmbCamera_, CB_GETCURSEL, 0, 0);
                    ActivateCameraPreset(sel <= 0 ? -1 : sel - 1);
                }
                break;
            }
            case IDC_SEQUENCE: {
                if (code == CBN_SELCHANGE) {
                    const i32 sel = (i32)SendMessageW(cmbSequence_, CB_GETCURSEL, 0, 0);
                    if (sel < 0) break;
                    auto* focus = service_.Scene().Actors().Find(
                        focusActor_.load(std::memory_order_relaxed));
                    const i32 prev = focus ? focus->animation.ActiveSequenceIndex() : 0;
                    if (focus) focus->animation.SetActiveSequenceIndex(sel);
                    if (sel == prev) break;

                    auto containsCi = [](const std::string& hay, const char* needle) {
                        const usize hn = hay.size(), nn = std::strlen(needle);
                        if (nn == 0 || hn < nn) return false;
                        for (usize i = 0; i + nn <= hn; ++i) {
                            bool ok = true;
                            for (usize j = 0; j < nn; ++j) {
                                const char a = (char)std::tolower((unsigned char)hay[i + j]);
                                const char b = (char)std::tolower((unsigned char)needle[j]);
                                if (a != b) { ok = false; break; }
                            }
                            if (ok) return true;
                        }
                        return false;
                    };
                    bool keep = false;
                    {
                        std::lock_guard<std::mutex> lk(hostMutex_);
                        if (sel < (i32)sequenceNames_.size()) {
                            const std::string& name = sequenceNames_[sel];
                            keep = containsCi(name, "decay") || containsCi(name, "dissipate");
                        }
                    }
                    if (!keep) service_.Splats().Clear();
                }
                break;
            }
            case IDC_LIGHTING: {
                if (code == CBN_SELCHANGE) {
                    i32 sel = (i32)SendMessageW(cmbLighting_, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel <= 2)
                        service_.Settings().SetLightingMode(static_cast<LightingMode>(sel));
                }
                break;
            }

        }
        return 0;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        running_ = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RenderWindow::SetCameraPresets(std::vector<CameraPreset> presets) {
    if (cmbCamera_) {
        SendMessageW(cmbCamera_, CB_RESETCONTENT, 0, 0);
        SendMessageW(cmbCamera_, CB_ADDSTRING, 0, (LPARAM)L"Free Camera");
        for (auto& p : presets)
            SendMessageW(cmbCamera_, CB_ADDSTRING, 0, (LPARAM)p.name.c_str());
        SendMessageW(cmbCamera_, CB_SETCURSEL, 0, 0);
    }
    std::lock_guard<std::mutex> lk(hostMutex_);
    cameraPresets_         = std::move(presets);
    activeCameraPresetIdx_ = -1;
    cameraLocked_          = false;
}

void RenderWindow::SetSequences(std::vector<std::string> names,
                                std::vector<SequenceInfo> ranges) {
    if (cmbSequence_) {
        SendMessageW(cmbSequence_, CB_RESETCONTENT, 0, 0);
        for (auto& n : names) {
            std::wstring wn(n.begin(), n.end());
            SendMessageW(cmbSequence_, CB_ADDSTRING, 0, (LPARAM)wn.c_str());
        }
        if (!names.empty()) {
            ShowWindow(lblSequence_, SW_SHOW);
            ShowWindow(cmbSequence_, SW_SHOW);
            SendMessageW(cmbSequence_, CB_SETCURSEL, 0, 0);
            if (auto* focus = service_.Scene().Actors().Find(
                    focusActor_.load(std::memory_order_relaxed)))
                focus->animation.SetActiveSequenceIndex(0);
        }
    }
    std::lock_guard<std::mutex> lk(hostMutex_);
    sequenceNames_  = std::move(names);
    sequenceRanges_ = std::move(ranges);
}

std::vector<SequenceInfo> RenderWindow::SequenceRanges() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return sequenceRanges_;
}

void RenderWindow::ActivateCameraPreset(i32 idx) {
    auto& cam = service_.Scene().Camera();

    // Snapshot the preset under lock so we don't hold it across the
    // animator() callback (which can run arbitrary user code).
    CameraPreset preset;
    bool         haveValidPreset = false;
    {
        std::lock_guard<std::mutex> lk(hostMutex_);
        if (idx < 0 || idx >= (i32)cameraPresets_.size()) {
            activeCameraPresetIdx_ = -1;
            cameraLocked_          = false;
        } else {
            preset                 = cameraPresets_[idx];
            activeCameraPresetIdx_ = idx;
            cameraLocked_          = preset.isLive;
            haveValidPreset        = true;
        }
    }

    if (!haveValidPreset) {
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        return;
    }

    Vector3f pos  = preset.position;
    Vector3f tgt  = preset.target;
    f32      roll = preset.staticRoll;
    if (preset.animator) {
        i32 seqStart = 0, seqEnd = 0;
        auto* focus = service_.Scene().Actors().Find(
            focusActor_.load(std::memory_order_relaxed));
        i32   seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            if (seqIdx >= 0 && seqIdx < (i32)sequenceRanges_.size()) {
                seqStart = sequenceRanges_[seqIdx].startMs;
                seqEnd   = sequenceRanges_[seqIdx].endMs;
            }
        }
        if (seqStart == 0 && seqEnd == 0) seqEnd = 1 << 30;
        const i32 sampleMs = focus ? focus->animation.TimeMs()
                                   : service_.Scene().GetAnimationTime();
        preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }

    cam.SetDirectPose(pos, tgt, roll);
    const f32 fov = (preset.fovDiagonal > 1e-3f) ? preset.fovDiagonal
                                                 : Camera::kDefaultFovDiagonal;
    cam.SetFovDiagonal(fov);
    cam.SetClip(preset.zNear, preset.zFar);
}

void RenderWindow::UpdateCameraPresetAnimator() {
    CameraPreset preset;
    i32 seqStart = 0, seqEnd = 0;
    {
        std::lock_guard<std::mutex> lk(hostMutex_);
        if (activeCameraPresetIdx_ < 0
            || activeCameraPresetIdx_ >= (i32)cameraPresets_.size()) return;
        preset = cameraPresets_[activeCameraPresetIdx_];
        if (!preset.animator) return;

        auto* focus = service_.Scene().Actors().Find(
            focusActor_.load(std::memory_order_relaxed));
        const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        if (seqIdx >= 0 && seqIdx < (i32)sequenceRanges_.size()) {
            seqStart = sequenceRanges_[seqIdx].startMs;
            seqEnd   = sequenceRanges_[seqIdx].endMs;
        }
    }
    if (seqStart == 0 && seqEnd == 0) seqEnd = 1 << 30;

    auto* focus = service_.Scene().Actors().Find(
        focusActor_.load(std::memory_order_relaxed));
    Vector3f pos  = preset.position;
    Vector3f tgt  = preset.target;
    f32      roll = preset.staticRoll;
    const i32 sampleMs = focus ? focus->animation.TimeMs()
                               : service_.Scene().GetAnimationTime();
    preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    service_.Scene().Camera().SetDirectPose(pos, tgt, roll);
}

void RenderWindow::SetTitle(const wchar_t* title) {
    if (hwnd_) SetWindowTextW(hwnd_, title);
}

void RenderWindow::InvalidateTeamColorSwatch() {
    if (btnTeamColor_) InvalidateRect(btnTeamColor_, nullptr, TRUE);
}

i32 RenderWindow::GetActiveCameraIndex() const {
    return cmbCamera_ ? (i32)SendMessageW(cmbCamera_, CB_GETCURSEL, 0, 0) : 0;
}

void RenderWindow::SyncViewMenuFromService() {
    if (!hMenuView_) return;
    const DisplayFlags df = service_.Settings().GetDisplayFlags();
    auto syncToggle = [&](UINT id, bool checked) {
        CheckMenuItem(hMenuBar_, id,
                      MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    };
    syncToggle(IDM_VIEW_GRID,      df.showGrid);
    syncToggle(IDM_VIEW_PARTICLES, df.showParticles);
    syncToggle(IDM_VIEW_RIBBONS,   df.showRibbons);
    syncToggle(IDM_VIEW_EVENTS,    df.showEvents);
    syncToggle(IDM_DBG_COLLISIONS, df.showCollisions);
    syncToggle(IDM_DBG_LIGHTS,     df.showLights);
    if (hMenuTileset_) {
        const i32 n      = static_cast<i32>(whiteout::flakes::io::Tileset::Count);
        const i32 curIdx = std::clamp(static_cast<i32>(whiteout::flakes::io::GetCurrentTileset()), 0, n - 1);
        CheckMenuRadioItem(hMenuTileset_,
                           IDM_TILESET_BASE, IDM_TILESET_LAST,
                           IDM_TILESET_BASE + curIdx, MF_BYCOMMAND);
    }

    if (hwnd_) DrawMenuBar(hwnd_);
}

void RenderWindow::EnsureSettingsWindow() {
    if (hwndSettings_) return;

    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)&RenderWindow::SettingsWndProc, &hMod);
    HINSTANCE hInst = hMod ? (HINSTANCE)hMod : GetModuleHandle(nullptr);

    WNDCLASSEXW sc = {};
    sc.cbSize        = sizeof(sc);
    sc.style         = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc   = RenderWindow::SettingsWndProc;
    sc.hInstance     = hInst;
    sc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    sc.hIcon         = icon_;
    sc.hIconSm       = icon_;
    sc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    sc.lpszClassName = SETTINGS_CLASS;
    if (!RegisterClassExW(&sc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    constexpr i32 kClientW = 380;
    constexpr i32 kClientH = 502;
    RECT rc = {0, 0, kClientW, kClientH};

    const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOOLWINDOW;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    i32 x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (hwnd_) {
        RECT pr{};
        GetWindowRect(hwnd_, &pr);
        x = pr.left + 60;
        y = pr.top  + 60;
    }

    hwndSettings_ = CreateWindowExW(exStyle, SETTINGS_CLASS, L"Settings",
                                    style, x, y,
                                    rc.right - rc.left, rc.bottom - rc.top,
                                    hwnd_, nullptr, hInst, this);
    if (!hwndSettings_) return;

    i32 rowY = 12;
    CreateWindowW(L"STATIC", L"Background:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    btnBgColor_ = CreateWindowW(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        108, rowY + 1, 28, 20, hwndSettings_,
        (HMENU)(INT_PTR)IDC_BGCOLOR, hInst, nullptr);

    rowY += 38;
    CreateWindowW(L"STATIC", L"Exposure:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    sldExposure_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        108, rowY, 150, 24, hwndSettings_,
        (HMENU)(INT_PTR)IDC_EXPOSURE, hInst, nullptr);
    SendMessageW(sldExposure_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 300));
    SendMessageW(sldExposure_, TBM_SETPOS,   TRUE,
                 (LPARAM)(i32)(service_.Settings().GetTonemapExposure() * 100.0f));
    {
        wchar_t buf[16];
        swprintf_s(buf, L"%.2f", service_.Settings().GetTonemapExposure());
        lblExposure_ = CreateWindowW(L"STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            264, rowY, 44, 22, hwndSettings_, nullptr, hInst, nullptr);
    }

    rowY += 38;
    CreateWindowW(L"STATIC", L"SND Volume:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    sldSndVolume_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        108, rowY, 150, 24, hwndSettings_,
        (HMENU)(INT_PTR)IDC_SND_VOLUME, hInst, nullptr);
    SendMessageW(sldSndVolume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(sldSndVolume_, TBM_SETPOS,   TRUE,
                 (LPARAM)(i32)(service_.Sound().GetVolume() * 100.0f));
    {
        wchar_t buf[16];
        swprintf_s(buf, L"%.2f", service_.Sound().GetVolume());
        lblSndVolume_ = CreateWindowW(L"STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            264, rowY, 44, 22, hwndSettings_, nullptr, hInst, nullptr);
    }

    rowY += 38;
    chkLoopNonLoop_ = CreateWindowW(L"BUTTON",
        L"Loop NonLooping animations",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12, rowY, 280, 22, hwndSettings_,
        (HMENU)(INT_PTR)IDC_LOOP_NONLOOP, hInst, nullptr);
    SendMessageW(chkLoopNonLoop_, BM_SETCHECK,
                 loopNonLoopingPolicy_ ? BST_CHECKED : BST_UNCHECKED, 0);

    rowY += 38;
    CreateWindowW(L"STATIC", L"Time of Day:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    sldTimeOfDay_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        108, rowY, 150, 24, hwndSettings_,
        (HMENU)(INT_PTR)IDC_TIME_OF_DAY, hInst, nullptr);
    {
        const f32 hpd = service_.GetDncService()
                              ? service_.GetDncService()->GetHoursPerDay()
                              : 24.0f;
        const i32 rangeHi = static_cast<i32>(hpd * 100.0f);
        SendMessageW(sldTimeOfDay_, TBM_SETRANGE, TRUE, MAKELPARAM(0, rangeHi));
        const f32 tod = service_.GetDncService()
                              ? service_.GetDncService()->GetTimeOfDay()
                              : 12.0f;
        SendMessageW(sldTimeOfDay_, TBM_SETPOS, TRUE,
                     (LPARAM)(i32)(tod * 100.0f));
        wchar_t buf[16];
        const i32 hh = static_cast<i32>(tod) % 24;
        const i32 mm = static_cast<i32>((tod - std::floor(tod)) * 60.0f) % 60;
        swprintf_s(buf, L"%02d:%02d", hh, mm);
        lblTimeOfDay_ = CreateWindowW(L"STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            264, rowY, 44, 22, hwndSettings_, nullptr, hInst, nullptr);
    }

    rowY += 38;
    chkAnimateTod_ = CreateWindowW(L"BUTTON",
        L"Animate TOD",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12, rowY, 280, 22, hwndSettings_,
        (HMENU)(INT_PTR)IDC_ANIMATE_TOD, hInst, nullptr);
    {
        const bool animating = service_.GetDncService()
                                  && service_.GetDncService()->GetTodScale() > 0.0f;
        SendMessageW(chkAnimateTod_, BM_SETCHECK,
                     animating ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    rowY += 38;
    CreateWindowW(L"STATIC", L"IBL:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    cmbIblMode_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        108, rowY, 200, 200, hwndSettings_,
        (HMENU)(INT_PTR)IDC_IBL_MODE, hInst, nullptr);
    SendMessageW(cmbIblMode_, CB_ADDSTRING, 0, (LPARAM)L"Portrait");
    SendMessageW(cmbIblMode_, CB_ADDSTRING, 0, (LPARAM)L"Day/Night");
    SendMessageW(cmbIblMode_, CB_ADDSTRING, 0, (LPARAM)L"Dungeon");
    SendMessageW(cmbIblMode_, CB_ADDSTRING, 0, (LPARAM)L"Sunset");
    SendMessageW(cmbIblMode_, CB_SETCURSEL,
                 static_cast<WPARAM>(service_.Settings().GetIblMode()), 0);

    rowY += 38;
    CreateWindowW(L"STATIC", L"Shadows:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    cmbShadows_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        108, rowY, 200, 200, hwndSettings_,
        (HMENU)(INT_PTR)IDC_SHADOWS, hInst, nullptr);
    SendMessageW(cmbShadows_, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(cmbShadows_, CB_ADDSTRING, 0, (LPARAM)L"1 cascade");
    SendMessageW(cmbShadows_, CB_ADDSTRING, 0, (LPARAM)L"2 cascades");
    SendMessageW(cmbShadows_, CB_ADDSTRING, 0, (LPARAM)L"3 cascades");
    {
        i32 sel = 0;
        if (auto* shadow = service_.GetShadowService()) {
            sel = shadow->IsEnabled() ? shadow->Params().cascadeCount : 0;
            if (sel < 0) sel = 0; else if (sel > 3) sel = 3;
        }
        SendMessageW(cmbShadows_, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }

    rowY += 38;
    CreateWindowW(L"STATIC", L"DNC Model:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    editDncPath_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        108, rowY + 1, 200, 20, hwndSettings_,
        (HMENU)(INT_PTR)IDC_DNC_PATH, hInst, nullptr);
    btnDncReset_ = CreateWindowW(L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        314, rowY, 54, 22, hwndSettings_,
        (HMENU)(INT_PTR)IDC_DNC_RESET, hInst, nullptr);
    if (auto* dnc = service_.GetDncService()) {
        const std::string& path = dnc->UnitMdlPath();

        const i32 wlen = ::MultiByteToWideChar(CP_UTF8, 0,
                                               path.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wpath(wlen, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1,
                                  wpath.data(), wlen);

            SetWindowTextW(editDncPath_, wpath.c_str());
        }
    }

    // --- Startup-only graphics settings (apply on next launch) ----------
    // Both controls just persist to the .ini; the validation layer and
    // the backend selection are locked in at gfx::CreateDevice time so
    // they don't take effect until the process is restarted.
    rowY += 38;
    CreateWindowW(L"STATIC", L"Backend:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    cmbDefaultBackend_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        108, rowY, 200, 200, hwndSettings_,
        (HMENU)(INT_PTR)IDC_DEFAULT_BACKEND, hInst, nullptr);
    SendMessageW(cmbDefaultBackend_, CB_ADDSTRING, 0, (LPARAM)L"D3D11");
    SendMessageW(cmbDefaultBackend_, CB_ADDSTRING, 0, (LPARAM)L"D3D12");
    SendMessageW(cmbDefaultBackend_, CB_ADDSTRING, 0, (LPARAM)L"Vulkan");
    {
        i32 sel = 1;  // D3D12 default
        switch (service_.Settings().DefaultBackend()) {
            case gfx::GfxApi::D3D11:  sel = 0; break;
            case gfx::GfxApi::D3D12:  sel = 1; break;
            case gfx::GfxApi::Vulkan: sel = 2; break;
        }
        SendMessageW(cmbDefaultBackend_, CB_SETCURSEL, sel, 0);
    }

    rowY += 38;
    CreateWindowW(L"STATIC", L"Device:",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        12, rowY, 90, 22, hwndSettings_, nullptr, hInst, nullptr);
    cmbPreferredDev_ = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        108, rowY, 260, 200, hwndSettings_,
        (HMENU)(INT_PTR)IDC_PREFERRED_DEVICE, hInst, nullptr);
    // First entry is the "auto" sentinel — empty preferred-device string
    // means "let the backend pick". Subsequent entries come from
    // gfx::EnumerateDevices(currentBackend); we use the saved
    // DefaultBackend as the enumeration target so the picker is
    // populated even when the running process used a different
    // backend (e.g. CLI override).
    SendMessageW(cmbPreferredDev_, CB_ADDSTRING, 0,
                 (LPARAM)L"(Auto — highest VRAM)");
    {
        const auto names = gfx::EnumerateDevices(
            service_.Settings().DefaultBackend());
        for (const auto& n : names) {
            const i32 wlen = ::MultiByteToWideChar(CP_UTF8, 0,
                                                    n.c_str(), -1, nullptr, 0);
            if (wlen <= 0) continue;
            std::wstring w(wlen, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1,
                                  w.data(), wlen);
            SendMessageW(cmbPreferredDev_, CB_ADDSTRING, 0, (LPARAM)w.c_str());
        }
    }
    {
        const std::string& cur = service_.Settings().PreferredDevice();
        i32 sel = 0;  // default to "Auto"
        if (!cur.empty()) {
            const i32 wlen = ::MultiByteToWideChar(CP_UTF8, 0,
                                                    cur.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring w(wlen, L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, cur.c_str(), -1,
                                      w.data(), wlen);
                const LRESULT found = SendMessageW(
                    cmbPreferredDev_, CB_FINDSTRINGEXACT,
                    (WPARAM)-1, (LPARAM)w.c_str());
                if (found != CB_ERR) sel = static_cast<i32>(found);
            }
        }
        SendMessageW(cmbPreferredDev_, CB_SETCURSEL, sel, 0);
    }

    rowY += 38;
    chkGraphicsDebug_ = CreateWindowW(L"BUTTON",
        L"Graphics Debug (validation, restart required)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12, rowY, 320, 22, hwndSettings_,
        (HMENU)(INT_PTR)IDC_GRAPHICS_DEBUG, hInst, nullptr);
    SendMessageW(chkGraphicsDebug_, BM_SETCHECK,
                 service_.Settings().GraphicsDebug() ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT RenderWindow::HandleSettingsMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:

        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == IDC_BGCOLOR) {
            COLORREF bc = service_.Settings().BackgroundColorRaw();
            HBRUSH brush = CreateSolidBrush(bc);
            FillRect(dis->hDC, &dis->rcItem, brush);
            DeleteObject(brush);
            DrawEdge(dis->hDC, &dis->rcItem, EDGE_SUNKEN, BF_RECT);
        }
        return TRUE;
    }

    case WM_HSCROLL: {

        const HWND src = (HWND)lParam;
        if (src == sldExposure_ && sldExposure_) {
            i32 pos = (i32)SendMessageW(sldExposure_, TBM_GETPOS, 0, 0);
            f32 exposure = (f32)pos / 100.0f;
            service_.Settings().SetTonemapExposure(exposure);
            if (lblExposure_) {
                wchar_t buf[16];
                swprintf_s(buf, L"%.2f", exposure);
                SetWindowTextW(lblExposure_, buf);
            }

            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (src == sldSndVolume_ && sldSndVolume_) {
            i32 pos = (i32)SendMessageW(sldSndVolume_, TBM_GETPOS, 0, 0);
            f32 volume = (f32)pos / 100.0f;
            service_.Sound().SetVolume(volume);
            if (lblSndVolume_) {
                wchar_t buf[16];
                swprintf_s(buf, L"%.2f", volume);
                SetWindowTextW(lblSndVolume_, buf);
            }
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (src == sldTimeOfDay_ && sldTimeOfDay_) {
            const i32 pos = (i32)SendMessageW(sldTimeOfDay_, TBM_GETPOS, 0, 0);
            const f32 tod = (f32)pos / 100.0f;
            if (auto* dnc = service_.GetDncService()) dnc->SetTimeOfDay(tod);
            if (lblTimeOfDay_) {
                wchar_t buf[16];
                const i32 hh = static_cast<i32>(tod) % 24;
                const i32 mm = static_cast<i32>((tod - std::floor(tod)) * 60.0f) % 60;
                swprintf_s(buf, L"%02d:%02d", hh, mm);
                SetWindowTextW(lblTimeOfDay_, buf);
            }
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        const i32 id = LOWORD(wParam);
        if (id == IDC_BGCOLOR) {
            CHOOSECOLORW cc = {};
            static COLORREF customColors[16] = {};
            cc.lStructSize  = sizeof(cc);
            cc.hwndOwner    = hwnd;
            cc.rgbResult    = service_.Settings().BackgroundColorRaw();
            cc.lpCustColors = customColors;
            cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                service_.Settings().SetBackgroundColor(
                    GetRValue(cc.rgbResult),
                    GetGValue(cc.rgbResult),
                    GetBValue(cc.rgbResult));
                if (btnBgColor_) InvalidateRect(btnBgColor_, nullptr, TRUE);
                SaveSettingsIni(service_, loopNonLoopingPolicy_);
            }
            return 0;
        }
        if (id == IDC_LOOP_NONLOOP) {
            const bool on = chkLoopNonLoop_
                && SendMessageW(chkLoopNonLoop_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            loopNonLoopingPolicy_ = on;
            // Apply to every actor currently in the scene. Tools that load
            // additional units later are responsible for applying the policy
            // to those actors themselves (test_main does this after Loader().SpawnUnit).
            for (auto& [h, mi] : service_.Scene().Actors().All()) {
                if (mi->IsChild()) continue;
                mi->ignoreNonLooping = on;
            }
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (id == IDC_ANIMATE_TOD) {
            const bool on = chkAnimateTod_
                && SendMessageW(chkAnimateTod_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (auto* dnc = service_.GetDncService()) dnc->SetTodScale(on ? 1.0f : 0.0f);
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (id == IDC_IBL_MODE && HIWORD(wParam) == CBN_SELCHANGE && cmbIblMode_) {
            const i32 sel = (i32)SendMessageW(cmbIblMode_, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel <= static_cast<i32>(IblMode::Sunset)) {
                service_.Settings().SetIblMode(static_cast<IblMode>(sel));
                SaveSettingsIni(service_, loopNonLoopingPolicy_);
            }
            return 0;
        }
        if (id == IDC_SHADOWS && HIWORD(wParam) == CBN_SELCHANGE && cmbShadows_) {
            const i32 sel = (i32)SendMessageW(cmbShadows_, CB_GETCURSEL, 0, 0);
            if (auto* shadow = service_.GetShadowService(); shadow && sel >= 0 && sel <= 3) {
                shadow::ShadowParams p = shadow->Params();
                p.enabled       = (sel > 0);
                p.cascadeCount  = (sel > 0) ? sel : 1;
                shadow->SetParams(p);
                SaveSettingsIni(service_, loopNonLoopingPolicy_);
            }
            return 0;
        }
        if (id == IDC_DEFAULT_BACKEND && HIWORD(wParam) == CBN_SELCHANGE && cmbDefaultBackend_) {
            const i32 sel = (i32)SendMessageW(cmbDefaultBackend_, CB_GETCURSEL, 0, 0);
            gfx::GfxApi b = gfx::GfxApi::D3D12;
            if      (sel == 0) b = gfx::GfxApi::D3D11;
            else if (sel == 1) b = gfx::GfxApi::D3D12;
            else if (sel == 2) b = gfx::GfxApi::Vulkan;
            service_.Settings().SetDefaultBackend(b);
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (id == IDC_GRAPHICS_DEBUG) {
            const bool on = chkGraphicsDebug_
                && SendMessageW(chkGraphicsDebug_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            service_.Settings().SetGraphicsDebug(on);
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (id == IDC_PREFERRED_DEVICE && HIWORD(wParam) == CBN_SELCHANGE && cmbPreferredDev_) {
            const i32 sel = (i32)SendMessageW(cmbPreferredDev_, CB_GETCURSEL, 0, 0);
            // Index 0 is the "(Auto — highest VRAM)" sentinel; anything
            // higher is a verbatim device name from EnumerateDevices.
            if (sel <= 0) {
                service_.Settings().SetPreferredDevice("");
            } else {
                const i32 wlen = (i32)SendMessageW(cmbPreferredDev_, CB_GETLBTEXTLEN, sel, 0);
                if (wlen > 0) {
                    std::wstring w(wlen, L'\0');
                    SendMessageW(cmbPreferredDev_, CB_GETLBTEXT,
                                 sel, (LPARAM)w.data());
                    const i32 u8len = ::WideCharToMultiByte(CP_UTF8, 0,
                                                            w.data(), wlen,
                                                            nullptr, 0, nullptr, nullptr);
                    if (u8len > 0) {
                        std::string utf8(u8len, '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen,
                                              utf8.data(), u8len, nullptr, nullptr);
                        service_.Settings().SetPreferredDevice(std::move(utf8));
                    }
                }
            }
            SaveSettingsIni(service_, loopNonLoopingPolicy_);
            return 0;
        }
        if (id == IDC_DNC_RESET && btnDncReset_) {

            if (auto* dnc = service_.GetDncService()) {
                dnc->SetUnitMdl(dnc::DncService::kDefaultUnitMdl);
                if (editDncPath_) {
                    SetWindowTextA(editDncPath_, dnc::DncService::kDefaultUnitMdl);
                }
                SaveSettingsIni(service_, loopNonLoopingPolicy_);
            }
            return 0;
        }
        if (id == IDC_DNC_PATH && HIWORD(wParam) == EN_KILLFOCUS && editDncPath_) {

            wchar_t buf[512] = {};
            const i32 n = GetWindowTextW(editDncPath_, buf, 512);
            if (n >= 0) {
                const i32 u8len = ::WideCharToMultiByte(CP_UTF8, 0, buf, n,
                                                        nullptr, 0, nullptr, nullptr);
                std::string utf8(u8len, '\0');
                if (u8len > 0) {
                    ::WideCharToMultiByte(CP_UTF8, 0, buf, n,
                                          utf8.data(), u8len, nullptr, nullptr);
                }
                if (auto* dnc = service_.GetDncService()) {
                    dnc->SetUnitMdl(utf8);
                    SaveSettingsIni(service_, loopNonLoopingPolicy_);
                }
            }
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}
