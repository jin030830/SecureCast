// =============================================================================
// selection-overlay.cpp — 수동 드래그 블러 선택 오버레이 구현
// =============================================================================
#ifdef _WIN32

#define NOMINMAX
#include "selection-overlay.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include <algorithm>

std::atomic<SelectionOverlay*> SelectionOverlay::s_active{nullptr};

// =============================================================================
// WndProc
// =============================================================================
LRESULT CALLBACK SelectionOverlay::WndProc(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    SelectionOverlay* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<SelectionOverlay*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<SelectionOverlay*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {

    case WM_PAINT:
        paintSelection(hwnd, self->m_start, self->m_cur, self->m_dragging);
        return 0;

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        self->m_dragging = true;
        self->m_start = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        self->m_cur   = self->m_start;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (self->m_dragging) {
            self->m_cur = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_LBUTTONUP: {
        if (!self->m_dragging)
            return 0;

        ReleaseCapture();
        self->m_dragging = false;

        POINT end = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int x  = std::min(self->m_start.x, end.x);
        int y  = std::min(self->m_start.y, end.y);
        int bw = std::abs(end.x - self->m_start.x);
        int bh = std::abs(end.y - self->m_start.y);

        if (bw > 8 && bh > 8) {
            // 클라이언트 좌표 → 모니터 상대 좌표로 변환
            BlurRect rect = {
                x + self->m_monLeft,
                y + self->m_monTop,
                bw, bh, 0
            };
            blog(LOG_INFO, "[SecureCast][D] Selection done: (%d,%d %dx%d)",
                 rect.x, rect.y, rect.width, rect.height);
            if (self->m_callback)
                self->m_callback(rect);
        }

        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            self->m_dragging = false;
            blog(LOG_INFO, "[SecureCast][D] Selection cancelled (ESC via keydown).");
            KillTimer(hwnd, 1);
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_TIMER:
        // 포커스가 없어도 ESC 감지 (50ms 폴링)
        if (wParam == 1 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            self->m_dragging = false;
            blog(LOG_INFO, "[SecureCast][D] Selection cancelled (ESC via poll).");
            KillTimer(hwnd, 1);
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        s_active.store(nullptr, std::memory_order_release);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// paintSelection — 반투명 다크 오버레이 + 선택 영역 그리기
// LWA_ALPHA 방식: 전체 창이 반투명(어두운 오버레이)이고
//                 선택 영역은 밝은 색으로 구분됨
// =============================================================================
void SelectionOverlay::paintSelection(HWND hwnd, POINT start, POINT cur, bool dragging)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    // 1. 전체 배경: 다크 오버레이 (반투명은 LWA_ALPHA가 담당)
    HBRUSH bgBrush = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // 2. 안내 문구 (항상 표시)
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HFONT font = CreateFont(
        22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    RECT textRect = {0, 16, rc.right, 52};
    DrawText(hdc, L"Drag to select blur area    ESC = cancel",
             -1, &textRect, DT_CENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    if (dragging) {
        int x  = std::min(start.x, cur.x);
        int y  = std::min(start.y, cur.y);
        int x2 = std::max(start.x, cur.x);
        int y2 = std::max(start.y, cur.y);

        // 3. 선택 영역 내부: 더 밝게 (배경보다 밝은 색)
        HBRUSH selBrush = CreateSolidBrush(RGB(0, 100, 200));
        RECT selRect = {x, y, x2, y2};
        FillRect(hdc, &selRect, selBrush);
        DeleteObject(selBrush);

        // 4. 선택 영역 흰 테두리
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH nullBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, nullBrush));
        Rectangle(hdc, x, y, x2, y2);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);

        // 5. 크기 표시 (선택 박스 좌상단)
        wchar_t sizeText[64];
        swprintf_s(sizeText, L" %d x %d ", x2 - x, y2 - y);
        SetTextColor(hdc, RGB(255, 255, 100));
        HFONT smallFont = CreateFont(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldSmall = static_cast<HFONT>(SelectObject(hdc, smallFont));
        RECT sizeRect = {x + 4, y + 4, x2, y + 24};
        DrawText(hdc, sizeText, -1, &sizeRect, DT_LEFT | DT_SINGLELINE);
        SelectObject(hdc, oldSmall);
        DeleteObject(smallFont);
    }

    EndPaint(hwnd, &ps);
}

// =============================================================================
// messageLoop
// =============================================================================
void SelectionOverlay::messageLoop()
{
    HINSTANCE hInst = GetModuleHandle(nullptr);

    // Primary monitor 정보 (좌표 변환용)
    HMONITOR hmon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfo(hmon, &mi);

    m_monLeft = mi.rcMonitor.left;
    m_monTop  = mi.rcMonitor.top;
    int monW  = mi.rcMonitor.right  - mi.rcMonitor.left;
    int monH  = mi.rcMonitor.bottom - mi.rcMonitor.top;

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = CreateSolidBrush(RGB(10, 10, 10));
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName,
        L"SecureCast Selection",
        WS_POPUP | WS_VISIBLE,
        m_monLeft, m_monTop, monW, monH,
        nullptr, nullptr, hInst, this);

    if (!hwnd) {
        blog(LOG_WARNING, "[SecureCast][D] SelectionOverlay: CreateWindowEx failed (%lu)",
             GetLastError());
        m_ready.store(true);
        return;
    }

    // LWA_ALPHA: 전체 창을 60% 불투명(어두운 오버레이)으로 설정
    // colorkey 방식 대신 alpha 방식 사용 → GDI 렌더링 문제 없음
    SetLayeredWindowAttributes(hwnd, 0, 153, LWA_ALPHA); // 153/255 ≈ 60%

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // ESC 폴링 타이머 (포커스 없어도 동작)
    SetTimer(hwnd, 1, 50, nullptr);

    m_ready.store(true);
    blog(LOG_INFO, "[SecureCast][D] Selection overlay active. Drag to select, ESC to cancel.");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClass(kClassName, hInst);
    m_hwnd = NULL;
    m_running.store(false, std::memory_order_release);
}

// =============================================================================
// start / cancel
// =============================================================================
void SelectionOverlay::start(DoneCallback cb)
{
    if (m_running.exchange(true, std::memory_order_acq_rel))
        return;

    // 이전 스레드가 종료됐지만 아직 join되지 않은 경우 std::terminate() 방지
    if (m_thread.joinable())
        m_thread.join();

    m_callback = std::move(cb);
    m_dragging = false;
    m_ready.store(false);
    s_active.store(this, std::memory_order_release);

    m_thread = std::thread(&SelectionOverlay::messageLoop, this);
    // OBS 핫키 콜백 스레드 블로킹 방지 — 오버레이는 비동기로 생성됨
}

void SelectionOverlay::cancel()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    if (m_hwnd)
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    // join은 소멸자에서 처리 — 여기서 blocking하면 OBS 핫키 콜백 스레드를 막아 크래시 발생
}

#endif // _WIN32
