// =============================================================================
// overlay-window.cpp — OverlayWindow 구현 (경광등 HUD)
//
// 핵심 메커니즘:
//   - Win32 레이어드 팝업 창 (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
//     WS_EX_NOACTIVATE)
//   - SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) → OBS·캡처 소프트웨어 불가시
//   - UpdateLayeredWindow(퍼-픽셀 알파)로 광택 + 글로우(빛 번짐)가 있는 LED 느낌의
//     경광등을 그린다. 동그라미 바깥은 완전 투명, 글로우 영역은 알파 그라데이션.
//   - 보안 상태(SAFE/PARTIAL/RISK) 변경 시 WM_SC_STATE 메시지로 스레드에 전달.
//   - 사용자가 동그라미를 드래그하면 이동(WM_NCHITTEST→HTCAPTION),
//     우클릭하면 즉시 숨김. 둘 다 콜백으로 필터에 통지해 설정에 영속화한다.
// =============================================================================

#ifdef _WIN32

#include "overlay-window.h"
#include "plugin-support.h"  // blog / LOG_INFO / LOG_WARNING

#include <obs-module.h>      // blog
#include <winternl.h>        // RTL_OSVERSIONINFOW (RtlGetVersion용)
#include <windowsx.h>        // GET_X_LPARAM / GET_Y_LPARAM
#include <shobjidl.h>        // ITaskbarList3 (작업표시줄 아이콘 overlay)

#include <cmath>


// =============================================================================
// isExcludeFromCaptureSupported — WDA_EXCLUDEFROMCAPTURE 지원 여부 반환
//
// RtlGetVersion으로 실제 빌드 번호를 확인한다.
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

// ---- 경광등 형상 상수 (px, 56x56 창 기준) ----
// 멀리서도 눈에 들어오되 너무 크지 않게 + 글로우(빛 번짐)를 넓게 잡는다.
static constexpr float kCoreR    = 10.0f;  // 코어(채워진 동그라미) 반지름
static constexpr float kRingOut  = 12.0f;  // 코어 테두리(rim) 바깥 반지름
static constexpr float kGlowOut  = 27.0f;  // 글로우(빛 번짐) 바깥 반지름
static constexpr float kGlowMaxA = 225.0f; // 글로우 최대 알파(0~255) — 밝게
static constexpr float kHitR     = 14.0f;  // 드래그/우클릭 히트 반지름(코어+여유)
static constexpr float kTwoPi    = 6.2831853f;

// =============================================================================
// setInitialState / setCallbacks — create() 전에 호출
// =============================================================================
void OverlayWindow::setInitialState(int x, int y, bool visible)
{
    m_posX.store(x);
    m_posY.store(y);
    m_visible.store(visible);
}

void OverlayWindow::setCallbacks(MovedCallback onMoved, HiddenCallback onHidden)
{
    m_onMoved  = std::move(onMoved);
    m_onHidden = std::move(onHidden);
}

void OverlayWindow::setTaskbarTarget(void *obsMainHwnd)
{
    m_obsHwnd = static_cast<HWND>(obsMainHwnd);
}

