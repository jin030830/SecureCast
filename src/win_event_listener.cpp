// =============================================================================
// win_event_listener.cpp — SetWinEventHook listener 구현
// =============================================================================

#ifdef _WIN32

#include "win_event_listener.h"
#include "plugin-support.h"
#include <obs.h> // blog, LOG_INFO/ERROR

std::atomic<WinEventListener *> WinEventListener::s_active{nullptr};

void WinEventListener::start() {
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true))
    return; // 이미 동작 중

  // 한 번에 한 인스턴스만 글로벌 active 로 설정. 나머지는 보조 (이벤트 받지
  // 않음).
  WinEventListener *expectedActive = nullptr;
  s_active.compare_exchange_strong(expectedActive, this);

  m_thread = std::thread([this]() { run(); });
}

void WinEventListener::stop() {
  bool expected = true;
  if (!m_running.compare_exchange_strong(expected, false))
    return;

  // 스레드 깨우기: 메시지 펌프에 WM_QUIT 보냄.
  if (m_threadId)
    PostThreadMessage(m_threadId, WM_QUIT, 0, 0);

  if (m_thread.joinable())
    m_thread.join();

  WinEventListener *expectedActive = this;
  s_active.compare_exchange_strong(expectedActive, nullptr);
}

void WinEventListener::run() {
  m_threadId = GetCurrentThreadId();

  // 그룹 1: 등장/사라짐/소멸 (드물게 발생, 모든 윈도우 대상)
  m_hookGroup1 = SetWinEventHook(
      EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER, nullptr, eventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

  // 그룹 2: 포그라운드 변경 — 카톡이 앞으로 오거나 다른 앱이 앞으로 와서 카톡이
  // 뒤로 가는 순간을 감지. EVENT_OBJECT_LOCATIONCHANGE(위치 이동)와 달리
  // 포그라운드 전환은 드물게 발생하므로 CPU 부담 없이 추가 가능.
  m_hookGroup2 = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, eventProc, 0,
      0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

  // 그룹 3: 최소화 및 복원 (시스템 이벤트 감지)
  m_hookGroup3 = SetWinEventHook(
      EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, eventProc,
      0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

  // 세 그룹 중 하나라도 실패하면 부분 활성 상태를 만들지 않고 전부 해제 →
  // 폴링 폴백. 부분 활성 상태는 어떤 윈도우 이벤트는 받고 어떤 건 못 받는
  // 비대칭 상태가 되어 디버깅을 어렵게 만든다.
  if (!m_hookGroup1 || !m_hookGroup2 || !m_hookGroup3) {
    blog(LOG_ERROR,
         "[SecureCast] SetWinEventHook failed (group1: %p, group2: %p, "
         "group3: %p); falling back to polling only.",
         (void *)m_hookGroup1, (void *)m_hookGroup2, (void *)m_hookGroup3);
    if (m_hookGroup1) {
      UnhookWinEvent(m_hookGroup1);
      m_hookGroup1 = nullptr;
    }
    if (m_hookGroup2) {
      UnhookWinEvent(m_hookGroup2);
      m_hookGroup2 = nullptr;
    }
    if (m_hookGroup3) {
      UnhookWinEvent(m_hookGroup3);
      m_hookGroup3 = nullptr;
    }
  } else {
    blog(LOG_INFO, "[SecureCast] WinEventListener started (CREATE~REORDER + "
                   "FOREGROUND + MINIMIZE).");
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

  if (m_hookGroup1)
    UnhookWinEvent(m_hookGroup1);
  if (m_hookGroup2)
    UnhookWinEvent(m_hookGroup2);
  if (m_hookGroup3)
    UnhookWinEvent(m_hookGroup3);
  m_hookGroup1 = nullptr;
  m_hookGroup2 = nullptr;
  m_hookGroup3 = nullptr;
  m_threadId = 0;

  blog(LOG_INFO, "[SecureCast] WinEventListener stopped.");
}

void CALLBACK WinEventListener::eventProc(HWINEVENTHOOK /*hHook*/,
                                          DWORD /*event*/, HWND hwnd,
                                          LONG idObject, LONG idChild,
                                          DWORD /*idEventThread*/,
                                          DWORD /*dwmsEventTime*/) {
  // OBJID_WINDOW + CHILDID_SELF 만 처리 (탑레벨 윈도우 자체 이벤트).
  // 그 외 (메뉴, 컨트롤, 캐럿 등) 는 노이즈.
  if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr)
    return;

  WinEventListener *self = s_active.load(std::memory_order_acquire);
  if (!self)
    return;

  // 단순 flag set. 무거운 작업은 video_tick 에서.
  self->m_needRescan.store(true, std::memory_order_release);
}

#endif // _WIN32
