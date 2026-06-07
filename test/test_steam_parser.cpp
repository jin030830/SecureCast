// test_steam_parser.cpp — Valve VDF/ACF 파서 단위 테스트
//
// 빌드/실행:
//   cmake --preset windows-x64 -DBUILD_TESTING=ON
//   cmake --build --preset windows-x64
//   ctest --preset windows-x64 --output-on-failure -R steam_parser
//
// 커버리지:
//   - 빈 입력 / 단순 key-value / 중첩 subtree
//   - UTF-8 한글 디코딩
//   - 백슬래시 이스케이프 (\", \\, \n, \t)
//   - // 라인 주석
//   - UTF-8 BOM 자동 스킵
//   - 손상된 입력(잘린 string, 불균형 brace) → 예외 없이 부분 결과
//   - 누락 키 / 잘못된 타입 접근 → 안전한 missing 노드
//   - 실제 libraryfolders.vdf / appmanifest_*.acf 구조

#include "../src/steam_parser.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_passed = 0, g_failed = 0;

static void check(bool ok, const char *name) {
  if (ok) {
    std::printf("[PASS] %s\n", name);
    ++g_passed;
  } else {
    std::printf("[FAIL] %s\n", name);
    ++g_failed;
  }
}

using securecast::VdfNode;
using securecast::VdfParser;
using securecast::utf8_to_utf16;

// ─────────────────────────────────────────────────────────────────
// utf8_to_utf16
// ─────────────────────────────────────────────────────────────────
static void test_utf8_helper() {
  // ASCII
  check(utf8_to_utf16("hello") == L"hello", "utf8 ascii");
  // 빈 입력
  check(utf8_to_utf16("") == L"", "utf8 empty");
  // 한글 (UTF-8 3바이트 시퀀스)
  // "리니지W" = EB A6 AC EB 8B 88 EC A7 80 57
  const char *korean = "\xEB\xA6\xAC\xEB\x8B\x88\xEC\xA7\x80W";
  std::wstring w = utf8_to_utf16(korean);
  check(w == L"리니지W", "utf8 korean");
}

// ─────────────────────────────────────────────────────────────────
// 기본 파싱
// ─────────────────────────────────────────────────────────────────
static void test_empty_input() {
  VdfNode root = VdfParser().parse_string("");
  check(root.is_subtree(), "empty input → subtree");
  check(root.size() == 0, "empty input size 0");
}

static void test_simple_kv() {
  const char *src = "\"foo\" \"bar\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root.size() == 1, "simple kv size 1");
  check(root[L"foo"].is_leaf(), "simple kv leaf type");
  check(root[L"foo"].as_string() == L"bar", "simple kv value");
}

static void test_nested_subtree() {
  const char *src =
      "\"libraryfolders\"\n"
      "{\n"
      "    \"0\"\n"
      "    {\n"
      "        \"path\"  \"C:\\\\Steam\"\n"
      "        \"label\" \"\"\n"
      "    }\n"
      "}\n";
  VdfNode root = VdfParser().parse_string(src);
  check(root.size() == 1, "nested top-level size");
  const VdfNode &lf = root[L"libraryfolders"];
  check(lf.is_subtree(), "libraryfolders subtree");
  check(lf.size() == 1, "libraryfolders has 1 child");
  const VdfNode &zero = lf[L"0"];
  check(zero.is_subtree(), "[0] is subtree");
  check(zero[L"path"].as_string() == L"C:\\Steam",
        "[0][path] backslash escape");
  check(zero[L"label"].as_string() == L"", "[0][label] empty string");
}

// ─────────────────────────────────────────────────────────────────
// 인코딩 / 이스케이프
// ─────────────────────────────────────────────────────────────────
static void test_utf8_korean_value() {
  // "name" "리니지W"
  const char *src = "\"name\" \"\xEB\xA6\xAC\xEB\x8B\x88\xEC\xA7\x80W\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"name"].as_string() == L"리니지W",
        "utf8 korean value");
}

static void test_escape_sequences() {
  const char *src = "\"k\" \"line1\\nline2\\twith tab and \\\"quote\\\"\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"k"].as_string() == L"line1\nline2\twith tab and \"quote\"",
        "escape \\n \\t \\\"");
}

static void test_double_backslash() {
  // Windows 경로 — '\\' 두 개가 '\' 하나로 변환되어야 한다.
  const char *src = "\"path\" \"C:\\\\Program Files\\\\Steam\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"path"].as_string() == L"C:\\Program Files\\Steam",
        "double backslash → single");
}

static void test_line_comments() {
  const char *src =
      "// 헤더 주석\n"
      "\"a\" \"1\"\n"
      "// 중간 주석\n"
      "\"b\" \"2\" // 끝줄 주석\n";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"a"].as_string() == L"1", "comment 후 a");
  check(root[L"b"].as_string() == L"2", "comment 후 b");
}

