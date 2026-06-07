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
#include <cstdint>
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

    // [Freeze T04] 현재 윈도우 이벤트 세대. 어떤 top-level 창의 생성/소멸/표시/
    // 숨김/z-order/포그라운드/위치 변화가 있을 때마다 증가한다. VisibleSubrectsCache
    // 가 캐시 유효성 판정에 사용 — 세대가 달라진 캐시 항목은 stale로 간주해 재계산.
    // 프로세스 전역(단일 인스턴스 가정과 동일). 어느 스레드에서나 호출 가능.
    static uint64_t eventGeneration() {
        return s_eventGeneration.load(std::memory_order_acquire);
    }

private:
    void run();

    static void CALLBACK eventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild,
                                   DWORD idEventThread, DWORD dwmsEventTime);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    DWORD             m_threadId    = 0;
    HWINEVENTHOOK     m_hookGroup1  = nullptr; // CREATE/DESTROY/SHOW/HIDE/REORDER
    HWINEVENTHOOK     m_hookGroup2  = nullptr; // SYSTEM_FOREGROUND (포그라운드 전환)
    // [Freeze T04] LOCATIONCHANGE(창 위치 이동). 캐시 무효화(세대 증가)에만 쓰고
    // 풀 스캔 rescan은 트리거하지 않는다 — 기존 설계가 CPU 때문에 피한 "위치
    // 이동마다 풀 스캔"을 재발시키지 않으면서 stale 노출 구멍만 닫기 위함.
    HWINEVENTHOOK     m_hookGroup3  = nullptr; // EVENT_OBJECT_LOCATIONCHANGE

    std::atomic<bool> m_needRescan{false};

    // 콜백에서 도달하기 위한 스레드별 self 포인터 (한 프로세스에 instance 1개 가정).
    static std::atomic<WinEventListener*> s_active;

    // [Freeze T04] 이벤트 세대 카운터 (전역). eventProc에서 증가.
    static std::atomic<uint64_t> s_eventGeneration;
};

#endif // _WIN32