// =============================================================================
// createRiskIcon — 작업표시줄 overlay용 빨간 원형 아이콘(HICON) 생성
//   32bpp straight-alpha DIB에 빨간 원을 그려 CreateIconIndirect로 아이콘화.
// =============================================================================
static HICON createRiskIcon()
{
    const int S = 32;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = S;
    bmi.bmiHeader.biHeight      = -S;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color)
        return NULL;

    uint8_t *p = static_cast<uint8_t *>(bits);
    const float cx = S * 0.5f - 0.5f;
    const float cy = S * 0.5f - 0.5f;
    const float R  = S * 0.46f;

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const float dx = x - cx, dy = y - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);

            float r = 240, g = 36, b = 36, a = 0;
            if (d <= R) {
                a = 255.0f;
                const float sh = 1.0f - (d / R) * 0.20f;  // 중심이 살짝 밝게
                r *= sh; g *= sh; b *= sh;
                if (d > R - 2.0f) { r = 150; g = 0; b = 0; }  // 진한 테두리
            } else if (d <= R + 1.0f) {
                a = 255.0f * (R + 1.0f - d);                  // 1px AA
            }

            const int idx = (y * S + x) * 4;  // straight alpha BGRA
            p[idx + 0] = static_cast<uint8_t>(b);
            p[idx + 1] = static_cast<uint8_t>(g);
            p[idx + 2] = static_cast<uint8_t>(r);
            p[idx + 3] = static_cast<uint8_t>(a);
        }
    }

    // AND 마스크는 전부 0(=컬러/알파 사용). 한 줄 = 32bit = 4byte로 워드 정렬됨.
    uint8_t maskBits[S * 4] = {0};
    HBITMAP mask = CreateBitmap(S, S, 1, 1, maskBits);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// =============================================================================
// initTaskbar / shutdownTaskbar / updateTaskbarOverlay — 메시지 루프 스레드 전용
// =============================================================================
void OverlayWindow::initTaskbar()
{
    if (!m_obsHwnd)
        return;  // 대상 창이 없으면 작업표시줄 점등 비활성

    // 우리 전용 스레드라 보통 S_OK. S_FALSE(동일 모드 재진입)도 ref가 증가하므로
    // CoUninitialize 필요 → SUCCEEDED면 m_comInit=true. RPC_E_CHANGED_MODE는
    // ref 증가 없음(SUCCEEDED=false) → CoUninitialize 안 함.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInit = SUCCEEDED(hr);

    ITaskbarList3 *tb = nullptr;
    hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITaskbarList3, reinterpret_cast<void **>(&tb));
    if (SUCCEEDED(hr) && tb) {
        if (SUCCEEDED(tb->HrInit())) {
            m_taskbar = tb;
            m_riskIcon = createRiskIcon();
        } else {
            tb->Release();
        }
    }
    if (!m_taskbar)
        blog(LOG_WARNING,
             "[SecureCast][D] 작업표시줄 ITaskbarList3 초기화 실패 — 아이콘 점등 비활성.");
}

void OverlayWindow::shutdownTaskbar()
{
    // 종료 시 깜빡임 정지 (RISK 도중 필터 제거 대비)
    if (m_obsHwnd) {
        FLASHWINFO fi{};
        fi.cbSize  = sizeof(fi);
        fi.hwnd    = m_obsHwnd;
        fi.dwFlags = FLASHW_STOP;
        FlashWindowEx(&fi);
    }
    if (m_taskbar) {
        ITaskbarList3 *tb = static_cast<ITaskbarList3 *>(m_taskbar);
        if (m_obsHwnd)
            tb->SetOverlayIcon(m_obsHwnd, NULL, L"");  // 종료 시 배지 제거
        tb->Release();
        m_taskbar = nullptr;
    }
    if (m_riskIcon) {
        DestroyIcon(m_riskIcon);
        m_riskIcon = NULL;
    }
    if (m_comInit) {
        CoUninitialize();
        m_comInit = false;
    }
    m_lastTaskbarRisk = -1;
    m_flashActive = false;
    m_fgFlashDone = false;
}

void OverlayWindow::updateTaskbarOverlay(SecurityState state)
{
    if (!m_obsHwnd)
        return;

    const int risk = (state == SecurityState::RISK) ? 1 : 0;
    if (risk == m_lastTaskbarRisk)
        return;  // 변경 없을 때 중복 호출 방지(상태는 매 프레임 들어옴)
    m_lastTaskbarRisk = risk;

    // 빨간 원형 배지 (ITaskbarList3 초기화에 성공한 경우). 깜빡임은 펄스 타이머의
    // updateTaskbarFlash()가 별도로 관리한다.
    if (m_taskbar) {
        ITaskbarList3 *tb = static_cast<ITaskbarList3 *>(m_taskbar);
        if (risk)
            tb->SetOverlayIcon(m_obsHwnd, m_riskIcon, L"보안 위험");
        else
            tb->SetOverlayIcon(m_obsHwnd, NULL, L"");
    }
}