static void test_utf8_bom() {
  // EF BB BF prefix.
  const char *src = "\xEF\xBB\xBF\"k\" \"v\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"k"].as_string() == L"v", "BOM 후 정상 파싱");
}

// ─────────────────────────────────────────────────────────────────
// 손상된 입력 → 예외 안전
// ─────────────────────────────────────────────────────────────────
static void test_unterminated_string() {
  // 닫는 " 없음 — Error 토큰. 파서는 그 시점에 stop, 직전까지 파싱은 보존.
  const char *src = "\"k1\" \"v1\"\n\"k2\" \"unterminated\n";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"k1"].as_string() == L"v1", "unterminated 전까지 보존");
  // 결과적으로 예외가 나지 않으면 OK — k2는 파싱 실패해 missing.
  check(true, "unterminated string no throw");
}

static void test_unbalanced_brace() {
  const char *src =
      "\"outer\"\n"
      "{\n"
      "    \"a\" \"1\"\n"
      // 닫는 } 없음, EOF
      ;
  VdfNode root = VdfParser().parse_string(src);
  // 부분 결과 — outer subtree는 EOF로 종료되지만 안의 a=1은 보존.
  const VdfNode &outer = root[L"outer"];
  check(outer.is_subtree(), "unbalanced brace: outer still subtree");
  check(outer[L"a"].as_string() == L"1", "unbalanced brace: inner value preserved");
}

static void test_top_level_rbrace() {
  // top-level에 ' } '가 먼저 나오는 비정상 입력. 그 시점에 파싱 종료, 예외 없음.
  const char *src = "} \"k\" \"v\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root.size() == 0, "top-level rbrace → 즉시 종료");
}

// ─────────────────────────────────────────────────────────────────
// 누락 키 / 잘못된 타입 접근 — 안전한 missing 노드
// ─────────────────────────────────────────────────────────────────
static void test_missing_key() {
  const char *src = "\"a\" \"1\"";
  VdfNode root = VdfParser().parse_string(src);
  const VdfNode &missing = root[L"nope"];
  check(missing.is_missing(), "missing key → missing node");
  check(!missing, "missing node operator bool");
  check(missing.as_string() == L"", "missing.as_string empty");
}

static void test_chain_on_missing() {
  // missing → missing → missing → as_string("")
  const char *src = "\"a\" \"1\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"x"][L"y"][L"z"].as_string() == L"",
        "체이닝 모두 missing OK");
}

static void test_index_on_leaf() {
  // leaf 노드에 operator[] → missing. as_string은 leaf 값 반환.
  const char *src = "\"a\" \"1\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root[L"a"][L"x"].is_missing(), "leaf[키] → missing");
  check(root[L"a"].as_string() == L"1", "leaf as_string 유효");
}

static void test_iteration_on_non_subtree() {
  // missing/leaf의 begin/end는 empty range.
  const char *src = "\"a\" \"1\"";
  VdfNode root = VdfParser().parse_string(src);
  int n = 0;
  for (auto it = root[L"a"].begin(); it != root[L"a"].end(); ++it)
    ++n;
  check(n == 0, "leaf iteration: empty range");
  int n2 = 0;
  for (auto it = root[L"missing"].begin(); it != root[L"missing"].end(); ++it)
    ++n2;
  check(n2 == 0, "missing iteration: empty range");
}

// ─────────────────────────────────────────────────────────────────
// 실제 Steam 파일 구조
// ─────────────────────────────────────────────────────────────────
static void test_real_libraryfolders() {
  // C:\Program Files (x86)\Steam\config\libraryfolders.vdf의 실제 구조.
  const char *src =
      "\"libraryfolders\"\n"
      "{\n"
      "\t\"0\"\n"
      "\t{\n"
      "\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
      "\t\t\"label\"\t\t\"\"\n"
      "\t\t\"contentid\"\t\t\"7797262327368233308\"\n"
      "\t\t\"apps\"\n"
      "\t\t{\n"
      "\t\t\t\"228980\"\t\t\"0\"\n"
      "\t\t\t\"1086940\"\t\"7654321\"\n"
      "\t\t}\n"
      "\t}\n"
      "\t\"1\"\n"
      "\t{\n"
      "\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n"
      "\t\t\"apps\"\n"
      "\t\t{\n"
      "\t\t\t\"570\"\t\t\"0\"\n"
      "\t\t}\n"
      "\t}\n"
      "}\n";
  VdfNode root = VdfParser().parse_string(src);
  const VdfNode &lf = root[L"libraryfolders"];
  check(lf.size() == 2, "libraryfolders 2 libraries");
  check(lf[L"0"][L"path"].as_string() ==
            L"C:\\Program Files (x86)\\Steam",
        "library 0 path");
  check(lf[L"1"][L"path"].as_string() == L"D:\\SteamLibrary",
        "library 1 path");
  // apps 안의 numeric key들
  check(lf[L"0"][L"apps"].size() == 2, "library 0 has 2 apps");
  check(lf[L"0"][L"apps"][L"228980"].as_string() == L"0",
        "228980 → \"0\"");
  check(lf[L"1"][L"apps"][L"570"].as_string() == L"0",
        "570 → \"0\"");
}

