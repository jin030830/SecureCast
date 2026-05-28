// =============================================================================
// game_sources/common.cpp — 공용 헬퍼 구현
// =============================================================================

#include "common.h"

#include "../plugin-support.h" // obs_log

#include <obs.h>

#include <cwctype>

namespace securecast::game_src_common {

// ────────────────────────── 문자열 ──────────────────────────

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

bool contains_icase(const std::wstring &s, const wchar_t *needle) {
  size_t nn = wcslen(needle);
  if (nn == 0)
    return true;
  if (s.size() < nn)
    return false;
  for (size_t i = 0; i + nn <= s.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < nn; ++j) {
      if (towlower(s[i + j]) != towlower(needle[j])) {
        match = false;
        break;
      }
    }
    if (match)
      return true;
  }
  return false;
}

// ────────────────────────── 경로 ──────────────────────────

std::wstring normalize_path(std::wstring p) {
  for (auto &c : p) {
    if (c == L'/')
      c = L'\\';
  }
  while (!p.empty() && p.back() == L'\\')
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

std::wstring basename_of(const std::wstring &full) {
  size_t pos = full.find_last_of(L"\\/");
  if (pos == std::wstring::npos)
    return full;
  return full.substr(pos + 1);
}

bool directory_exists(const std::wstring &path) {
  if (path.empty())
    return false;
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool file_exists(const std::wstring &path) {
  if (path.empty())
    return false;
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring read_env_var(const wchar_t *name) {
  if (!name)
    return {};
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0)
    return {};
  std::wstring out(n, L'\0');
  DWORD got = GetEnvironmentVariableW(name, out.data(), n);
  if (got == 0 || got >= n) {
    out.clear();
    return out;
  }
  out.resize(got);
  return out;
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
  size_t maxChars = sizeof(buf) / sizeof(buf[0]);
  for (size_t i = 0; i < maxChars; ++i) {
    if (buf[i] == 0)
      return std::wstring(buf, i);
  }
  buf[maxChars - 1] = 0;
  return std::wstring(buf);
}

// ────────────────────────── exclude 휴리스틱 ──────────────────────────

bool is_excluded_exe_name(const std::wstring &basename) {
  static const wchar_t *const kPrefixes[] = {
      L"unins",         L"vc_redist",   L"vcredist",     L"vcruntime",
      L"setup",         L"crash",       L"report",       L"updater",
      L"update",        L"redist",      L"dxsetup",      L"dotnetfx",
      L"installer",     L"uninstall",   L"easyanticheat",L"battleye",
      L"steamerror",    L"steamservice",L"wpfgfx",       L"presentation",
      L"d3dcompiler",   L"helper",      L"wer",          L"epicwebhelper",
      L"epiconlineservices",            L"riotclientservices",
      L"riotclientcrash",
  };
  for (auto p : kPrefixes) {
    if (starts_with_icase(basename, p))
      return true;
  }
  return false;
}

bool is_excluded_subdir(const wchar_t *dirname) {
  static const wchar_t *const kDirs[] = {
      L"_CommonRedist",  L"_Redist",      L"Redistributables", L"DirectX",
      L"dotnet",         L"DotNet",       L"vc_redist",        L"VCRedist",
      L"redist",         L"installers",   L"_Installer",       L"EasyAntiCheat",
      L"BattlEye",       L"DLC",          L"Engine",           L"ThirdParty",
  };
  for (auto d : kDirs) {
    if (wstr_iequals(dirname, d))
      return true;
  }
  return false;
}

// ────────────────────────── .exe 스캔 ──────────────────────────

void scan_dir_for_exes(const std::wstring &dir, int depth,
                       std::vector<std::wstring> &out) {
  if (depth > kMaxScanDepth || !directory_exists(dir))
    return;

  std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == 0 ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
      continue;

    std::wstring child = path_join(dir, fd.cFileName);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        continue;
      if (is_excluded_subdir(fd.cFileName))
        continue;
      scan_dir_for_exes(child, depth + 1, out);
    } else {
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

void emit_games_for_installdir(const std::wstring &display_name,
                               const std::wstring &install_dir,
                               const std::wstring &source,
                               std::vector<GameInfo> &out) {
  if (install_dir.empty() || !directory_exists(install_dir))
    return;
  std::vector<std::wstring> exes;
  scan_dir_for_exes(install_dir, 0, exes);
  if (exes.empty())
    return;

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
    gi.source = source;
    out.push_back(std::move(gi));
  }
}

} // namespace securecast::game_src_common