// =============================================================================
// updateTaskbarFlash — 작업표시줄 버튼 깜빡임 상태머신 (펄스 타이머가 매 틱 호출)
//   RISK + 백그라운드 : FLASHW_TIMER로 위험 동안 계속 깜빡.
//   RISK + 활성창     : 깜빡임을 시작한 뒤 kFgFlashMs(10초)만 깜빡이고 정지.
//                       (다시 백그라운드로 갔다 오면 또 10초 깜빡)
//   RISK 아님        : 깜빡임 정지.
// =============================================================================
void OverlayWindow::updateTaskbarFlash()
{
    if (!m_obsHwnd)
        return;

    const bool risk =
        static_cast<SecurityState>(m_state.load()) == SecurityState::RISK;

    auto startFlash = [&]() {
        FLASHWINFO fi{};
        fi.cbSize    = sizeof(fi);
        fi.hwnd      = m_obsHwnd;
        fi.dwFlags   = FLASHW_ALL | FLASHW_TIMER;   // 캡션+작업표시줄, STOP 전까지 계속
        fi.uCount    = 0;
        fi.dwTimeout = 0;                            // 시스템 기본 점멸 주기
        FlashWindowEx(&fi);
        m_flashActive = true;
    };
    auto stopFlash = [&]() {
        FLASHWINFO fi{};
        fi.cbSize  = sizeof(fi);
        fi.hwnd    = m_obsHwnd;
        fi.dwFlags = FLASHW_STOP;
        FlashWindowEx(&fi);
        m_flashActive = false;
    };

    if (!risk) {
        if (m_flashActive)
            stopFlash();
        m_fgFlashDone = false;  // 다음 RISK를 위해 리셋
        return;
    }

    const bool foreground = (GetForegroundWindow() == m_obsHwnd);

    if (foreground) {
        // 활성창: 10초만 깜빡인다.
        if (m_fgFlashDone) {
            if (m_flashActive)
                stopFlash();         // 10초 끝 → 정지 유지
        } else {
            if (!m_flashActive) {
                startFlash();
                m_flashFgStart = GetTickCount64();
            } else if (GetTickCount64() - m_flashFgStart >= kFgFlashMs) {
                stopFlash();
                m_fgFlashDone = true;  // 활성창 깜빡임 완료(다시 bg 갔다 와야 재개)
            }
        }
    } else {
        // 백그라운드: 위험 동안 무조건 계속 깜빡. 다음 활성창 방문 시 10초 재개되도록
        // 완료 래치를 풀고 타이머 기준점을 갱신해 둔다.
        m_fgFlashDone  = false;
        m_flashFgStart = GetTickCount64();
        if (!m_flashActive)
            startFlash();
    }
}

// =============================================================================
// ensureSurface / releaseSurface — 알파 DIB 표면 준비/해제 (오버레이 스레드 전용)
// =============================================================================
bool OverlayWindow::ensureSurface()
{
    if (m_memDC && m_bits)
        return true;

    HDC screen = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!m_memDC)
        return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = kWidth;
    bmi.bmiHeader.biHeight      = -kHeight;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_dib = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (!m_dib || !m_bits) {
        DeleteDC(m_memDC);
        m_memDC = NULL;
        m_dib   = NULL;
        m_bits  = nullptr;
        return false;
    }

    m_oldBmp = static_cast<HBITMAP>(SelectObject(m_memDC, m_dib));
    return true;
}

