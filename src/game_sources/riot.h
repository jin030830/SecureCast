// =============================================================================
// game_sources/riot.h — Game Mode v2 / T09c: Riot Games (LoL/Valorant/TFT)
//
// Riot은 별도 launcher 등록 정보가 일관적이지 않아 표준 경로 + 일부 env var
// fallback. LoL/Valorant는 한국 사용자 최우선이므로 정확하게 잡힐 필요 있음.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo
#include <vector>

namespace securecast {

std::vector<GameInfo> enum_riot_games();

} // namespace securecast
