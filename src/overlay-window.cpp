// =============================================================================
// overlay-window.cpp — OverlayWindow 구현
//
// 핵심 메커니즘:
//   - Win32 레이어드 팝업 창 (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW)
//   - SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) → OBS·캡처 소프트웨어 불가시
//   - SetLayeredWindowAttributes(colorkey=RGB(1,1,1), LWA_COLORKEY)
//     배경 색(RGB 1,1,1)을 투명 처리 → 배지 모양만 화면에 떠있음
//   - 보안 상태(SAFE/PARTIAL/RISK) 변경 시 WM_APP+0 메시지로 스레드에 전달
//   - GDI로 왼쪽 색상 바 + 상태 텍스트 배지 렌더링
// =============================================================================

#ifdef _WIN32

#include "overlay-window.h"
#include "plugin-support.h"  // blog / LOG_INFO / LOG_WARNING

#include <obs-module.h>      // blog
#include <winternl.h>        // RTL_OSVERSIONINFOW (RtlGetVersion용)

// WDA_EXCLUDEFROMCAPTURE: Windows 10 2004+ (build 19041+) 에서 정의.
// 구버전 SDK로 빌드할 때를 대비해 직접 정의해둠.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// =============================================================================
// isExcludeFromCaptureSupported — WDA_EXCLUDEFROMCAPTURE 지원 여부 반환
//
// RtlGetVersion으로 실제 빌드 번호를 확인한다.
// GetVersionEx는 Win 8.1+에서 매니페스트 없이 호출하면 6.2(Win8)를 반환하지만,
// RtlGetVersion은 매니페스트와 무관하게 실제 버전을 반환한다.
// WDA_EXCLUDEFROMCAPTURE는 Windows 10 2004 (build 19041) 이상에서만 지원된다.
// =============================================================================
static bool isExcludeFromCaptureSupported()
{
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;

    auto fnRtlGetVersion = reinterpret_cast<RtlGetVersion_t>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fnRtlGetVersion)
        return false;

    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fnRtlGetVersion(&vi) != 0 /* STATUS_SUCCESS = 0 */)
        return false;

    return vi.dwBuildNumber >= 19041;
}

// ---- 색상 상수 ----
static constexpr COLORREF kBgColor      = RGB(1, 1, 1);   // 투명 처리용 배경 (colorkey)
static constexpr COLORREF kBarSafe      = RGB(0,  200,  80);  // SAFE  : 초록
static constexpr COLORREF kBarPartial   = RGB(240, 200,   0);  // CAUTION: 노랑
static constexpr COLORREF kBarRisk      = RGB(220,  40,  40);  // RISK  : 빨강
static constexpr COLORREF kBadgeBg      = RGB(20,  20,  20);   // 배지 배경 (어두운 회색)
static constexpr COLORREF kTextColor    = RGB(255, 255, 255);  // 흰 텍스트

// ---- 레이아웃 상수 ----
static constexpr int kBarWidth = 10;   // 왼쪽 색상 바 너비(px)
static constexpr int kMargin   =  8;   // 텍스트 좌측 여백(px)

