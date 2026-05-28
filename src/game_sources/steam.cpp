// =============================================================================
// game_sources/steam.cpp — Steam 게임 자동 enum 구현
//
// 흐름:
//   1. 레지스트리에서 SteamPath 읽기
//      - HKCU\Software\Valve\Steam\SteamPath (Steam 실행 직후 갱신됨)
//      - fallback: HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath
//   2. <SteamPath>/config/libraryfolders.vdf 파싱 → 라이브러리 폴더 목록
//      - "libraryfolders" → "0", "1", ... → "path"
//      - 폴더 없으면 SteamPath 자체만 사용
//   3. 각 라이브러리에서 steamapps/appmanifest_*.acf 글로브 → 파싱
//      - "AppState" → "installdir", "name", "appid"
//      - install 디렉토리: <library>/steamapps/common/<installdir>
//   4. install 디렉토리 재귀 .exe 스캔 (depth 4 cap):
//      - 10MB 미만 / unins*/vc_redist*/setup* 등 제외
//      - 재배포/엔진 redist 서브디렉토리 prune
//
// 디자인 결정:
//   - 한 게임에 .exe가 여러 개면(DX11/DX12 등) GameInfo 여러 개 반환 — 모두
//     같은 display_name 공유. 사용자가 dialog에서 그룹으로 인식.
//   - 게임 install 디렉토리가 사라진 manifest는 skip (다운로드 일시정지 등).
//   - 매니페스트 파싱 실패는 LOG_INFO만 — 한 게임 누락이 전체 실패는 아님.
// =============================================================================

#include "steam.h"
#include "../steam_parser.h"
#include "../plugin-support.h" // obs_log

#include <obs.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <string>
#include <vector>

