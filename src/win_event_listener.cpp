// =============================================================================
// win_event_listener.cpp — SetWinEventHook listener 구현
// =============================================================================

#ifdef _WIN32

#include "win_event_listener.h"
#include "plugin-support.h"
#include <obs.h>            // blog, LOG_INFO/ERROR

std::atomic<WinEventListener*> WinEventListener::s_active{nullptr};
// [Freeze T04] 이벤트 세대 — eventProc에서 top-level 창 이벤트마다 증가.
std::atomic<uint64_t> WinEventListener::s_eventGeneration{0};

void WinEventListener::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true))
        return; // 이미 동작 중

    // 한 번에 한 인스턴스만 글로벌 active 로 설정. 나머지는 보조 (이벤트 받지 않음).
    WinEventListener* expectedActive = nullptr;
    s_active.compare_exchange_strong(expectedActive, this);

    m_thread = std::thread([this]() { run(); });
}

void WinEventListener::stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false))
        return;

    // 스레드 깨우기: 메시지 펌프에 WM_QUIT 보냄.
    if (m_threadId)
        PostThreadMessage(m_threadId, WM_QUIT, 0, 0);

    if (m_thread.joinable())
        m_thread.join();

    WinEventListener* expectedActive = this;
    s_active.compare_exchange_strong(expectedActive, nullptr);
}

void WinEventListener::run()
{
    m_threadId = GetCurrentThreadId();

    // 그룹 1: 생성/소멸/표시/숨김/재정렬 (모든 윈도우 대상).
    // 주의: SetWinEventHook은 [eventMin, eventMax] 폐구간이며 eventMin > eventMax 면
    // 어떤 이벤트도 받지 못한다. 기존 (SHOW=0x8002, DESTROY=0x8001) 범위는
    // min>max 라 그룹 1 훅이 사실상 무효였다. CREATE(0x8000)~REORDER(0x8004)로
    // 정상화하면 SHOW/HIDE/DESTROY 전부 커버.
    m_hookGroup1 = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 그룹 2: 포그라운드 변경 — 카톡이 앞으로 오거나 다른 앱이 앞으로 와서 카톡이
    // 뒤로 가는 순간을 감지. EVENT_OBJECT_LOCATIONCHANGE(위치 이동)와 달리 포그라운드
    // 전환은 드물게 발생하므로 CPU 부담 없이 추가 가능.
    m_hookGroup2 = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 그룹 3 [Freeze T04 — 비활성]: 가시영역 캐시를 제거하면서 LOCATIONCHANGE 훅도
    // 등록하지 않는다. 이 훅은 캐시 무효화 전용이었고, 캐시가 없으면 잦은 위치 이동
    // 콜백이 순수 CPU 오버헤드(기존 설계가 의도적으로 피한 부분)이기 때문.
    // 캐시 재구현 시 여기 훅을 다시 등록하면 된다.

    if (!m_hookGroup1) {
        blog(LOG_ERROR, "[SecureCast] SetWinEventHook failed; falling back to polling only.");
    } else {
        blog(LOG_INFO, "[SecureCast] WinEventListener started (CREATE/DESTROY/SHOW/HIDE/REORDER + FOREGROUND).");
    }

    // 메시지 펌프 — WINEVENT_OUTOFCONTEXT 콜백은 이 펌프에서 dispatch 됨.
    // [Hang-fix] WM_QUIT에 의존하지 않고 100ms마다 m_running을 재확인한다.
    // stop()은 m_threadId(비원자적)를 동기화 없이 읽어, 스레드가 막 시작돼
    // m_threadId가 아직 0이면 WM_QUIT를 보내지 못한다. 또 스레드 메시지 큐가
    // 생성되기 전에 PostThreadMessage가 호출되면 WM_QUIT가 유실된다. 둘 중
    // 어느 경우든 기존 GetMessage는 영원히 블록되어 stop()의 join()이 데드락
    // 했다. 타임아웃 펌프는 그 상황에서도 100ms 내에 루프를 종료한다.
    // (OUTOFCONTEXT 콜백은 PeekMessage 펌프로도 정상 dispatch 된다.)
    MSG msg{};
    while (m_running.load(std::memory_order_acquire)) {
        DWORD wr = MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, 0);
        if (wr == WAIT_FAILED)
            break;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running.store(false, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (m_hookGroup1) UnhookWinEvent(m_hookGroup1);
    if (m_hookGroup2) UnhookWinEvent(m_hookGroup2);
    if (m_hookGroup3) UnhookWinEvent(m_hookGroup3);
    m_hookGroup1 = nullptr;
    m_hookGroup2 = nullptr;
    m_hookGroup3 = nullptr;
    m_threadId   = 0;

    blog(LOG_INFO, "[SecureCast] WinEventListener stopped.");
}

void CALLBACK WinEventListener::eventProc(HWINEVENTHOOK /*hHook*/, DWORD event,
                                          HWND hwnd, LONG idObject, LONG idChild,
                                          DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    // OBJID_WINDOW + CHILDID_SELF 만 처리 (탑레벨 윈도우 자체 이벤트).
    // 그 외 (메뉴, 컨트롤, 캐럿, 커서 LOCATIONCHANGE 등) 는 노이즈.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr)
        return;

    // [Freeze T04] 어떤 top-level 창 이벤트든 가시영역 캐시를 무효화하도록 세대
    // 증가. self(active 인스턴스) 유무와 무관 — 세대는 전역이라 항상 갱신.
    s_eventGeneration.fetch_add(1, std::memory_order_acq_rel);

    WinEventListener* self = s_active.load(std::memory_order_acquire);
    if (!self)
        return;

    // [Freeze T04] LOCATIONCHANGE는 캐시 무효화(세대)에만 쓰고 풀 스캔 rescan은
    // 트리거하지 않는다 — 잦은 위치 이동마다 video_tick 풀 스캔이 도는 CPU 부담을
    // 피하기 위함. 나머지(생성/소멸/표시/숨김/z-order/포그라운드)는 종전대로 rescan.
    if (event != EVENT_OBJECT_LOCATIONCHANGE) {
        // 단순 flag set. 무거운 작업은 video_tick 에서.
        self->m_needRescan.store(true, std::memory_order_release);
    }
}

#endif // _WIN32
