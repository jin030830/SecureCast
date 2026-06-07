// =============================================================================
// scroll_motion_hook.cpp — 전역 스크롤/페이지 이동 입력 감지 (구현부)
//
// 무엇을 하나:
//   OS 저수준 훅(WH_MOUSE_LL / WH_KEYBOARD_LL)으로 마우스 휠과
//   PageUp/PageDown/Home/End 키 입력을 시스템 전역에서 가로채, 그 "마지막
//   시각"만 기록한다(입력 내용은 저장하지 않음).
//
// 왜 필요한가:
//   화면이 실제로 스크롤되어 픽셀이 바뀌기 "전에" 사용자의 입력이 먼저
//   일어난다. 이 인과 신호를 미리 잡아 두면 visual-tracker가 박스를 선제적으로
//   확장해, 스크롤이 시작되는 첫 프레임의 순간 노출을 막을 수 있다(픽셀 변화
//   뒤를 따라가는 NCC 속도 추정보다 빠름).
//
// 동작 방식:
//   훅 콜백이 처리되려면 설치한 스레드에 메시지 펌프가 있어야 하므로, 전용
//   스레드(hook_thread_main)를 하나 만들어 그 안에서 훅을 설치하고 GetMessage
//   루프를 돈다. start_scroll_motion_hooks()는 idempotent — 첫 호출에만 스레드를
//   띄우고, 그 스레드는 detach 되어 OBS 종료 시 OS가 훅을 자동 해제한다.
//   콜백은 최대한 가볍게(시각 기록 1회) — 무거우면 OS가 훅을 강제 해제한다.
// =============================================================================
#include "scroll_motion_hook.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::atomic<int64_t> g_lastMotionMs{0};
std::atomic<bool> g_hookStarted{false};
HHOOK g_mouseHook = nullptr;
HHOOK g_kbdHook = nullptr;

void record_motion() {
  using namespace std::chrono;
  const int64_t ms =
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count();
  g_lastMotionMs.store(ms, std::memory_order_release);
}

LRESULT CALLBACK mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
  // LowLevelHooksTimeout(300ms) 초과 시 OS가 hook 자동 해제. 콜백은 매우 빠르게.
  if (nCode >= 0) {
    if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)
      record_motion();
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK kbd_proc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
    const auto *kb = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
    const DWORD vk = kb->vkCode;
    // PageUp/Down/Home/End만 — 화살표/Space는 텍스트 입력 노이즈 회피.
    if (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END)
      record_motion();
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void hook_thread_main() {
  // low-level hook은 호출 thread에 message pump이 있어야 콜백이 처리된다.
  // hMod=nullptr 로 등록 시 같은 process 내 dll 주입 없이 호출 process에서만
  // 실행됨. SecureCast process(OBS)가 활성 thread message pump 보유.
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, nullptr, 0);
  g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, nullptr, 0);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  // GetMessage가 WM_QUIT 받거나 오류 시 종료. process exit 경로에서는 OS가
  // hook 자동 해제하므로 명시 unhook은 best-effort.
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
  }
  if (g_kbdHook) {
    UnhookWindowsHookEx(g_kbdHook);
    g_kbdHook = nullptr;
  }
}

} // namespace

namespace securecast {

void start_scroll_motion_hooks() {
  bool expected = false;
  if (!g_hookStarted.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel))
    return; // 이미 시작됨

  // detach: process exit 시 자동 정리. 명시 stop API 없음 — 플러그인 lifetime
  // 동안만 살면 충분하고 OBS 종료 시 OS가 hook 자동 해제.
  std::thread(hook_thread_main).detach();
}

int64_t last_scroll_motion_time_ms() {
  return g_lastMotionMs.load(std::memory_order_acquire);
}

} // namespace securecast