namespace securecast {

namespace {

// .exe 검색 재귀 깊이. UE5 게임 일부가
//   installdir/<sub>/Binaries/Win64/x.exe (depth 4) 까지 들어가므로 4가 안전.
constexpr int kMaxScanDepth = 4;

// .exe 최소 크기. 10 MB 미만은 거의 redist/uninstaller/crashpad 류.
constexpr uint64_t kMinExeBytes = 10ull * 1024ull * 1024ull;

// ────────────────────────── 문자열/경로 헬퍼 ──────────────────────────

bool wstr_iequals(const wchar_t *a, const wchar_t *b) {
  if (!a || !b)
    return a == b;
  return _wcsicmp(a, b) == 0;
}

bool starts_with_icase(const std::wstring &s, const wchar_t *prefix) {
  size_t n = wcslen(prefix);
  if (s.size() < n)
    return false;
  for (size_t i = 0; i < n; ++i) {
    if (towlower(s[i]) != towlower(prefix[i]))
      return false;
  }
  return true;
}

bool ends_with_icase(const std::wstring &s, const wchar_t *suffix) {
  size_t n = wcslen(suffix);
  if (s.size() < n)
    return false;
  for (size_t i = 0; i < n; ++i) {
    if (towlower(s[s.size() - n + i]) != towlower(suffix[i]))
      return false;
  }
  return true;
}

// Steam VDF는 forward slash를 쓰므로 Windows API 안전성 위해 back slash로 정규화.
std::wstring normalize_path(std::wstring p) {
  for (auto &c : p) {
    if (c == L'/')
      c = L'\\';
  }
  // trailing slash 제거 (FindFirstFileW 호환).
  while (!p.empty() && (p.back() == L'\\'))
    p.pop_back();
  return p;
}

std::wstring path_join(const std::wstring &a, const std::wstring &b) {
  if (a.empty())
    return b;
  if (b.empty())
    return a;
  if (a.back() == L'\\' || a.back() == L'/')
    return a + b;
  return a + L'\\' + b;
}

// "C:\...\foo.exe" → "foo.exe"
std::wstring basename_of(const std::wstring &full) {
  size_t pos = full.find_last_of(L"\\/");
  if (pos == std::wstring::npos)
    return full;
  return full.substr(pos + 1);
}

// ────────────────────────── exclude 휴리스틱 ──────────────────────────

// 게임 본체가 아닐 가능성이 큰 exe 이름 prefix.
bool is_excluded_exe_name(const std::wstring &basename) {
  static const wchar_t *const kPrefixes[] = {
      L"unins",        // uninstall*
      L"vc_redist",    //
      L"vcredist",     //
      L"vcruntime",    //
      L"setup",        //
      L"crash",        // crashpad, crashreporter
      L"report",       //
      L"updater",      //
      L"update",       // update-helper 등 — game 본체와 겹치진 않음
      L"redist",       //
      L"dxsetup",      //
      L"dotnetfx",     //
      L"installer",    //
      L"uninstall",    //
      L"easyanticheat", // EAC launcher
      L"battleye",     // BattlEye launcher (BEService 본체는 service)
      L"steamerror",   //
      L"steamservice", //
      L"wpfgfx",       // .NET WPF
      L"presentation", // .NET WPF
      L"d3dcompiler",  //
  };
  for (auto p : kPrefixes) {
    if (starts_with_icase(basename, p))
      return true;
  }
  return false;
}

// 재귀 스캔 시 prune할 디렉토리 (재배포/엔진 redist).
// 게임 본체 .exe는 보통 여기 안에 없음.
bool is_excluded_subdir(const wchar_t *dirname) {
  static const wchar_t *const kDirs[] = {
      L"_CommonRedist",
      L"_Redist",
      L"Redistributables",
      L"DirectX",
      L"dotnet",
      L"DotNet",
      L"vc_redist",
      L"VCRedist",
      L"redist",
      L"installers",
      L"_Installer",
      L"EasyAntiCheat",
      L"BattlEye",
      L"DLC",
  };
  for (auto d : kDirs) {
    if (wstr_iequals(dirname, d))
      return true;
  }
  return false;
}

// ────────────────────────── 파일/디렉토리 헬퍼 ──────────────────────────

bool directory_exists(const std::wstring &path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void scan_dir_for_exes(const std::wstring &dir, int depth,
                       std::vector<std::wstring> &out) {
  if (depth > kMaxScanDepth)
    return;

  std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  do {
    // "." / ".." 스킵
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == 0 ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
      continue;

    std::wstring child = path_join(dir, fd.cFileName);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      // reparse point(주로 심볼릭 링크)는 잠재 무한루프 — skip.
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        continue;
      if (is_excluded_subdir(fd.cFileName))
        continue;
      scan_dir_for_exes(child, depth + 1, out);
    } else {
      // .exe만
      const std::wstring name = fd.cFileName;
      if (!ends_with_icase(name, L".exe"))
        continue;
      if (is_excluded_exe_name(name))
        continue;
      uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                    static_cast<uint64_t>(fd.nFileSizeLow);
      if (sz < kMinExeBytes)
        continue;
      out.push_back(child);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// ────────────────────────── 레지스트리 ──────────────────────────

std::wstring read_registry_string(HKEY root, const wchar_t *subkey,
                                  const wchar_t *value, REGSAM extra) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ | extra, &key) != ERROR_SUCCESS)
    return {};
  wchar_t buf[MAX_PATH * 2] = {};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  LSTATUS r =
      RegQueryValueExW(key, value, nullptr, &type,
                       reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    return {};
  // null-terminate 안전 (RegQueryValueExW가 보장 안 함).
  size_t maxChars = sizeof(buf) / sizeof(buf[0]);
  for (size_t i = 0; i < maxChars; ++i) {
    if (buf[i] == 0)
      return std::wstring(buf, i);
  }
  buf[maxChars - 1] = 0;
  return std::wstring(buf);
}

std::wstring locate_steam_root() {
  // 1. HKCU\Software\Valve\Steam — Steam 클라이언트가 시작 시 갱신.
  //    가장 신뢰할 수 있는 source. 64-bit/32-bit 구분 없음.
  std::wstring p = read_registry_string(HKEY_CURRENT_USER,
                                        L"Software\\Valve\\Steam",
                                        L"SteamPath", 0);
  if (!p.empty())
    return normalize_path(std::move(p));

  // 2. HKLM 32-bit view — Steam은 32-bit 인스톨러로 깔리므로 여기에 있음.
  p = read_registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
                           L"InstallPath", KEY_WOW64_32KEY);
  if (!p.empty())
    return normalize_path(std::move(p));

  // 3. HKLM 64-bit view (드물지만 일부 환경).
  p = read_registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
                           L"InstallPath", 0);
  if (!p.empty())
    return normalize_path(std::move(p));

