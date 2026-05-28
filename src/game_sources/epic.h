// =============================================================================
// game_sources/epic.h — Game Mode v2 / T09a: Epic Games Launcher enum
//
// %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item 파일들을 파싱.
// 각 .item은 JSON: DisplayName + InstallLocation + LaunchExecutable.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo

#include <vector>

namespace securecast {

std::vector<GameInfo> enum_epic_games();

} // namespace securecast