void OverlayWindow::releaseSurface()
{
    if (m_memDC) {
        if (m_oldBmp)
            SelectObject(m_memDC, m_oldBmp);
        DeleteDC(m_memDC);
        m_memDC = NULL;
    }
    if (m_dib) {
        DeleteObject(m_dib);
        m_dib = NULL;
    }
    m_oldBmp = NULL;
    m_bits   = nullptr;
}

// =============================================================================
// renderBeacon — 경광등 렌더링 (퍼-픽셀 알파)
//   평소(SAFE/CAUTION): 진한 초록 동그라미 + 광택 + 초록 글로우.
//   RISK: 진입 후 kRiskBlinkMs(10초)간 빨강 맥동(밝기/글로우가 숨쉬듯), 이후 솔리드.
//   동그라미 바깥은 완전 투명, 글로우는 알파 그라데이션으로 "불 켜진" 느낌을 준다.
// =============================================================================
void OverlayWindow::renderBeacon()
{
    if (!m_hwnd || !m_bits)
        return;

    const int   W  = kWidth, H = kHeight;
    const float cx = W * 0.5f - 0.5f;
    const float cy = H * 0.5f - 0.5f;

    const SecurityState state = static_cast<SecurityState>(m_state.load());

    // 색 팔레트: body(본체) / glow(빛 번짐) / edge(테두리). 멀리서도 잘 보이도록
    // 형광에 가깝게 밝고 쨍한 색으로 잡는다.
    //   🔴 RISK    : 빨강 — 하나라도 놓칠 위험
    //   🟡 PARTIAL : 노랑 — 민감정보 감지·가리는 중
    //   🟢 SAFE    : 초록 — 안전
    float bodyR, bodyG, bodyB, glowR, glowG, glowB, edgeR, edgeG, edgeB;
    if (state == SecurityState::RISK) {
        bodyR = 255; bodyG = 60;  bodyB = 60;   // 밝은 빨강
        glowR = 255; glowG = 90;  glowB = 90;
        edgeR = 200; edgeG = 20;  edgeB = 20;
    } else if (state == SecurityState::PARTIAL) {
        bodyR = 255; bodyG = 205; bodyB = 35;   // 밝고 쨍한 노랑(앰버)
        glowR = 255; glowG = 225; glowB = 110;
        edgeR = 205; edgeG = 140; edgeB = 0;
    } else {
        // 밝고 쨍한 형광 초록 본체 + 밝은 초록 글로우 → "빤짝빤짝 빛나는" 느낌
        bodyR = 40;  bodyG = 245; bodyB = 110;
        glowR = 120; glowG = 255; glowB = 170;
        edgeR = 0;   edgeG = 165; edgeB = 70;
    }

    const uint64_t now = GetTickCount64();

    // glow(빛 번짐) 세기 맥동.
    //   RISK: 진입 후 kRiskBlinkMs 동안 빠르게 0.45~1.0 으로 강하게 깜빡.
    //   평소: 은은하게 숨쉬듯 반짝(약 1.5초 주기, 0.78~1.0) → "빤짝빤짝".
    float pulse;
    // 코어(전구) 밝기 반짝임 — 평소에도 중심이 살짝 더 밝아졌다 어두워짐.
    float corePulse;
    if (state == SecurityState::RISK) {
        const uint64_t elapsed = now - m_riskStartMs.load();
        if (elapsed < kRiskBlinkMs) {
            const float p =
                static_cast<float>(elapsed % (kRiskBlinkHalf * 2)) /
                static_cast<float>(kRiskBlinkHalf * 2);            // 0..1
            const float tri = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f; // 0..1..0
            pulse = 0.45f + 0.55f * tri;
        } else {
            pulse = 1.0f;
        }
        corePulse = 0.80f + 0.20f * pulse;
    } else {
        // 부드러운 코사인 숨쉬기 (1500ms 주기)
        const float ph = static_cast<float>(now % 1500) / 1500.0f;
        const float s  = 0.5f - 0.5f * std::cos(ph * kTwoPi);     // 0..1..0
        pulse     = 0.78f + 0.22f * s;
        corePulse = 0.90f + 0.10f * s;
    }

    auto clamp255 = [](float v) -> float {
        return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    };

    uint8_t *buf = static_cast<uint8_t *>(m_bits);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = x - cx;
            const float dy = y - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);

            float r = 0, g = 0, b = 0, a = 0;

            if (d <= kCoreR) {
                // 코어: 중심이 흰빛으로 타오르는 전구(bulb) 느낌(블룸) + 광택 점.
                r = bodyR; g = bodyG; b = bodyB;

                // 중심 블룸: 중심에 가까울수록 흰색으로 → 환하게 빛나는 코어.
                float bloom = 1.0f - (d / kCoreR);   // 1(중심)→0(가장자리)
                bloom = bloom * bloom * corePulse;    // 중심 집중 + 반짝임
                r = r + (255.0f - r) * bloom * 0.90f;
                g = g + (255.0f - g) * bloom * 0.90f;
                b = b + (255.0f - b) * bloom * 0.90f;

                // 좌상단 광택 하이라이트(유리알 반사).
                const float hx = x - (cx - kCoreR * 0.32f);
                const float hy = y - (cy - kCoreR * 0.32f);
                const float hd = std::sqrt(hx * hx + hy * hy);
                const float hr = kCoreR * 0.62f;
                if (hd < hr) {
                    float gloss = 1.0f - hd / hr;
                    gloss = gloss * gloss * 0.85f;       // 강한 광택
                    r = r + (255.0f - r) * gloss;
                    g = g + (255.0f - g) * gloss;
                    b = b + (255.0f - b) * gloss;
                }
                a = 255.0f;
            } else if (d <= kRingOut + 1.0f) {
                // 테두리(rim): edge 색, 바깥 1px 안티앨리어싱
                float aa = 1.0f;
                if (d > kRingOut)
                    aa = 1.0f - (d - kRingOut);          // kRingOut..+1 → 1..0
                if (aa < 0.0f) aa = 0.0f;
                r = edgeR; g = edgeG; b = edgeB;
                a = 255.0f * aa;
            } else if (d <= kGlowOut) {
                // 글로우: 안쪽은 밝게, 바깥으로 부드럽게 감쇠. 안쪽 절반은 거의
                // 균일하게 밝게 유지해 "빛 덩어리"가 크고 또렷하게 보이도록.
                const float t       = (d - kRingOut) / (kGlowOut - kRingOut); // 0..1
                float falloff = 1.0f - t;
                falloff = falloff * falloff * (0.55f + 0.45f * (1.0f - t)); // 안쪽 보강
                r = glowR; g = glowG; b = glowB;
                a = kGlowMaxA * falloff * pulse;
            }

            r = clamp255(r);
            g = clamp255(g);
            b = clamp255(b);
            a = clamp255(a);

            // premultiplied alpha + BGRA 바이트 순서 (UpdateLayeredWindow 요구)
            const float af  = a / 255.0f;
            const int   idx = (y * W + x) * 4;
            buf[idx + 0] = static_cast<uint8_t>(b * af);
            buf[idx + 1] = static_cast<uint8_t>(g * af);
            buf[idx + 2] = static_cast<uint8_t>(r * af);
            buf[idx + 3] = static_cast<uint8_t>(a);
        }
    }

    POINT ptSrc = {0, 0};
    SIZE  sz    = {W, H};
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    // pptDst=NULL → 위치는 그대로 두고 내용/크기만 갱신(드래그 이동과 충돌 방지).
    UpdateLayeredWindow(m_hwnd, NULL, NULL, &sz, m_memDC, &ptSrc, 0, &bf,
                        ULW_ALPHA);
}

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
    // WM_NCHITTEST: 동그라미(코어) 영역만 드래그/우클릭 대상(HTCAPTION),
    //   나머지(투명/글로우)는 클릭 통과(HTTRANSPARENT) → 아래 게임으로 전달.
    // ------------------------------------------------------------------
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        const float cx = kWidth * 0.5f - 0.5f;
        const float cy = kHeight * 0.5f - 0.5f;
        const float dx = pt.x - cx, dy = pt.y - cy;
        if (std::sqrt(dx * dx + dy * dy) <= kHitR)
            return HTCAPTION;        // 코어 위 → 드래그로 창 이동
        return HTTRANSPARENT;        // 그 외 → 클릭 통과
    }

    // ------------------------------------------------------------------
    // WM_EXITSIZEMOVE: 드래그 이동 종료 → 새 위치 저장 + 콜백 통지
    // ------------------------------------------------------------------
    case WM_EXITSIZEMOVE: {
        RECT r;
        if (GetWindowRect(hwnd, &r)) {
            self->m_posX.store(r.left);
            self->m_posY.store(r.top);
            if (self->m_onMoved)
                self->m_onMoved(r.left, r.top);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_NCRBUTTONUP: 동그라미 우클릭 → 즉시 숨김 + 콜백 통지(설정 off)
    //   (WM_NCHITTEST가 HTCAPTION을 반환하므로 우클릭은 NC 메시지로 들어온다.)
    // ------------------------------------------------------------------
    case WM_NCRBUTTONUP: {
        self->m_visible.store(false);
        ShowWindow(hwnd, SW_HIDE);
        if (self->m_onHidden)
            self->m_onHidden();
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_TIMER: 반짝임/맥동 애니메이션 (프레임 공급과 무관하게 부드럽게).
    //   평소엔 은은한 숨쉬기, RISK엔 강한 깜빡임 — 항상 갱신해 살아있게 보인다.
    // ------------------------------------------------------------------
    case WM_TIMER: {
        if (wParam == kPulseTimerId) {
            if (self->m_visible.load())
                self->renderBeacon();
            // 작업표시줄 깜빡임은 경광등 표시 여부와 무관하게 관리.
            self->updateTaskbarFlash();
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_PAINT: 레이어드 창은 UpdateLayeredWindow로 그리므로 검증만 한다.
    // ------------------------------------------------------------------
    case WM_PAINT: {
        ValidateRect(hwnd, nullptr);
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_SC_STATE: setState() 가 전달하는 상태 변경 메시지
    //   wParam = SecurityState as int, lParam = gameMode as bool (0/1)
    // ------------------------------------------------------------------
    case WM_SC_STATE: {
        const int newSt = static_cast<int>(wParam);
        const int oldSt = self->m_state.exchange(newSt);
        self->m_gameMode.store(lParam != 0);
        // SAFE/CAUTION → RISK 전환 시에만 RISK 시작 시각 기록(맥동 타이밍 기준).
        const int risk = static_cast<int>(SecurityState::RISK);
        if (newSt == risk && oldSt != risk)
            self->m_riskStartMs.store(GetTickCount64());
        if (self->m_visible.load())
            self->renderBeacon();
        // [작업표시줄] OBS 아이콘 빨간 배지 — 경광등 표시 여부와 무관하게 갱신.
        self->updateTaskbarOverlay(static_cast<SecurityState>(newSt));
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_SC_VISIBLE: 표시/숨김 토글 (wParam = visible 0/1)
    // ------------------------------------------------------------------
    case WM_SC_VISIBLE: {
        if (wParam) {
            self->m_visible.store(true);
            self->renderBeacon();  // 표시 전에 내용 준비
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        } else {
            self->m_visible.store(false);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // WM_SC_MOVE: 위치 적용 (wParam = x, lParam = y)
    // ------------------------------------------------------------------
    case WM_SC_MOVE: {
        const int x = static_cast<int>(static_cast<LONG_PTR>(wParam));
        const int y = static_cast<int>(static_cast<LONG_PTR>(lParam));
        self->m_posX.store(x);
        self->m_posY.store(y);
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kPulseTimerId);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
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
    wc.hCursor       = LoadCursor(nullptr, IDC_SIZEALL);  // 이동 가능 힌트
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;  // UpdateLayeredWindow로 직접 그림

    RegisterClassEx(&wc);  // 이미 등록된 경우 무시 (ERROR_CLASS_ALREADY_EXISTS)

    // --- 배치: 저장된 위치(있으면) 또는 기본 모니터 우상단 ---
    int posX = m_posX.load();
    int posY = m_posY.load();
    if (posX < 0 || posY < 0) {
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        posX = screenW - kWidth - 24;
        posY = 24;
        m_posX.store(posX);
        m_posY.store(posY);
    }

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED   |    // 퍼-픽셀 알파 (UpdateLayeredWindow)
        WS_EX_TOPMOST   |    // 항상 최상위
        WS_EX_NOACTIVATE|    // 포커스 훔치지 않음
        WS_EX_TOOLWINDOW,    // 작업표시줄에 나타나지 않음
        kClassName,
        L"SecureCast",
        WS_POPUP,            // SW_SHOWNOACTIVATE로 별도 표시
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

    // --- 알파 렌더 표면 준비 ---
    if (!ensureSurface()) {
        blog(LOG_WARNING,
             "[SecureCast][D] OverlayWindow: 알파 표면 생성 실패.");
    }

    // --- SetWindowDisplayAffinity: 캡처 소프트웨어에서 보이지 않게 ---
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

    // --- 첫 렌더 후, 설정상 표시 상태면 화면에 띄움 ---
    renderBeacon();
    if (m_visible.load()) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }

    // RISK 맥동 애니메이션용 타이머
    SetTimer(hwnd, kPulseTimerId, kPulseTimerMs, nullptr);

    // [작업표시줄] OBS 아이콘 빨간 배지 점등 준비 + 현재 상태 반영
    initTaskbar();
    updateTaskbarOverlay(static_cast<SecurityState>(m_state.load()));

    m_ready.store(true);  // create() 블로킹 해제

    // --- 메시지 루프 ---
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 루프 종료 후 작업표시줄 배지 제거 + 표면/클래스 정리
    shutdownTaskbar();
    releaseSurface();
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

    PostMessage(m_hwnd, WM_SC_STATE, static_cast<WPARAM>(state),
                static_cast<LPARAM>(gameMode ? 1 : 0));
}

// =============================================================================
// setVisible — 임의 스레드에서 표시/숨김 토글 (설정 체크박스에서 사용)
// =============================================================================
void OverlayWindow::setVisible(bool visible)
{
    m_visible.store(visible);
    if (m_hwnd)
        PostMessage(m_hwnd, WM_SC_VISIBLE, visible ? 1 : 0, 0);
}

// =============================================================================
// setPosition — 임의 스레드에서 위치 적용 (저장된 좌표 복원에 사용)
// =============================================================================
void OverlayWindow::setPosition(int x, int y)
{
    if (x < 0 || y < 0)
        return;  // 음수 = 기본 위치 유지
    m_posX.store(x);
    m_posY.store(y);
    if (m_hwnd)
        PostMessage(m_hwnd, WM_SC_MOVE, static_cast<WPARAM>(x),
                    static_cast<LPARAM>(y));
}

// =============================================================================
// BeaconManager — 프로세스 전역 단일 경광등 (싱글톤) 구현
// =============================================================================
BeaconManager &BeaconManager::instance()
{
    static BeaconManager s_instance;
    return s_instance;
}

void BeaconManager::recomputeStateLocked()
{
    SecurityState maxSt = SecurityState::SAFE;
    bool anyGame = false;
    for (const auto &kv : m_entries) {
        if (static_cast<int>(kv.second.state) > static_cast<int>(maxSt))
            maxSt = kv.second.state;
        anyGame = anyGame || kv.second.gameMode;
    }
    // 변경이 있을 때만 경광등에 전달 (매 프레임 reportState 호출 대비).
    if (!m_haveLast || maxSt != m_lastState || anyGame != m_lastGame) {
        m_lastState = maxSt;
        m_lastGame  = anyGame;
        m_haveLast  = true;
        m_overlay.setState(maxSt, anyGame);
    }
}

void BeaconManager::acquire(const void *id, int x, int y, bool visible,
                            void *obsMainHwnd,
                            OverlayWindow::MovedCallback onMoved,
                            OverlayWindow::HiddenCallback onHidden)
{
    std::lock_guard<std::mutex> lk(m_);

    Entry e;
    e.onMoved  = std::move(onMoved);
    e.onHidden = std::move(onHidden);
    m_entries[id] = std::move(e);

    if (!m_created) {
        // 첫 필터 → 실제 경광등을 만든다. 소유자는 이 필터.
        m_owner = id;
        m_overlay.setInitialState(x, y, visible);
        m_overlay.setTaskbarTarget(obsMainHwnd);

        // 오버레이 콜백은 매니저에 한 번만 묶어두고, 내부에서 "현재 소유자"의
        // 콜백으로 위임한다. 이렇게 하면 소유자가 바뀌어도 재등록이 필요 없다.
        m_overlay.setCallbacks(
            [this](int mx, int my) {
                OverlayWindow::MovedCallback cb;
                {
                    std::lock_guard<std::mutex> lk2(m_);
                    auto it = m_entries.find(m_owner);
                    if (it != m_entries.end())
                        cb = it->second.onMoved;
                }
                if (cb)
                    cb(mx, my);
            },
            [this]() {
                OverlayWindow::HiddenCallback cb;
                {
                    std::lock_guard<std::mutex> lk2(m_);
                    auto it = m_entries.find(m_owner);
                    if (it != m_entries.end())
                        cb = it->second.onHidden;
                }
                if (cb)
                    cb();
            });

        m_overlay.create();
        m_created = true;
        m_haveLast = false;  // 첫 상태를 반드시 전달
    }
    // 두 번째 이후 필터: 공유만 한다(새 창을 만들지 않음 → 경광등은 항상 하나).

    recomputeStateLocked();
}

void BeaconManager::release(const void *id)
{
    bool doDestroy = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        m_entries.erase(id);

        if (m_entries.empty()) {
            // 마지막 필터 → 경광등 파괴 (락 밖에서 수행: destroy()가 스레드를
            // join하는데 그 스레드가 매니저 콜백에서 m_를 기다리면 데드락).
            doDestroy = m_created;
            m_created = false;
            m_owner = nullptr;
            m_haveLast = false;
        } else {
            if (m_owner == id)
                m_owner = m_entries.begin()->first;  // 소유권 이전
            recomputeStateLocked();
        }
    }
    if (doDestroy)
        m_overlay.destroy();
}

void BeaconManager::reportState(const void *id, SecurityState state,
                                bool gameMode)
{
    std::lock_guard<std::mutex> lk(m_);
    auto it = m_entries.find(id);
    if (it == m_entries.end())
        return;
    it->second.state = state;
    it->second.gameMode = gameMode;
    recomputeStateLocked();
}

void BeaconManager::setVisible(const void * /*id*/, bool visible)
{
    std::lock_guard<std::mutex> lk(m_);
    if (m_created)
        m_overlay.setVisible(visible);  // 어느 필터에서든 단일 경광등에 반영
}

void BeaconManager::setPosition(const void * /*id*/, int x, int y)
{
    if (x < 0 || y < 0)
        return;
    std::lock_guard<std::mutex> lk(m_);
    if (m_created)
        m_overlay.setPosition(x, y);
}

#endif // _WIN32
