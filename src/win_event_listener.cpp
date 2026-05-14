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
    m_hookGroup1 = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_DESTROY,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 그룹 2: 포그라운드 변경 — 카톡이 앞으로 오거나 다른 앱이 앞으로 와서 카톡이
    // 뒤로 가는 순간을 감지. EVENT_OBJECT_LOCATIONCHANGE(위치 이동)와 달리 포그라운드
    // 전환은 드물게 발생하므로 CPU 부담 없이 추가 가능.
    m_hookGroup2 = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 그룹 3: 최소화 시작/완료. IsIconic()은 Aero Peek 중에 FALSE를 반환하는 경우가
    // 있으므로 WinEvent 세트를 별도로 유지한다.
    m_hookGroup3 = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, eventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_hookGroup1) {
        blog(LOG_WARNING, "[SecureCast] hookGroup1 (SHOW/HIDE/DESTROY) failed — range issue, non-fatal.");
    }
    if (!m_hookGroup2) {
        blog(LOG_ERROR, "[SecureCast] hookGroup2 (FOREGROUND) failed.");
    }
    if (!m_hookGroup3) {
        blog(LOG_ERROR, "[SecureCast] hookGroup3 (MINIMIZESTART/END) failed — minimize detection will not work!");
    } else {
        blog(LOG_INFO, "[SecureCast] WinEventListener started (FOREGROUND + MINIMIZESTART/END hooks active).");
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
    if (m_hookGroup3) UnhookWinEvent(m_hookGroup3);
    m_hookGroup1 = nullptr;
    m_hookGroup2 = nullptr;
    m_hookGroup3 = nullptr;
    m_threadId   = 0;

    blog(LOG_INFO, "[SecureCast] WinEventListener stopped.");
}

bool WinEventListener::isMinimized(HWND hwnd)
{
    WinEventListener* self = s_active.load(std::memory_order_acquire);
    if (!self)
        return IsIconic(hwnd) != 0;
    std::lock_guard<std::mutex> lk(self->m_minimizedMutex);
    for (int i = 0; i < self->m_minimizedCount; ++i) {
        if (self->m_minimizedSet[i] == hwnd)
            return true;
    }
    return false;
}

void CALLBACK WinEventListener::eventProc(HWINEVENTHOOK /*hHook*/, DWORD event,
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

    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        blog(LOG_INFO, "[SecureCast] MINIMIZESTART hwnd=%p IsIconic=%d", hwnd, IsIconic(hwnd));
        {
            std::lock_guard<std::mutex> lk(self->m_minimizedMutex);
            bool found = false;
            for (int i = 0; i < self->m_minimizedCount; ++i) {
                if (self->m_minimizedSet[i] == hwnd) { found = true; break; }
            }
            if (!found && self->m_minimizedCount < 16)
                self->m_minimizedSet[self->m_minimizedCount++] = hwnd;
        }
        // video_tick 이 popMinimizeStart()로 consume할 때까지 보관.
        self->m_minimizeStartHwnd.store(hwnd, std::memory_order_release);
    } else if (event == EVENT_SYSTEM_MINIMIZEEND) {
        blog(LOG_INFO, "[SecureCast] MINIMIZEEND hwnd=%p IsIconic=%d", hwnd, IsIconic(hwnd));
        // IsIconic() still TRUE → window is still minimized (Aero Peek spurious fire).
        // Do NOT remove from set — only remove on genuine restore.
        if (IsIconic(hwnd)) {
            self->m_needRescan.store(true, std::memory_order_release);
        } else {
            {
                std::lock_guard<std::mutex> lk(self->m_minimizedMutex);
                for (int i = 0; i < self->m_minimizedCount; ++i) {
                    if (self->m_minimizedSet[i] == hwnd) {
                        self->m_minimizedSet[i] =
                            self->m_minimizedSet[--self->m_minimizedCount];
                        break;
                    }
                }
            }
            self->m_needRescan.store(true, std::memory_order_release);
        }
    } else {
        self->m_needRescan.store(true, std::memory_order_release);
    }
}

#endif // _WIN32
