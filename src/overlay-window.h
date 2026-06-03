// =============================================================================
// overlay-window.h — 스트리머 전용 보안 상태 HUD 오버레이 (경광등)
//
// 역할:
//   SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)를 적용한 Win32 팝업 창으로
//   스트리머 모니터에만 보이는 보안 상태 경광등(평소 초록 / RISK 빨강)을 표시한다.
//   OBS를 포함한 모든 화면 캡처 소프트웨어에서는 이 창이 보이지 않는다.
//
//   렌더링은 UpdateLayeredWindow(퍼-픽셀 알파)로 처리해 "진짜 불이 켜진 것 같은"
//   광택 + 글로우(빛 번짐)를 가진 LED 느낌의 경광등을 그린다.
//
// 사용처:
//   - SecureCastFilter::overlay 멤버로 보유
//   - securecast_create()에서 create() 호출 (그 전에 setInitialState/setCallbacks)
//   - securecast_destroy()에서 destroy() 호출
//   - currentState 변경 시 setState() 호출 (any thread safe)
//
// 사용자 조작:
//   - 동그라미를 마우스로 드래그 → 위치 이동 (이동 후 onMoved 콜백으로 설정 저장)
//   - 동그라미를 우클릭 → 즉시 숨김 (onHidden 콜백으로 설정 off 저장)
//   - 설정 체크박스 → setVisible(true/false) 로 표시/숨김
//
// Win32 전용: _WIN32 매크로 보호 아래에서만 활성화됨
// =============================================================================

#pragma once

#ifdef _WIN32

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "securecast-types.h"  // SecurityState

// WDA_EXCLUDEFROMCAPTURE: Windows 10 2004 (build 19041)+ 에서 지원
// 구버전에서는 WDA_MONITOR fallback
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ----------------------------------------------------
// OverlayWindow
//
// 동작 방식:
//   1. create() 호출 → 별도 스레드에서 Win32 윈도우 생성 + 메시지 루프 시작
//   2. 윈도우에 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 적용
//      → OBS/캡처 소프트웨어에서는 보이지 않음
//   3. setState() 호출 → WM_APP 메시지로 스레드에 전달 → UpdateLayeredWindow 갱신
//   4. destroy() 호출 → WM_CLOSE 전송 → 스레드 join → 윈도우 파괴
// ----------------------------------------------------
class OverlayWindow {
public:
    // 위치 이동 콜백: 사용자가 드래그를 끝냈을 때 새 화면 좌표(x,y)와 함께 호출.
    // 숨김 콜백: 사용자가 경광등을 우클릭해 숨겼을 때 호출.
    // 둘 다 오버레이 스레드에서 호출되므로 콜백 구현은 thread-safe해야 한다.
    using MovedCallback  = std::function<void(int x, int y)>;
    using HiddenCallback = std::function<void()>;

    OverlayWindow()  = default;
    ~OverlayWindow() { destroy(); }

    // create() 호출 전에 저장된 상태를 복원한다.
    //   x,y : 저장된 화면 좌표 (-1 이면 기본 위치 = 기본 모니터 우상단)
    //   visible : 설정상 경광등 표시 여부 (false면 창은 만들되 숨김 상태로 시작)
    void setInitialState(int x, int y, bool visible);

    // create() 호출 전에 이동/숨김 콜백을 등록한다.
    void setCallbacks(MovedCallback onMoved, HiddenCallback onHidden);

    // 오버레이 윈도우 생성 및 캡처 제외 처리. 실패 시 false 반환(로그만 남기고 계속)
    bool create();

    // 윈도우 파괴 및 메시지 루프 스레드 종료
    void destroy();

    // 보안 상태 갱신 — 임의 스레드에서 호출 가능.
    // ocrDisabled: OCR PII 마스킹 토글이 꺼진 상태(=보호 안 함). true면 경광등을
    // 회색으로 표시해 "OCR OFF"를 한눈에 보여준다.
    void setState(SecurityState state, bool gameMode, bool ocrDisabled = false);

    // 표시/숨김 토글 — 임의 스레드에서 호출 가능 (설정 체크박스에서 사용).
    void setVisible(bool visible);
    bool isVisible() const { return m_visible.load(); }

    // 위치 이동 — 임의 스레드에서 호출 가능 (설정에서 저장된 좌표 적용).
    // x<0 또는 y<0 이면 무시(기본 위치 유지).
    void setPosition(int x, int y);

    // [작업표시줄 점등] RISK일 때 빨간 배지를 띄울 OBS 메인 창 핸들 설정.
    // create() 전에 호출해야 한다(메시지 루프 스레드에서 COM/타스크바 초기화).
    void setTaskbarTarget(void *obsMainHwnd);

    bool isCreated() const { return m_hwnd != NULL; }

private:
    // 메시지 루프를 실행하는 워커 스레드 함수
    void messageLoop();