static void test_real_appmanifest() {
  // appmanifest_<id>.acf의 실제 구조.
  const char *src =
      "\"AppState\"\n"
      "{\n"
      "\t\"appid\"\t\t\"1599340\"\n"
      "\t\"Universe\"\t\t\"1\"\n"
      "\t\"name\"\t\t\"Lost Ark\"\n"
      "\t\"StateFlags\"\t\t\"4\"\n"
      "\t\"installdir\"\t\t\"Lost Ark\"\n"
      "\t\"LastUpdated\"\t\t\"1700000000\"\n"
      "\t\"SizeOnDisk\"\t\t\"30000000000\"\n"
      "\t\"InstalledDepots\"\n"
      "\t{\n"
      "\t\t\"1599341\"\n"
      "\t\t{\n"
      "\t\t\t\"manifest\"\t\t\"1234567890\"\n"
      "\t\t}\n"
      "\t}\n"
      "}\n";
  VdfNode root = VdfParser().parse_string(src);
  const VdfNode &state = root[L"AppState"];
  check(state.is_subtree(), "AppState subtree");
  check(state[L"appid"].as_string() == L"1599340", "appid");
  check(state[L"name"].as_string() == L"Lost Ark", "name");
  check(state[L"installdir"].as_string() == L"Lost Ark", "installdir");
  check(state[L"InstalledDepots"][L"1599341"][L"manifest"].as_string() ==
            L"1234567890",
        "deep nested manifest");
}

static void test_real_appmanifest_korean() {
  // 한글 게임명이 들어간 ACF.
  // "name" "잘 알려진 한국 게임"  (UTF-8)
  const char *src =
      "\"AppState\"\n"
      "{\n"
      "\t\"appid\"\t\t\"123\"\n"
      "\t\"name\"\t\t\"\xEC\x9E\x98 \xEC\x95\x8C\xEB\xA0\xA4\xEC\xA7\x84 "
      "\xED\x95\x9C\xEA\xB5\xAD \xEA\xB2\x8C\xEC\x9E\x84\"\n"
      "\t\"installdir\"\t\"KoreanGame\"\n"
      "}\n";
  VdfNode root = VdfParser().parse_string(src);
  const std::wstring &name = root[L"AppState"][L"name"].as_string();
  // 첫 글자 "잘" (U+C798) 확인.
  check(!name.empty() && name[0] == L'잘', "ACF 한글 이름 디코딩");
  check(root[L"AppState"][L"installdir"].as_string() == L"KoreanGame",
        "ACF installdir");
}

// ─────────────────────────────────────────────────────────────────
// contains() / 추가 API
// ─────────────────────────────────────────────────────────────────
static void test_contains() {
  const char *src = "\"a\" \"1\" \"b\" \"2\"";
  VdfNode root = VdfParser().parse_string(src);
  check(root.contains(L"a"), "contains a");
  check(root.contains(L"b"), "contains b");
  check(!root.contains(L"c"), "!contains c");
}

static void test_iteration() {
  const char *src = "\"a\" \"1\" \"b\" \"2\" \"c\" \"3\"";
  VdfNode root = VdfParser().parse_string(src);
  int n = 0;
  std::wstring concat;
  for (const auto &kv : root) {
    concat += kv.first + L"=" + kv.second.as_string() + L";";
    ++n;
  }
  check(n == 3, "iteration count");
  // std::map은 정렬 — a, b, c 순서.
  check(concat == L"a=1;b=2;c=3;", "iteration order + content");
}

// ─────────────────────────────────────────────────────────────────
// 파일 I/O
// ─────────────────────────────────────────────────────────────────
static void test_parse_nonexistent_file() {
  VdfNode root =
      VdfParser().parse(L"Z:\\definitely_does_not_exist_42.vdf");
  check(root.is_subtree(), "nonexistent file → empty subtree");
  check(root.size() == 0, "nonexistent file size 0");
}

// ─────────────────────────────────────────────────────────────────
int main() {
  std::printf("=== Steam VDF/ACF parser tests ===\n");

  test_utf8_helper();

  test_empty_input();
  test_simple_kv();
  test_nested_subtree();

  test_utf8_korean_value();
  test_escape_sequences();
  test_double_backslash();
  test_line_comments();
  test_utf8_bom();

  test_unterminated_string();
  test_unbalanced_brace();
  test_top_level_rbrace();

  test_missing_key();
  test_chain_on_missing();
  test_index_on_leaf();
  test_iteration_on_non_subtree();

  test_real_libraryfolders();
  test_real_appmanifest();
  test_real_appmanifest_korean();

  test_contains();
  test_iteration();

  test_parse_nonexistent_file();

  std::printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