  return {};
}

// ────────────────────────── 라이브러리 enum ──────────────────────────

std::vector<std::wstring>
enum_steam_libraries(const std::wstring &steam_root) {
  std::vector<std::wstring> libs;
  // 항상 steam_root 자체를 라이브러리로 포함 — 일부 오래된 환경은
  // libraryfolders.vdf 자체가 없거나 빈 상태.
  libs.push_back(steam_root);

  const std::wstring vdf_path =
      path_join(steam_root, L"config\\libraryfolders.vdf");

  VdfParser parser;
  VdfNode root = parser.parse(vdf_path);
  const VdfNode &folders = root[L"libraryfolders"];
  if (!folders.is_subtree())
    return libs;

  for (const auto &kv : folders) {
    const std::wstring &lib_path = kv.second[L"path"].as_string();
    if (lib_path.empty())
      continue;
    std::wstring norm = normalize_path(lib_path);
    // dedup vs steam_root (대소문자 무시).
    bool dup = false;
    for (const auto &existing : libs) {
      if (wstr_iequals(existing.c_str(), norm.c_str())) {
        dup = true;
        break;
      }
    }
    if (!dup)
      libs.push_back(std::move(norm));
  }
  return libs;
}

// ────────────────────────── 게임 후보 만들기 ──────────────────────────

void emit_games_for_installdir(const std::wstring &display_name,
                               const std::wstring &install_dir,
                               std::vector<GameInfo> &out) {
  if (install_dir.empty() || !directory_exists(install_dir))
    return;

  std::vector<std::wstring> exes;
  scan_dir_for_exes(install_dir, /*depth=*/0, exes);

  if (exes.empty())
    return;

  // 같은 basename이 여러 경로로 중복 발견되면 첫 번째만. exe_basename은
  // game-mode-trigger의 매칭 키이므로 중복은 무의미.
  std::vector<std::wstring> seen_basenames;
  seen_basenames.reserve(exes.size());

  for (const auto &exe_path : exes) {
    std::wstring bn = basename_of(exe_path);
    bool dup = false;
    for (const auto &s : seen_basenames) {
      if (wstr_iequals(s.c_str(), bn.c_str())) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;
    seen_basenames.push_back(bn);

    GameInfo gi;
    gi.exe_basename = std::move(bn);
    gi.display_name = display_name;
    gi.exe_full_path = exe_path;
    gi.source = L"Steam";
    out.push_back(std::move(gi));
  }
}

void enum_apps_in_library(const std::wstring &lib_root,
                          std::vector<GameInfo> &out) {
  const std::wstring steamapps = path_join(lib_root, L"steamapps");
  const std::wstring pattern = path_join(steamapps, L"appmanifest_*.acf");

  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  VdfParser parser;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    const std::wstring manifest_path = path_join(steamapps, fd.cFileName);
    VdfNode root = parser.parse(manifest_path);
    const VdfNode &state = root[L"AppState"];
    if (!state.is_subtree())
      continue;

    const std::wstring &installdir = state[L"installdir"].as_string();
    if (installdir.empty())
      continue;
    const std::wstring &name = state[L"name"].as_string();

    // <library>/steamapps/common/<installdir>
    std::wstring install_full = path_join(steamapps, L"common");
    install_full = path_join(install_full, installdir);
    install_full = normalize_path(install_full);

    // display_name fallback: installdir 사용.
    const std::wstring &display = name.empty() ? installdir : name;
    emit_games_for_installdir(display, install_full, out);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

} // namespace

// ────────────────────────── public API ──────────────────────────

std::vector<GameInfo> enum_steam_games() {
  std::vector<GameInfo> out;
  try {
    std::wstring steam_root = locate_steam_root();
    if (steam_root.empty()) {
      blog(LOG_INFO, "[steam_enum] Steam not installed — empty list");
      return out;
    }
    blog(LOG_INFO, "[steam_enum] Steam root: %ls", steam_root.c_str());

    auto libs = enum_steam_libraries(steam_root);
    blog(LOG_INFO, "[steam_enum] %zu library folder(s)", libs.size());

    for (const auto &lib : libs) {
      const size_t before = out.size();
      enum_apps_in_library(lib, out);
      blog(LOG_INFO, "[steam_enum]   %ls — %zu game(s)", lib.c_str(),
           out.size() - before);
    }
    blog(LOG_INFO, "[steam_enum] total %zu game candidate(s)", out.size());
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[steam_enum] exception: %s — partial results may be lost",
         e.what());
  } catch (...) {
    blog(LOG_WARNING, "[steam_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
