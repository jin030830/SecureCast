// =============================================================================
// game_sources/battlenet.h — Game Mode v2 / T09b: Blizzard Battle.net enum
//
// Blizzard 게임은 HKLM\SOFTWARE\WOW6432Node\Blizzard Entertainment\<Game>
// 아래에 InstallPath 등을 등록. 각 자식 키별로 install dir을 찾아 .exe 스캔.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo
#include <vector>

namespace securecast {

std::vector<GameInfo> enum_battlenet_games();

} // namespace securecast
