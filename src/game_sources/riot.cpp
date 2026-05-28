// =============================================================================
// game_sources/riot.cpp — Riot 게임 enum 구현
//
// Riot은 표준 install 경로를 거의 강제 (변경 불가):
//   <drive>:\Riot Games\League of Legends\
//   <drive>:\Riot Games\VALORANT\
//   <drive>:\Riot Games\Teamfight Tactics\
//
// <drive>은 C: 또는 D: 또는 E:가 흔함. 가능한 후보 모두 시도.
// =============================================================================

#include "riot.h"
#include "common.h"
#include "../plugin-support.h" // obs_log

#include <obs.h>

#include <windows.h>

#include <exception>
#include <string>
#include <vector>

namespace securecast {

namespace {

using namespace securecast::game_src_common;

struct RiotEntry {
  const wchar_t *folder;
  const wchar_t *display;
};

} // namespace

std::vector<GameInfo> enum_riot_games() {
  std::vector<GameInfo> out;
  try {
    static const RiotEntry kEntries[] = {
        {L"League of Legends", L"League of Legends"},
        {L"VALORANT", L"Valorant"},
        {L"Teamfight Tactics", L"Teamfight Tactics"},
    };

    // 모든 드라이브 letter를 후보로 (GetLogicalDrives 비트마스크).
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
      if (!(drives & (1u << (letter - L'A'))))
        continue;
      std::wstring root = std::wstring(1, letter) + L":\\Riot Games";
      if (!directory_exists(root))
        continue;
      for (const auto &e : kEntries) {
        std::wstring install = path_join(root, e.folder);
        if (!directory_exists(install))
          continue;
        emit_games_for_installdir(e.display, install, L"Riot", out);
      }
    }
    blog(LOG_INFO, "[riot_enum] %zu game candidate(s)", out.size());
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[riot_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[riot_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
