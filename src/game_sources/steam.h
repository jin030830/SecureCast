// =============================================================================
// game_sources/steam.h — Game Mode v2 / T06: Steam 게임 자동 enum
//
// 역할:
//   설치된 Steam 라이브러리를 스캔해 게임 exe 후보를 모은다. T08 "자동 검색"
//   dialog가 이 리스트를 사용자에게 보여주고, 사용자가 체크한 항목이 Tier 3
//   (sc_set_user_game_list)로 등록된다.
//
// 호출 시점:
//   - 사용자가 OBS 설정 dialog에서 "자동 검색" 버튼을 누를 때 (UI 스레드)
//   - 결과는 한 번의 enum 호출로 완전한 목록 — 비동기/스트리밍 아님
//
// 비용:
//   - 라이브러리 + appmanifest 파싱: 보통 수십~수백 ms
//   - 각 게임 install 디렉토리의 재귀 .exe 스캔: 게임당 수십 ms (디렉토리
//     크기에 비례). 라이브러리 전체에서 수 초 가능.
//   ⇒ UI 스레드 호출 시 사용자에게 "검색 중..." 인디케이터 노출 권장.
//
// 안전성:
//   Steam 미설치, 잘린 VDF, 접근 불가 디렉토리 등 모든 실패는 빈 벡터로
//   집계되며 예외는 던지지 않는다. 호출 측 try/catch 불필요.
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace securecast {

struct GameInfo {
  // Foreground 매칭에 쓰일 정규화된 exe basename (소문자 보존하지만 비교는
  // case-insensitive). 예: "lostark.exe", "valorant-win64-shipping.exe".
  std::wstring exe_basename;

  // 사용자에게 보여줄 게임 이름. appmanifest_*.acf의 "AppState" → "name".
  // UTF-8 한글 포함 가능. 예: "Lost Ark", "리니지W"는 Steam에 없지만 같은
  // 패턴으로 다른 source가 채울 수 있다.
  std::wstring display_name;

  // 실제 발견된 .exe 절대 경로. 디버그/검증 + 향후 우선순위 매김에 사용.
  std::wstring exe_full_path;

  // 어디서 발견됐는지. 항상 "Steam" — 다른 source(Epic/Battle.net 등)가
  // 같은 GameInfo 구조를 채울 때 자기 source 명을 넣는다.
  std::wstring source;
};

// 설치된 Steam 라이브러리들을 스캔해 게임 후보 리스트 반환.
// Steam 미설치 또는 모든 실패 케이스에서 빈 벡터 반환 (예외 없음).
// 동기 호출 — 수초까지 블록 가능 (T08 caller가 별도 스레드에서 호출 권장).
std::vector<GameInfo> enum_steam_games();

} // namespace securecast
