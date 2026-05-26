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
