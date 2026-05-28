// =============================================================================
// game_sources/epic.cpp — Epic Games Launcher enum 구현
//
// .item 파일은 정식 JSON이지만 우리가 필요한 건 3개 string 필드뿐.
// 정식 JSON 파서를 끌어들이지 않고, "key" : "value" 패턴을 직접 추출한다.
// (값에 escape된 따옴표가 들어갈 가능성은 거의 없음 — 모두 단순 경로/이름)
//
// Epic 미설치/Manifests 디렉토리 없음 → 빈 벡터 반환 (예외 없음).
// =============================================================================

#include "epic.h"
#include "common.h"
#include "../plugin-support.h" // obs_log

#include <obs.h>

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace securecast {

namespace {

using namespace securecast::game_src_common;

// JSON에서 "key" 다음의 첫 quoted string 값을 추출.
// 매우 단순 — escape sequence 미지원. Epic .item은 경로 외 특수 문자 없어 OK.
// 반환: 찾으면 UTF-8 값, 못 찾으면 빈 string.
std::string extract_json_string(const std::string &src, const char *key) {
  std::string pattern = std::string("\"") + key + "\"";
  size_t pos = src.find(pattern);
  if (pos == std::string::npos)
    return {};
  pos += pattern.size();

  // ':' 까지 whitespace 스킵
  while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
                              src[pos] == '\r' || src[pos] == '\n'))
    ++pos;
  if (pos >= src.size() || src[pos] != ':')
    return {};
  ++pos;
  // value 앞 whitespace
  while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
                              src[pos] == '\r' || src[pos] == '\n'))
    ++pos;
  if (pos >= src.size() || src[pos] != '"')
    return {};
  ++pos;
  size_t start = pos;
  while (pos < src.size() && src[pos] != '"') {
    if (src[pos] == '\\' && pos + 1 < src.size())
      pos += 2;
    else
      ++pos;
  }
  if (pos > src.size())
    return {};
  // 추출한 raw substring에서 backslash 이스케이프(\\, \", \/)만 풀어준다.
  std::string raw = src.substr(start, pos - start);
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      char e = raw[i + 1];
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out += e;
        break;
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      default:
        out += e;
        break;
      }
      ++i;
      continue;
    }
    out += raw[i];
  }
  return out;
}

std::wstring utf8_to_wstring(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

std::string read_file_text(const std::wstring &path) {
  FILE *f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    return {};
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return {};
  }
  long size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return {};
  }
  std::string buf;
  if (size > 0) {
    buf.resize(static_cast<size_t>(size));
    size_t r = fread(buf.data(), 1, buf.size(), f);
    buf.resize(r);
  }
  fclose(f);
  return buf;
}

// %ProgramData% → C:\ProgramData. CSIDL_COMMON_APPDATA fallback.
std::wstring program_data_path() {
  // 1차: env var.
  std::wstring p = read_env_var(L"ProgramData");
  if (!p.empty())
    return normalize_path(std::move(p));

  // 2차: SHGetFolderPathW.
  wchar_t buf[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                  SHGFP_TYPE_CURRENT, buf))) {
    return normalize_path(std::wstring(buf));
  }
  return {};
}

} // namespace

std::vector<GameInfo> enum_epic_games() {
  std::vector<GameInfo> out;
  try {
    std::wstring pd = program_data_path();
    if (pd.empty()) {
      blog(LOG_INFO, "[epic_enum] %%ProgramData%% not resolvable");
      return out;
    }

    std::wstring manifests = path_join(
        pd, L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (!directory_exists(manifests)) {
      blog(LOG_INFO, "[epic_enum] Epic launcher not installed (no Manifests)");
      return out;
    }

    std::wstring pattern = path_join(manifests, L"*.item");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      blog(LOG_INFO, "[epic_enum] no .item manifests found");
      return out;
    }

    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      std::wstring item_path = path_join(manifests, fd.cFileName);
      std::string text = read_file_text(item_path);
      if (text.empty())
        continue;

      std::wstring display = utf8_to_wstring(
          extract_json_string(text, "DisplayName"));
      std::wstring install = utf8_to_wstring(
          extract_json_string(text, "InstallLocation"));
      std::wstring launch_exe = utf8_to_wstring(
          extract_json_string(text, "LaunchExecutable"));

      if (install.empty())
        continue;
      install = normalize_path(std::move(install));

      // 1차: LaunchExecutable이 있으면 단일 GameInfo로 emit.
      if (!launch_exe.empty()) {
        std::wstring full_exe = path_join(install, launch_exe);
        full_exe = normalize_path(std::move(full_exe));
        std::wstring bn = basename_of(launch_exe);
        if (!bn.empty() &&
            !is_excluded_exe_name(bn) &&
            ends_with_icase(bn, L".exe") && file_exists(full_exe)) {
          GameInfo gi;
          gi.exe_basename = std::move(bn);
          gi.display_name = display.empty() ? basename_of(install) : display;
          gi.exe_full_path = std::move(full_exe);
          gi.source = L"Epic Games";
          out.push_back(std::move(gi));
          continue;
        }
      }

      // 2차 fallback: install dir 재귀 .exe 스캔.
      const std::wstring disp =
          display.empty() ? basename_of(install) : display;
      emit_games_for_installdir(disp, install, L"Epic Games", out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    blog(LOG_INFO, "[epic_enum] %zu game candidate(s)", out.size());
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[epic_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[epic_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
