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
// [T11] 게임 모드 인디케이터 (우측). 보라색은 SAFE/CAUTION/RISK 어느 것과도
// 시각적으로 안 겹쳐 한눈에 "지금 게임 모드"라고 인지된다.
static constexpr COLORREF kGameBadgeBg  = RGB(120,  40, 200);  // 보라
static constexpr COLORREF kGameTextColor = RGB(255, 255, 255);

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
        paintBadge(hwnd, state, self->m_riskStartMs.load());
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_SC_STATE: setState() 에서 전달하는 상태 변경 메시지
    //   wParam = SecurityState as int
    //   lParam = gameMode as bool (0/1)
    // ------------------------------------------------------------------
    case WM_APP + 0: {
        const int newSt = static_cast<int>(wParam);
        const int oldSt = self->m_state.exchange(newSt);
        self->m_gameMode.store(lParam != 0);
        // SAFE/CAUTION → RISK 전환 시에만 RISK 시작 시각 기록(깜빡임 타이밍 기준).
        // RISK 지속 중에는 갱신하지 않아 10초 후 솔리드로 넘어간다. RISK가 풀리면
        // 다음 RISK 진입 때 다시 기록되어 깜빡임이 새로 시작된다.
        const int risk = static_cast<int>(SecurityState::RISK);
        if (newSt == risk && oldSt != risk)
            self->m_riskStartMs.store(GetTickCount64());
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
// paintBadge — 경광등 렌더링
//   평소(SAFE/CAUTION): 초록 동그라미.
//   RISK: 진입 후 kRiskBlinkMs(10초)간 빨강 깜빡임(on/off), 이후 솔리드 빨강.
//   RISK가 풀리면 다시 초록(깜빡임 도중에 풀려도 즉시 초록).
//   setState가 매 프레임 호출 → 매 프레임 repaint로 시간 기반 깜빡임이 갱신됨.
// =============================================================================
void OverlayWindow::paintBadge(HWND hwnd, SecurityState state,
                               uint64_t riskStartMs)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    // 전체 배경을 colorkey(투명)로 채움 → 원 바깥은 투명.
    HBRUSH bgBrush = CreateSolidBrush(kBgColor);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // 색/표시 결정: 기본 초록, RISK면 빨강(깜빡 또는 솔리드).
    COLORREF color = kBarSafe; // 초록
    bool draw = true;
    if (state == SecurityState::RISK) {
        color = kBarRisk; // 빨강
        const uint64_t elapsed = GetTickCount64() - riskStartMs;
        if (elapsed < kRiskBlinkMs) {
            // 깜빡임 구간: kRiskBlinkHalf 주기로 on/off. off면 안 그림(투명).
            draw = ((elapsed / kRiskBlinkHalf) % 2) == 0;
        }
        // elapsed >= kRiskBlinkMs → 솔리드 빨강(draw=true 유지).
    }

    if (draw) {
        // 테두리 = 채움색의 진한 버전 (각 채널 ~55%).
        const COLORREF dark = RGB(GetRValue(color) * 55 / 100,
                                  GetGValue(color) * 55 / 100,
                                  GetBValue(color) * 55 / 100);
        // 펜 두께를 감안해 ellipse 사각형을 안쪽으로 들여, 테두리가 창 밖으로
        // 잘리지 않게 한다.
        const int pad = 2 + kBeaconBorderPx / 2;
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, kBeaconBorderPx, dark);
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        Ellipse(hdc, rc.left + pad, rc.top + pad, rc.right - pad,
                rc.bottom - pad);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(brush);
        DeleteObject(pen);
    }

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

    // --- 배치: 기본 모니터 우상단 (RISK 경광등) ---
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int posX    = screenW - kWidth - 24;
    int posY    = 24;

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
void OverlayWindow::setState(SecurityState state, bool gameMode)
{
    if (!m_hwnd)
        return;

    // PostMessage는 thread-safe — 메시지 큐에 비동기로 전달.
    // wParam = SecurityState, lParam = gameMode (0/1).
    PostMessage(m_hwnd, WM_SC_STATE, static_cast<WPARAM>(state),
                static_cast<LPARAM>(gameMode ? 1 : 0));
}

#endif // _WIN32
