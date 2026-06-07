// =============================================================================
// game_sources/gamebar.h — Game Mode v2 / T09e: Xbox Game Bar 학습 데이터
//
// Windows의 Game Bar는 사용자가 "이건 게임" / "이건 게임 아님" 분류한 앱들을
// HKCU\Software\Microsoft\GameBar\Games (등록됨) +
// HKCU\Software\Microsoft\GameBar\PreviewedGames (제안됨) 아래에 저장한다.
//
// 이 키들의 자식 이름은 보통 exe의 절대 경로. value들은 게임 모드 토글 등.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo
#include <vector>

namespace securecast {

std::vector<GameInfo> enum_gamebar_games();

} // namespace securecast