    // Win32 윈도우 프로시저
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    // 경광등을 퍼-픽셀 알파 DIB에 그려 UpdateLayeredWindow로 갱신한다.
    // 평소 초록(광택+글로우), RISK 진입 후 kRiskBlinkMs 동안 빨강 맥동(pulse),
    // 이후 솔리드 빨강. m_state / m_riskStartMs 를 읽어 렌더링한다.
    void renderBeacon();

    // 오버레이 스레드에서 알파 DIB(섹션)와 메모리 DC를 준비/해제한다.
    bool ensureSurface();
    void releaseSurface();

    // [작업표시줄 점등] 메시지 루프 스레드에서 COM + ITaskbarList3 초기화/해제,
    // 그리고 상태에 따라 OBS 아이콘에 빨간 배지(overlay icon)를 갱신한다.
    void initTaskbar();
    void shutdownTaskbar();
    void updateTaskbarOverlay(SecurityState state);  // 빨간 배지(상태 변화 시)
    // 작업표시줄 버튼 깜빡임 상태머신 (펄스 타이머가 매 틱 호출).
    //   RISK + 백그라운드 : 계속 깜빡 / RISK + 활성창 : 10초만 깜빡 후 정지.
    void updateTaskbarFlash();

    // ---- 멤버 ----
    HWND               m_hwnd    = NULL;
    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // SecurityState를 int로 저장 (atomic 지원을 위해)
    std::atomic<int>   m_state{static_cast<int>(SecurityState::SAFE)};

    // [T11] 게임 모드 활성 여부. WM_SC_STATE 메시지의 lParam으로 전달됨.
    std::atomic<bool>  m_gameMode{false};

    // [OCR 표시] OCR PII 마스킹 토글 꺼짐 여부. WM_SC_STATE lParam 비트1로 전달.
    // true면 경광등을 회색(=보호 안 함)으로 렌더해 OCR OFF를 시각화.
    std::atomic<bool>  m_ocrDisabled{false};

    // [UI] RISK로 진입한 시각(GetTickCount64 ms). RISK 진입 시 갱신, 맥동/솔리드
    // 판정에 사용. RISK가 아니면 의미 없음(초록 경광등).
    std::atomic<uint64_t> m_riskStartMs{0};

    // [UI] 현재 화면 좌표(드래그/복원으로 갱신). -1 = 아직 미배치(기본 위치 사용).
    std::atomic<int>   m_posX{-1};
    std::atomic<int>   m_posY{-1};

    // [UI] 경광등 표시 여부 (설정 토글 + 우클릭 숨김으로 갱신).
    std::atomic<bool>  m_visible{true};

    // [UI] 사용자 조작 알림 콜백 (필터가 설정에 영속화). create() 전에 설정됨 →
    // 오버레이 스레드만 읽으므로 별도 동기화 없이 사용.
    MovedCallback  m_onMoved;
    HiddenCallback m_onHidden;

    // RISK 진입 후 이 시간(ms) 동안 빨강 맥동, 이후 솔리드 빨강.
    static constexpr uint64_t kRiskBlinkMs   = 10000;  // 맥동 지속 10초
    static constexpr uint64_t kRiskBlinkHalf = 500;    // 맥동 반주기(ms)

    // 윈도우 클래스 이름 (인스턴스마다 고유)
    static constexpr wchar_t kClassName[] = L"SecureCastOverlayV1";

    // [UI] 경광등 창 크기 — 글로우(빛 번짐)까지 담기 위해 정사각으로 넉넉히 잡는다.
    // 실제 동그라미(코어)는 중앙의 작은 원이고 나머지는 투명/글로우.
    // 멀리서도 눈에 들어오되 과하지 않은 크기(글로우 반경 포함).
    static constexpr int kWidth  = 56;
    static constexpr int kHeight = 56;

    // 펄스 애니메이션 타이머 (RISK 맥동이 프레임 공급과 무관하게 부드럽게).
    static constexpr UINT_PTR kPulseTimerId = 1;
    static constexpr UINT     kPulseTimerMs = 50;  // ≈20fps

    // ---- 알파 렌더 표면 (오버레이 스레드 전용) ----
    HDC      m_memDC  = NULL;     // 메모리 DC (DIB 선택됨)
    HBITMAP  m_dib    = NULL;     // 32bpp top-down DIB 섹션
    HBITMAP  m_oldBmp = NULL;     // m_memDC 원래 비트맵 (복원용)
    void*    m_bits   = nullptr;  // DIB 픽셀 버퍼 (BGRA, premultiplied)

    // ---- 작업표시줄 OBS 아이콘 점등 (오버레이 스레드 전용) ----
    HWND     m_obsHwnd        = NULL;     // 빨간 배지를 띄울 OBS 메인 창
    void    *m_taskbar        = nullptr;  // ITaskbarList3* (헤더 의존 분리 위해 void*)
    HICON    m_riskIcon       = NULL;     // 빨간 원형 overlay 아이콘
    bool     m_comInit        = false;    // 이 스레드에서 CoInitialize 성공 여부
    int      m_lastTaskbarRisk = -1;      // 배지 마지막 적용(0/1, -1=미적용) — 중복 호출 방지

