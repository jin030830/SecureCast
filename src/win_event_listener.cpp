// =============================================================================
// win_event_listener.cpp — SetWinEventHook listener 구현
// =============================================================================

#ifdef _WIN32

#include "win_event_listener.h"
#include "plugin-support.h"
#include <obs.h>            // blog, LOG_INFO/ERROR

std::atomic<WinEventListener*> WinEventListener::s_active{nullptr};

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

    // 그룹 1: 등장/사라짐/소멸 (드물게 발생, 모든 윈도우 대상)
    // EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, EVENT_OBJECT_DESTROY 가 이 범위에 들어감.
    m_hookGroup1 = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_DESTROY,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // [의도적으로 LOCATIONCHANGE 후킹하지 않음]
    // EVENT_OBJECT_LOCATIONCHANGE 는 마우스 커서 / UI 갱신 / 모든 윈도우 이동에 발생해
    // 1초당 수천 번 콜백. 모든 콜백이 needRescan flag 를 set 하면 매 video_tick 마다
    // 풀 스캔 (EnumWindows + OpenProcess) 이 돌아 CPU 가 폭주한다 (관측: 일반 25%,
    // 게임 중 70%). 위치 변경은 매 tick (60Hz) 의 sc_get_window_visible_bounds 폴링
    // 으로 처리. 게임 중 빠른 이동 노출은 4주차 게임 모드 (적응형 마진 등) 로 별도 처리.
    m_hookGroup2 = nullptr;

    if (!m_hookGroup1) {
        blog(LOG_ERROR, "[SecureCast] SetWinEventHook failed; falling back to polling only.");
    } else {
        blog(LOG_INFO, "[SecureCast] WinEventListener started (SHOW/HIDE/DESTROY only).");
    }

    // 메시지 펌프 — WINEVENT_OUTOFCONTEXT 콜백은 이 펌프에서 dispatch 됨.
    MSG msg{};
    while (m_running.load(std::memory_order_acquire)) {
        BOOL ret = GetMessage(&msg, nullptr, 0, 0);
        if (ret <= 0) // WM_QUIT 또는 에러
            break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (m_hookGroup1) UnhookWinEvent(m_hookGroup1);
    if (m_hookGroup2) UnhookWinEvent(m_hookGroup2);
    m_hookGroup1 = nullptr;
    m_hookGroup2 = nullptr;
    m_threadId   = 0;

    blog(LOG_INFO, "[SecureCast] WinEventListener stopped.");
}

void CALLBACK WinEventListener::eventProc(HWINEVENTHOOK /*hHook*/, DWORD /*event*/,
                                          HWND hwnd, LONG idObject, LONG idChild,
                                          DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    // OBJID_WINDOW + CHILDID_SELF 만 처리 (탑레벨 윈도우 자체 이벤트).
    // 그 외 (메뉴, 컨트롤, 캐럿 등) 는 노이즈.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr)
        return;

    WinEventListener* self = s_active.load(std::memory_order_acquire);
    if (!self)
        return;

    // 단순 flag set. 무거운 작업은 video_tick 에서.
    self->m_needRescan.store(true, std::memory_order_release);
}

#endif // _WIN32
