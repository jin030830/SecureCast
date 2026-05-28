// =============================================================================
// game_sources/uwp.h — Game Mode v2 / T09d: MS Store / UWP 게임 enum
//
// WinRT PackageManager.FindPackagesForUser로 현재 사용자에 등록된 UWP 패키지를
// 모두 enum. emit_games_for_installdir가 .exe 크기/exclude 필터로 자연스럽게
// 비-게임을 거른다 (앱 대부분은 ≥10MB .exe 없음).
//
// 알려진 한계: UWP는 ApplicationFrameHost.exe 호스팅으로 실행돼 fg.exe 기준
// 매칭이 어려움. 이 enum은 InstalledLocation 안의 실제 .exe basename을
// 등록하지만, 실행 시 fg가 ApplicationFrameHost로 잡히는 게임은 매칭 안 됨.
// 그런 케이스는 사용자가 수동 추가 — v2 plan의 "남은 한계"로 명시됨.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo
#include <vector>

namespace securecast {

std::vector<GameInfo> enum_uwp_games();

} // namespace securecast
