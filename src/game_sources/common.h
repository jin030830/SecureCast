// =============================================================================
// game_sources/common.h — Game Mode v2 / T09: source enum 공용 헬퍼
//
// steam.cpp는 자체 헬퍼를 유지 (회귀 최소화). 신규 source(Epic/Battle.net/Riot
// /UWP/Game Bar/Installed Programs)는 이 모듈을 사용.
// =============================================================================

#pragma once

#include "steam.h" // GameInfo

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace securecast::game_src_common {

// .exe 스캔 깊이 — UE5 게임의 sub/Binaries/Win64/x.exe 패턴까지.
inline constexpr int kMaxScanDepth = 4;
// .exe 최소 크기 — redist/uninstaller 거름.
inline constexpr uint64_t kMinExeBytes = 10ull * 1024ull * 1024ull;

// 문자열 ──────────────────────────────────────────────────────────────────
bool wstr_iequals(const wchar_t *a, const wchar_t *b);
bool starts_with_icase(const std::wstring &s, const wchar_t *prefix);
bool ends_with_icase(const std::wstring &s, const wchar_t *suffix);
bool contains_icase(const std::wstring &s, const wchar_t *needle);

// 경로 ───────────────────────────────────────────────────────────────────
std::wstring normalize_path(std::wstring p);
std::wstring path_join(const std::wstring &a, const std::wstring &b);
std::wstring basename_of(const std::wstring &full);
bool directory_exists(const std::wstring &path);
bool file_exists(const std::wstring &path);

// 환경 변수 → wstring. 없으면 빈 문자열.
std::wstring read_env_var(const wchar_t *name);

// 레지스트리 ──────────────────────────────────────────────────────────────
// extra: 보통 0 / KEY_WOW64_32KEY / KEY_WOW64_64KEY.
std::wstring read_registry_string(HKEY root, const wchar_t *subkey,
                                  const wchar_t *value, REGSAM extra);

// .exe 필터링 ──────────────────────────────────────────────────────────────
bool is_excluded_exe_name(const std::wstring &basename);
bool is_excluded_subdir(const wchar_t *dirname);

// dir에서 .exe를 재귀 검색 (kMaxScanDepth, kMinExeBytes 적용 + exclude).
// out에 절대 경로 append. 빈 dir이면 그대로 반환.
void scan_dir_for_exes(const std::wstring &dir, int depth,
                       std::vector<std::wstring> &out);

// install dir 한 곳에 대해 GameInfo들을 만들어 out에 append.
// - 같은 basename 중복 제거 (한 게임의 DX11/DX12 등은 모두 추가).
// - install_dir가 없거나 .exe 못 찾으면 아무것도 추가 안 함.
void emit_games_for_installdir(const std::wstring &display_name,
                               const std::wstring &install_dir,
                               const std::wstring &source,
                               std::vector<GameInfo> &out);

} // namespace securecast::game_src_common