// =============================================================================
// WndProc — Win32 메시지 처리
// =============================================================================
LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    // 생성 시 CREATESTRUCT::lpCreateParams 에 this 포인터를 저장해둠
    OverlayWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<OverlayWindow*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {

    // ------------------------------------------------------------------
    // WM_PAINT: 배지 그리기
    // ------------------------------------------------------------------
    case WM_PAINT: {
        SecurityState state = static_cast<SecurityState>(self->m_state.load());
        paintBadge(hwnd, state);
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_SC_STATE: setState() 에서 전달하는 상태 변경 메시지
    // ------------------------------------------------------------------
    case WM_APP + 0: {
        self->m_state.store(static_cast<int>(wParam));
        InvalidateRect(hwnd, nullptr, FALSE);  // 다음 메시지 루프에서 WM_PAINT 발생
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// paintBadge — GDI 배지 렌더링
//   레이아웃: [■ colorBar(10px) | 배지 배경 | 텍스트 "SECURECAST  SAFE"]
// =============================================================================
void OverlayWindow::paintBadge(HWND hwnd, SecurityState state)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    // --- 1. 전체 배경을 colorkey 색으로 채움 (투명 영역) ---
    HBRUSH bgBrush = CreateSolidBrush(kBgColor);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // --- 2. 배지 배경 (어두운 회색) ---
    RECT badgeRect = {0, 0, w, h};
    HBRUSH badgeBrush = CreateSolidBrush(kBadgeBg);
    FillRect(hdc, &badgeRect, badgeBrush);
    DeleteObject(badgeBrush);

    // --- 3. 왼쪽 색상 바 ---
    COLORREF barColor;
    const wchar_t* stateText;
    switch (state) {
    case SecurityState::PARTIAL:
        barColor  = kBarPartial;
        stateText = L"CAUTION";
        break;
    case SecurityState::RISK:
        barColor  = kBarRisk;
        stateText = L"  RISK ";
        break;
    default: // SAFE
        barColor  = kBarSafe;
        stateText = L"  SAFE ";
        break;
    }

    RECT barRect = {0, 0, kBarWidth, h};
    HBRUSH barBrush = CreateSolidBrush(barColor);
    FillRect(hdc, &barRect, barBrush);
    DeleteObject(barBrush);

    // --- 4. 텍스트 "SecureCast  <STATE>" ---
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kTextColor);

    // 첫 줄: "SecureCast" (작은 회색)
    SetTextColor(hdc, RGB(160, 160, 160));
    HFONT smallFont = CreateFont(
        12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, smallFont));

    RECT labelRect = {kBarWidth + kMargin, 4, w - 4, h / 2};
    DrawText(hdc, L"SecureCast", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 두 번째 줄: 상태 텍스트 (굵은 흰색)
    HFONT boldFont = CreateFont(
        16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SelectObject(hdc, boldFont);
    SetTextColor(hdc, kTextColor);

    RECT stateRect = {kBarWidth + kMargin, h / 2, w - 4, h - 4};
    DrawText(hdc, stateText, -1, &stateRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 폰트 정리
    SelectObject(hdc, oldFont);
    DeleteObject(smallFont);
    DeleteObject(boldFont);

    EndPaint(hwnd, &ps);
}

// =============================================================================
// messageLoop — 별도 스레드에서 실행되는 Win32 메시지 루프
// =============================================================================
void OverlayWindow::messageLoop()
{
    // --- 윈도우 클래스 등록 ---
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;  // WM_PAINT에서 직접 그림

    RegisterClassEx(&wc);  // 이미 등록된 경우 무시 (ERROR_CLASS_ALREADY_EXISTS)

    // --- 배치: 기본 모니터 우하단 ---
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX    = screenW - kWidth  - 16;
    int posY    = screenH - kHeight - 48;  // 작업표시줄 위

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED   |    // 투명 처리 지원
        WS_EX_TOPMOST   |    // 항상 최상위
        WS_EX_NOACTIVATE|    // 포커스 훔치지 않음
        WS_EX_TOOLWINDOW|    // 작업표시줄에 나타나지 않음
        WS_EX_TRANSPARENT,   // 클릭 통과 (마우스 이벤트를 아래 창으로 전달)
        kClassName,
        L"SecureCast",
        WS_POPUP | WS_VISIBLE,
        posX, posY, kWidth, kHeight,
        nullptr, nullptr, hInst,
        this               // WM_NCCREATE에서 GWLP_USERDATA에 저장
    );

    if (!hwnd) {
        blog(LOG_WARNING,
             "[SecureCast][D] OverlayWindow: CreateWindowEx failed (err=%lu)",
             GetLastError());
        m_ready.store(true);  // create() 블로킹 해제
        return;
    }

    // --- SetWindowDisplayAffinity: 캡처 소프트웨어에서 보이지 않게 ---
    // 지원 여부는 create()에서 isExcludeFromCaptureSupported()로 판단해 m_useExcludeFromCapture에 저장됨.
    {
        BOOL affOk = FALSE;

        if (m_useExcludeFromCapture) {
            affOk = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
            if (affOk) {
                blog(LOG_INFO, "[SecureCast][D] WDA_EXCLUDEFROMCAPTURE 적용.");
            } else {
                blog(LOG_WARNING,
                     "[SecureCast][D] WDA_EXCLUDEFROMCAPTURE 실패 (err=%lu) → WDA_MONITOR 시도.",
                     GetLastError());
                affOk = SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
                if (affOk)
                    blog(LOG_INFO, "[SecureCast][D] WDA_MONITOR fallback 적용.");
            }
        } else {
            // Windows 10 1909 이하: WDA_MONITOR 직행
            affOk = SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
            if (affOk)
                blog(LOG_INFO, "[SecureCast][D] WDA_MONITOR 적용 (build < 19041).");
        }

        if (!affOk) {
            blog(LOG_WARNING,
                 "[SecureCast][D] 캡처 제외 불가 (err=%lu). "
                 "오버레이는 동작하지만 방송 화면에 노출될 수 있습니다.",
                 GetLastError());
        }
    }

    // --- SetLayeredWindowAttributes: colorkey(RGB 1,1,1)를 투명 처리 ---
    SetLayeredWindowAttributes(hwnd, kBgColor, 0, LWA_COLORKEY);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    m_ready.store(true);  // create() 블로킹 해제

    // --- 메시지 루프 ---
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 루프 종료 후 클래스 등록 해제
    UnregisterClass(kClassName, hInst);
    m_hwnd = NULL;
}

// =============================================================================
// create — 오버레이 윈도우 시작
// =============================================================================
bool OverlayWindow::create()
{
    if (m_running.exchange(true))
        return true;  // 이미 실행 중

    m_useExcludeFromCapture = isExcludeFromCaptureSupported();
    blog(LOG_INFO, "[SecureCast][D] WDA_EXCLUDEFROMCAPTURE 지원: %s",
         m_useExcludeFromCapture ? "YES (build >= 19041)" : "NO (build < 19041)");

    m_ready.store(false);

    m_thread = std::thread(&OverlayWindow::messageLoop, this);

    // messageLoop가 윈도우를 만들 때까지 대기 (spin-wait, ~수 ms)
    while (!m_ready.load())
        Sleep(1);

    return m_hwnd != NULL;
}

// =============================================================================
// destroy — 오버레이 윈도우 종료
// =============================================================================
void OverlayWindow::destroy()
{
    if (!m_running.exchange(false))
        return;  // 이미 종료됨

    if (m_hwnd) {
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    }

    if (m_thread.joinable())
        m_thread.join();
}

// =============================================================================
// setState — 임의 스레드에서 상태 갱신 (PostMessage로 UI 스레드에 전달)
// =============================================================================
void OverlayWindow::setState(SecurityState state)
{
    if (!m_hwnd)
        return;

    // PostMessage는 thread-safe — 메시지 큐에 비동기로 전달
    PostMessage(m_hwnd, WM_SC_STATE,
                static_cast<WPARAM>(state), 0);
}

#endif // _WIN32
