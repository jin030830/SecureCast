// =============================================================================
// game_sources/gamebar.cpp — Xbox Game Bar 학습 게임 enum 구현
//
// 키 이름이 exe path를 변형한 형태 — Windows 버전마다 다른데, 가장 흔한 형태는:
//   "C:\\Games\\foo\\bar.exe"   (백슬래시 이스케이프, 따옴표 없음)
//   또는 그냥 "C:\Games\foo\bar.exe"
//
// 이름이 .exe 형태가 아닌 키들도 있을 수 있어 (예: PFN), .exe로 끝나는 경우만
// 처리. PFN 같은 경우는 UWP enum이 잡아준다.
// =============================================================================

#include "gamebar.h"
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

void walk_gamebar_subkey(const wchar_t *subkey, std::vector<GameInfo> &out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key) !=
      ERROR_SUCCESS)
    return;

  wchar_t name[1024] = {};
  DWORD idx = 0;
  while (true) {
    DWORD nameLen = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
    LSTATUS s = RegEnumKeyExW(key, idx++, name, &nameLen, nullptr, nullptr,
                              nullptr, nullptr);
    if (s == ERROR_NO_MORE_ITEMS)
      break;
    if (s != ERROR_SUCCESS)
      continue;

    std::wstring raw(name);
    // .exe로 끝나는 게 우리가 처리 가능한 케이스. 아니면 (PFN 등) skip.
    if (!ends_with_icase(raw, L".exe"))
      continue;
    // 이중 backslash → single backslash 정규화. (일부 키는 "C:\\..." 형식)
    std::wstring normalized;
    normalized.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      normalized += raw[i];
      if (raw[i] == L'\\' && i + 1 < raw.size() && raw[i + 1] == L'\\')
        ++i; // skip duplicate
    }
    normalized = normalize_path(std::move(normalized));

    std::wstring bn = basename_of(normalized);
    if (bn.empty() || is_excluded_exe_name(bn))
      continue;

    // 파일 자체가 사라졌어도(설치 위치 변경/제거) 사용자 학습 데이터로서는
    // 유효한 추천 — 등록. file_exists 강제하지 않음. 하지만 너무 작은 .exe면
    // 가비지일 가능성 — 그 검증은 file_exists 후 size로 옵션적으로 가능.
    // 우선은 단순 등록 (사용자가 dialog에서 검토).
    GameInfo gi;
    gi.exe_basename = bn;
    gi.display_name = bn; // 별도 display 정보 없음 → basename 사용
    gi.exe_full_path = normalized;
    gi.source = L"Game Bar";
    out.push_back(std::move(gi));
  }
  RegCloseKey(key);
}

} // namespace

std::vector<GameInfo> enum_gamebar_games() {
  std::vector<GameInfo> out;
  try {
    walk_gamebar_subkey(L"Software\\Microsoft\\GameBar\\Games", out);
    walk_gamebar_subkey(L"Software\\Microsoft\\GameBar\\PreviewedGames", out);

    // Game Bar 자체 dedup — 같은 exe가 Games와 PreviewedGames 양쪽에 있을 수
    // 있음. autodetect_worker의 전역 dedup에서 한 번 더 거르지만, 한 source
    // 안에서도 정리하는 게 깔끔.
    std::vector<GameInfo> deduped;
    deduped.reserve(out.size());
    for (auto &g : out) {
      bool dup = false;
      for (const auto &e : deduped) {
        if (wstr_iequals(e.exe_basename.c_str(), g.exe_basename.c_str())) {
          dup = true;
          break;
        }
      }
      if (!dup)
        deduped.push_back(std::move(g));
    }

    blog(LOG_INFO, "[gamebar_enum] %zu game candidate(s) after dedup",
         deduped.size());
    return deduped;
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[gamebar_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[gamebar_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
