#pragma once

#include <cstdint>

// ============================================================
// 글로벌 스크롤 모션 신호 — paint 이전 인과 신호
//
// WH_MOUSE_LL + WH_KEYBOARD_LL low-level hook으로 마우스 휠/PageUp/Down/Home/End
// 입력을 OS 단에서 가로채 시각을 기록한다. EMA 사후 신호(NCC velocity)와 달리
// 모션이 일어나기 전에 활성되어 visual-tracker가 박스를 미리 확장할 수 있다.
//
// hook은 전용 message-pump thread에서 설치. start_*는 idempotent, 첫 호출 시만
// thread 시작. process exit 시 자동 정리 (detach + OS가 hook unhook).
// ============================================================

namespace securecast {

// 첫 호출 시 hook thread 시작 + 휠/키 hook 설치. 두 번째 호출부터 no-op.
// 호출 thread는 UI/main thread일 필요 없음 (전용 thread 별도 생성).
void start_scroll_motion_hooks();

// 마지막 스크롤 모션 입력 시각(ms, steady_clock 기준). 0이면 입력 없음.
// 호출 비용 atomic load 1회 (< 1ns).
int64_t last_scroll_motion_time_ms();

} // namespace securecast
