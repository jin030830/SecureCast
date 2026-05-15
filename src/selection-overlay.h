// =============================================================================
// selection-overlay.h — 수동 드래그 블러 영역 선택용 Win32 전체화면 오버레이
//
// 동작:
//   SelectionOverlay::start(cb) → 전체화면 투명 창이 열리고 드래그 대기
//   좌클릭 드래그 → 파란 선택 박스 실시간 표시
//   마우스 업     → cb(BlurRect) 호출 후 창 닫힘
//   ESC           → 취소 (cb 미호출)
//
// 좌표계:
//   반환 BlurRect는 모니터 상대 픽셀 좌표 (primary monitor 기준).
//   Display Capture 소스라면 소스 픽셀 좌표와 1:1 대응.
//
// 스레드:
//   start() → 별도 스레드에서 Win32 메시지 루프 실행.
//   cancel() → PostMessage(WM_CLOSE) 로 스레드에 종료 요청 (thread-safe).
// =============================================================================
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>
#include "securecast-types.h"

class SelectionOverlay {
public:
    // 드래그 완료 시 호출되는 콜백. 모니터 상대 픽셀 좌표 BlurRect를 전달.
    using DoneCallback = std::function<void(BlurRect)>;

    SelectionOverlay()  = default;
    ~SelectionOverlay() { cancel(); if (m_thread.joinable()) m_thread.join(); }

    SelectionOverlay(const SelectionOverlay&)            = delete;
    SelectionOverlay& operator=(const SelectionOverlay&) = delete;

    // 선택 모드 시작. 이미 활성화 중이면 무시.
    void start(DoneCallback cb);

    // 선택 모드 강제 종료 (ESC와 동일 효과).
    void cancel();

    // cancel() 후 메시지 루프 스레드가 완전히 종료될 때까지 블로킹 대기.
    void wait_and_join();

    bool isActive() const { return m_running.load(std::memory_order_acquire); }

private:
    void messageLoop();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 선택 사각형 그리기 (WM_PAINT)
    static void paintSelection(HWND hwnd, POINT start, POINT cur, bool dragging);

    static constexpr LPCTSTR kClassName  = L"SecureCastSelectionOverlay";

    HWND             m_hwnd = NULL;
    std::thread      m_thread;
    std::atomic<bool> m_running{false};

    DoneCallback m_callback;

    // 드래그 상태 (message loop 스레드에서만 접근)
    bool  m_dragging = false;
    POINT m_start{};
    POINT m_cur{};

    // 모니터 원점 (좌표 변환용)
    int m_monLeft = 0;
    int m_monTop  = 0;
};

#endif // _WIN32
