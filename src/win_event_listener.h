// =============================================================================
// win_event_listener.h — SetWinEventHook 기반 push 감지 (Role A 4주차 사전작업)
//
// 가이드 §5.1 권장 구조: 별도 스레드 + 메시지 펌프 + WINEVENT_OUTOFCONTEXT.
// EVENT_OBJECT_SHOW / HIDE / DESTROY / LOCATIONCHANGE 를 후킹해 OS 이벤트가
// 도착할 때마다 atomic needRescan 플래그를 set. 다음 video_tick 에서 그 플래그를
// 보고 풀 스캔 throttle 을 무시하고 즉시 처리.
//
// 효과:
//   - 카톡 등장/사라짐/위치변경 평균 latency 50ms → ~16ms (1 video_tick)
//   - 풀 스캔 throttle 을 더 길게 (예: 500ms) 잡아도 push 로 즉시 반응 → CPU 절감
//
// 안전성:
//   - WINEVENT_OUTOFCONTEXT 는 DLL 인젝션 없이 OS 이벤트만 받는 안전 모드
//   - WINEVENT_SKIPOWNPROCESS 로 자기 자신 이벤트 제외 (loop 방지)
//
// 사용:
//   filter->listener.start();          // 보통 securecast_create
//   if (filter->listener.checkAndClearRescan()) { /* 즉시 풀 스캔 */ }
//   filter->listener.stop();           // securecast_destroy
//
// 멀티 인스턴스: 현재 글로벌 atomic 1개 사용 → 사실상 인스턴스 1개 가정.
//                추가 인스턴스가 등록되면 그쪽도 동일 콜백을 공유.
// =============================================================================
#pragma once

#ifdef _WIN32

#include <windows.h>
#include <atomic>
#include <thread>

class WinEventListener {
public:
    WinEventListener() = default;
    ~WinEventListener() { stop(); }

    WinEventListener(const WinEventListener&)            = delete;
    WinEventListener& operator=(const WinEventListener&) = delete;

    void start();
    void stop();

    // video_tick 에서 호출. 이벤트가 한 번이라도 있었으면 true 를 반환하면서 flag 리셋.
    bool checkAndClearRescan() {
        return m_needRescan.exchange(false, std::memory_order_acq_rel);
    }

private:
    void run();

    static void CALLBACK eventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild,
                                   DWORD idEventThread, DWORD dwmsEventTime);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    DWORD             m_threadId    = 0;
    HWINEVENTHOOK     m_hookGroup1  = nullptr; // SHOW / HIDE / DESTROY
    HWINEVENTHOOK     m_hookGroup2  = nullptr; // SYSTEM_FOREGROUND (포그라운드 전환)

    std::atomic<bool> m_needRescan{false};

    // 콜백에서 도달하기 위한 스레드별 self 포인터 (한 프로세스에 instance 1개 가정).
    static std::atomic<WinEventListener*> s_active;
};

#endif // _WIN32
