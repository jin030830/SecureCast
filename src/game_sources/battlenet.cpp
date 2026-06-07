// =============================================================================
// game_sources/battlenet.cpp — Battle.net 게임 enum 구현
//
// 레지스트리 구조 (예):
//   HKLM\SOFTWARE\WOW6432Node\Blizzard Entertainment\
//     ├ Battle.net           (런처 자체 — skip)
//     ├ World of Warcraft    InstallPath=...\World of Warcraft
//     ├ Diablo IV            InstallPath=...\Diablo IV
//     ├ Overwatch            InstallPath=...\Overwatch
//     ├ Hearthstone          InstallPath=...\Hearthstone
//     └ ...
//
// 게임의 InstallPath 안에 재귀 .exe 스캔.
// =============================================================================

#include "battlenet.h"
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

// "Battle.net" 자체는 런처라 게임 아님. 다른 비-게임 키도 차단할 여지 있어
// 명시 차단.
bool is_excluded_blizzard_subkey(const wchar_t *name) {
  static const wchar_t *const kExcl[] = {
      L"Battle.net",
      L"Agent",
      L"Battle.net Launcher",
  };
  for (auto e : kExcl) {
    if (wstr_iequals(name, e))
      return true;
  }
  return false;
}

void enum_under(HKEY root, const wchar_t *parent, REGSAM extra,
                std::vector<GameInfo> &out) {
  HKEY parentKey = nullptr;
  if (RegOpenKeyExW(root, parent, 0, KEY_READ | extra, &parentKey) !=
      ERROR_SUCCESS)
    return;

  wchar_t subKeyName[256] = {};
  DWORD idx = 0;
  while (true) {
    DWORD nameLen = static_cast<DWORD>(sizeof(subKeyName) /
                                       sizeof(subKeyName[0]));
    LSTATUS s = RegEnumKeyExW(parentKey, idx++, subKeyName, &nameLen, nullptr,
                              nullptr, nullptr, nullptr);
    if (s == ERROR_NO_MORE_ITEMS)
      break;
    if (s != ERROR_SUCCESS)
      continue;
    if (is_excluded_blizzard_subkey(subKeyName))
      continue;

    std::wstring fullSub = std::wstring(parent) + L"\\" + subKeyName;
    std::wstring installPath =
        read_registry_string(root, fullSub.c_str(), L"InstallPath", extra);
    if (installPath.empty())
      installPath = read_registry_string(root, fullSub.c_str(),
                                          L"InstallLocation", extra);
    if (installPath.empty())
      continue;
    installPath = normalize_path(std::move(installPath));
    if (!directory_exists(installPath))
      continue;

    emit_games_for_installdir(subKeyName, installPath, L"Battle.net", out);
  }
  RegCloseKey(parentKey);
}

} // namespace

std::vector<GameInfo> enum_battlenet_games() {
  std::vector<GameInfo> out;
  try {
    // 64-bit 머신에서 Blizzard는 32-bit 키에 등록.
    enum_under(HKEY_LOCAL_MACHINE,
               L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment",
               0, out);
    // Fallback: 32-bit OS / 일부 환경.
    enum_under(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Blizzard Entertainment",
               KEY_WOW64_64KEY, out);

    blog(LOG_INFO, "[battlenet_enum] %zu game candidate(s)", out.size());
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[battlenet_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[battlenet_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
