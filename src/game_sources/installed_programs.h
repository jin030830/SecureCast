// =============================================================================
// game_sources/installed_programs.h — Game Mode v2 / T09f: Installed Programs
// 휴리스틱 enum.
//
// HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\* (+ WOW6432Node)
// 모두 walk → DisplayName/Publisher가 게임 패턴이고 InstallLocation에 .exe가
// 있으면 후보.
//
// 휴리스틱이라 false positive가 있을 수 있음 — autodetect dialog의 source
// 라벨에 "Installed Programs"로 표시되어 사용자가 인지하고 추가 결정.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo
#include <vector>

namespace securecast {

std::vector<GameInfo> enum_installed_programs_games();

} // namespace securecast
