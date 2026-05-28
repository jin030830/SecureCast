// =============================================================================
// game_sources/installed_programs.cpp — Uninstall registry walk + 휴리스틱 매칭
//
// 매칭 기준 (OR):
//   (a) DisplayName 또는 Publisher가 알려진 게임 publisher 패턴
//   (b) DisplayName이 "game" 키워드 포함 AND anti-pattern("driver", "runtime"
//        등) 미포함
//
// 매칭 시 InstallLocation에서 .exe 재귀 검색 → emit.
// InstallLocation이 비어있으면 skip (Steam/Epic enum이 더 정확히 잡음).
// =============================================================================

#include "installed_programs.h"
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

// 게임 publisher (사용자가 한국 환경에서 자주 마주칠 회사 위주).
bool publisher_is_game_company(const std::wstring &pub) {
  static const wchar_t *const kPubs[] = {
      L"Blizzard",     L"Bethesda",     L"Square Enix",  L"Capcom",
      L"Activision",   L"Electronic Arts",               L"EA Games",
      L"Konami",       L"SEGA",         L"Nexon",        L"NCSOFT",
      L"NCsoft",       L"NC SOFT",      L"Smilegate",    L"Krafton",
      L"Bluehole",     L"Pearl Abyss",  L"Ubisoft",      L"2K",
      L"Rockstar",     L"From Software",                 L"FromSoftware",
      L"HoYoverse",    L"miHoYo",       L"COGNOSPHERE",  L"Kakao Games",
      L"Wemade",       L"Nimble Neuron",                 L"Devolver",
      L"Larian",       L"PUBG Studios",                  L"Riot Games",
      L"Valve",        L"Epic Games",   L"Frontier",     L"Paradox",
      L"NEOPLE",       L"Microsoft Studios",
      L"Xbox Game Studios",
  };
  for (auto p : kPubs) {
    if (contains_icase(pub, p))
      return true;
  }
  return false;
}

// DisplayName에 게임을 시사하는 키워드.
bool name_has_game_keyword(const std::wstring &name) {
  static const wchar_t *const kKeywords[] = {
      L"Game", L"게임", L"Online", L"온라인",
      L"MMORPG", L"RPG", L"PUBG", L"FPS",
  };
  for (auto k : kKeywords) {
    if (contains_icase(name, k))
      return true;
  }
  return false;
}

// DisplayName이 가져선 안 될 비-게임 패턴.
bool name_is_non_game(const std::wstring &name) {
  static const wchar_t *const kPatterns[] = {
      L"Driver",    L"Runtime",      L"Redistributable",
      L"Update for",L"Hotfix",       L"Service Pack",
      L"Visual C++",L"Visual Studio",L".NET",
      L"Office",    L"Edge",         L"Library",
      L"SDK",       L"Toolkit",      L"Tools for",
      L"Helper",    L"Add-in",       L"Plugin",
      L"Plug-in",   L"Anti-Cheat",   L"BattlEye",
      L"EasyAntiCheat",
  };
  for (auto p : kPatterns) {
    if (contains_icase(name, p))
      return true;
  }
  return false;
}

void walk_uninstall(HKEY root, const wchar_t *subkey, REGSAM extra,
                    std::vector<GameInfo> &out) {
  HKEY parent = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ | extra, &parent) !=
      ERROR_SUCCESS)
    return;

  wchar_t name[256] = {};
  DWORD idx = 0;
  while (true) {
    DWORD nameLen = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
    LSTATUS s = RegEnumKeyExW(parent, idx++, name, &nameLen, nullptr, nullptr,
                              nullptr, nullptr);
    if (s == ERROR_NO_MORE_ITEMS)
      break;
    if (s != ERROR_SUCCESS)
      continue;

    std::wstring fullSub = std::wstring(subkey) + L"\\" + name;
    std::wstring display =
        read_registry_string(root, fullSub.c_str(), L"DisplayName", extra);
    if (display.empty())
      continue;
    if (name_is_non_game(display))
      continue;

    std::wstring publisher =
        read_registry_string(root, fullSub.c_str(), L"Publisher", extra);

    bool looks_like_game = publisher_is_game_company(publisher) ||
                           name_has_game_keyword(display);
    if (!looks_like_game)
      continue;

    std::wstring install =
        read_registry_string(root, fullSub.c_str(), L"InstallLocation", extra);
    if (install.empty())
      continue;
    install = normalize_path(std::move(install));
    if (!directory_exists(install))
      continue;

    emit_games_for_installdir(display, install,
                              L"Installed Programs", out);
  }
  RegCloseKey(parent);
}

} // namespace

std::vector<GameInfo> enum_installed_programs_games() {
  std::vector<GameInfo> out;
  try {
    // HKLM 64-bit
    walk_uninstall(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                   0, out);
    // HKLM 32-bit
    walk_uninstall(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows"
                   L"\\CurrentVersion\\Uninstall",
                   0, out);
    // HKCU per-user
    walk_uninstall(HKEY_CURRENT_USER,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                   0, out);

    blog(LOG_INFO, "[installed_programs_enum] %zu game candidate(s)",
         out.size());
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[installed_programs_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[installed_programs_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
