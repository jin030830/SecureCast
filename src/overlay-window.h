// =============================================================================
// overlay-window.h — 스트리머 전용 보안 상태 HUD 오버레이
//
// 역할:
//   SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)를 적용한 Win32 팝업 창으로
//   스트리머 모니터에만 보이는 보안 상태 배지(SAFE/CAUTION/RISK)를 표시한다.
//   OBS를 포함한 모든 화면 캡처 소프트웨어에서는 이 창이 보이지 않는다.
//
// 사용처:
//   - SecureCastFilter::overlay 멤버로 보유
//   - securecast_create()에서 create() 호출
//   - securecast_destroy()에서 destroy() 호출
//   - currentState 변경 시 setState() 호출 (any thread safe)
//
// Win32 전용: _WIN32 매크로 보호 아래에서만 활성화됨
// =============================================================================

#pragma once

#ifdef _WIN32

#include <Windows.h>
#include <atomic>
#include <thread>

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
//      → OBS/캡처 소프트웨어에서는 투명(검정)으로 보임
//   3. setState() 호출 → WM_APP 메시지로 스레드에 전달 → WM_PAINT 갱신
//   4. destroy() 호출 → WM_QUIT 전송 → 스레드 join → 윈도우 파괴
// ----------------------------------------------------
class OverlayWindow {
public:
    OverlayWindow()  = default;
    ~OverlayWindow() { destroy(); }

    // 오버레이 윈도우 생성 및 캡처 제외 처리. 실패 시 false 반환(로그만 남기고 계속)
    bool create();

    // 윈도우 파괴 및 메시지 루프 스레드 종료
    void destroy();

    // 보안 상태 갱신 — 임의 스레드에서 호출 가능
    void setState(SecurityState state);

    bool isCreated() const { return m_hwnd != NULL; }

private:
    // 메시지 루프를 실행하는 워커 스레드 함수
    void messageLoop();

    // Win32 윈도우 프로시저
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    // 배지를 그리는 GDI 헬퍼 (WM_PAINT 내부에서 호출)
    static void paintBadge(HWND hwnd, SecurityState state);

    // ---- 멤버 ----
    HWND               m_hwnd    = NULL;
    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // SecurityState를 int로 저장 (atomic 지원을 위해)
    std::atomic<int>   m_state{static_cast<int>(SecurityState::SAFE)};

    // 윈도우 클래스 이름 (인스턴스마다 고유)
    static constexpr wchar_t kClassName[] = L"SecureCastOverlayV1";

    // 배지 크기 (픽셀)
    static constexpr int kWidth  = 200;
    static constexpr int kHeight =  48;

    // WDA_EXCLUDEFROMCAPTURE 지원 여부 (create()에서 판단, messageLoop()에서 사용)
    bool               m_useExcludeFromCapture{false};

    // WndProc ↔ messageLoop 초기화 완료 신호
    std::atomic<bool> m_ready{false};

    // WM_APP + 0 : 보안 상태 변경 통지 (wParam = SecurityState as int)
    static constexpr UINT WM_SC_STATE = WM_APP + 0;
};

#endif // _WIN32