    // 작업표시줄 버튼 깜빡임 상태머신용 (오버레이 스레드 전용)
    bool     m_flashActive  = false;      // 현재 FlashWindowEx(TIMER) 진행 중인지
    bool     m_fgFlashDone  = false;      // 활성창 10초 깜빡임을 이미 끝냈는지
    uint64_t m_flashFgStart = 0;          // 활성창 깜빡임 시작 시각(ms)
    static constexpr uint64_t kFgFlashMs = 10000;  // 활성창일 때 깜빡임 지속(10초)

    // WDA_EXCLUDEFROMCAPTURE 지원 여부 (create()에서 판단, messageLoop()에서 사용)
    bool               m_useExcludeFromCapture{false};

    // WndProc ↔ messageLoop 초기화 완료 신호
    std::atomic<bool> m_ready{false};

    // WM_APP + 0 : 보안 상태 변경 통지 (wParam = SecurityState as int)
    static constexpr UINT WM_SC_STATE   = WM_APP + 0;
    // WM_APP + 1 : 표시/숨김 통지 (wParam = visible 0/1)
    static constexpr UINT WM_SC_VISIBLE = WM_APP + 1;
    // WM_APP + 2 : 위치 이동 통지 (wParam = x, lParam = y)
    static constexpr UINT WM_SC_MOVE    = WM_APP + 2;
};

// =============================================================================
// BeaconManager — 프로세스 전역 단일 경광등 관리자 (싱글톤)
//
// 문제: SecureCast 필터를 여러 소스에 적용하면 필터 인스턴스마다 경광등이
//   하나씩 생겨 화면에 경광등이 여러 개 뜬다. 하지만 "내 화면의 보안 표시등"은
//   소스 개수와 무관하게 하나여야 자연스럽다.
//
// 해결: 모든 필터가 이 매니저를 통해 단 하나의 OverlayWindow를 공유한다.
//   - acquire(): 첫 필터가 들어올 때 실제 경광등 생성, 이후 필터는 공유만.
//   - release(): 마지막 필터가 나갈 때 경광등 파괴.
//   - reportState(): 각 필터의 보안 상태를 모아 "가장 높은 위험 등급"으로 표시.
//   - setVisible()/setPosition(): 어느 필터에서 토글/이동해도 그 하나에 반영.
//
// 모든 메서드는 thread-safe (내부 뮤텍스). 필터 포인터를 키로 사용한다.
// =============================================================================
class BeaconManager {
public:
    static BeaconManager &instance();

    // 필터 등록. 첫 등록 시 경광등을 생성한다. id = 필터 포인터(고유 키).
    //   x,y,visible : 이 필터 설정에 저장된 경광등 위치/표시 상태(첫 필터만 적용).
    //   obsMainHwnd : RISK 시 빨간 배지를 띄울 OBS 메인 창 핸들(첫 필터만 적용).
    //   onMoved/onHidden : 사용자가 드래그/우클릭숨김 시 설정 저장용 콜백.
    void acquire(const void *id, int x, int y, bool visible, void *obsMainHwnd,
                 OverlayWindow::MovedCallback onMoved,
                 OverlayWindow::HiddenCallback onHidden);

    // 필터 해제. 마지막 필터면 경광등을 파괴한다.
    void release(const void *id);

    // 이 필터의 보안 상태 보고. 전체 필터 중 최고 위험 등급으로 경광등을 갱신.
    // ocrDisabled: 전역 OCR 토글 꺼짐 상태(모든 필터 공통). 켜지면 경광등 회색.
    void reportState(const void *id, SecurityState state, bool gameMode,
                     bool ocrDisabled = false);

    // 표시/숨김 토글 (설정 체크박스). 어느 필터에서든 단일 경광등에 반영.
    void setVisible(const void *id, bool visible);

    // 위치 적용 (저장된 좌표 복원). x<0/y<0이면 무시.
    void setPosition(const void *id, int x, int y);

private:
    BeaconManager() = default;

    struct Entry {
        SecurityState state = SecurityState::SAFE;
        bool gameMode = false;
        OverlayWindow::MovedCallback  onMoved;
        OverlayWindow::HiddenCallback onHidden;
    };

    // m_ 보유 상태에서 최고 위험 등급을 계산해 경광등에 반영(변경 시에만 전달).
    void recomputeStateLocked();

    std::mutex m_;
    OverlayWindow m_overlay;
    std::unordered_map<const void *, Entry> m_entries;
    const void *m_owner = nullptr;  // 드래그/숨김 콜백을 위임받는 현재 소유 필터
    bool m_created = false;

    // 전역 OCR 토글 꺼짐 상태(필터 공통, 마지막 reportState 값).
    bool m_ocrDisabled = false;

    // 마지막으로 경광등에 전달한 상태(불필요한 반복 PostMessage 방지).
    SecurityState m_lastState = SecurityState::SAFE;
    bool m_lastGame = false;
    bool m_lastOcr = false;
    bool m_haveLast = false;
};

#endif // _WIN32
