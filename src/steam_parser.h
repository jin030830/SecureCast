// =============================================================================
// steam_parser.h — Game Mode v2 / T04: Valve VDF/ACF 파서
//
// 역할:
//   Steam이 사용하는 KeyValues(VDF/ACF) 텍스트 형식을 파싱해 트리로 변환.
//   T06(Steam enum)이 다음 파일들을 읽기 위한 기반:
//     - <Steam>/config/libraryfolders.vdf          → 라이브러리 폴더 경로 + appid
//     - <library>/steamapps/appmanifest_<id>.acf   → 게임 메타 (name, installdir)
//     - <Steam>/config/loginusers.vdf              → (사용 안 함 — 참고용)
//
// VDF 문법:
//   "key1"  "value"
//   "key2"
//   {
//       "subkey"   "subvalue"
//       "nested"
//       {
//           "deeper"  "x"
//       }
//   }
//   // 라인 주석. \", \\, \n, \t 이스케이프 처리.
//   인코딩: UTF-8 (BOM 허용).
//
// 사용 예:
//   VdfNode root = VdfParser().parse(L"C:\\Steam\\config\\libraryfolders.vdf");
//   for (const auto &[idx, folder] : root[L"libraryfolders"]) {
//     std::wstring path = folder[L"path"].as_string();
//     for (const auto &[appid, _] : folder[L"apps"]) { ... }
//   }
//
// 안전성:
//   파싱 실패/I/O 실패/잘린 파일 → 예외 없이 비어있는 subtree(.size() == 0)
//   반환. 호출 측은 결과만 확인하면 되며 try/catch 불필요.
//   존재하지 않는 키에 대한 operator[] / as_string()도 빈 값 반환.
// =============================================================================

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace securecast {

// 한 노드는 (a) 누락된 노드 (b) 리프 문자열 (c) 자식 맵 중 하나.
// monostate를 추가해 operator[] 체이닝 시 키가 없거나 타입이 안 맞을 때
// 안전하게 "missing"을 반환할 수 있게 한다 — 호출 측에서 매번 contains/has
// 검사를 안 해도 됨.
class VdfNode {
public:
  using Map = std::map<std::wstring, VdfNode>;
  using Variant = std::variant<std::monostate, std::wstring, Map>;

  VdfNode() = default;
  explicit VdfNode(std::wstring s) : data_(std::move(s)) {}
  explicit VdfNode(Map m) : data_(std::move(m)) {}

  bool is_missing() const { return data_.index() == 0; }
  bool is_leaf() const { return data_.index() == 1; }
  bool is_subtree() const { return data_.index() == 2; }
  explicit operator bool() const { return !is_missing(); }

  // Subtree access. 키가 없거나 현재 노드가 leaf/missing이면 missing 노드 반환.
  // 반환된 missing 노드도 operator[] 체인 가능 (계속 missing).
  const VdfNode &operator[](const wchar_t *key) const;
  const VdfNode &operator[](const std::wstring &key) const;

  // Leaf 값. subtree/missing이면 빈 wstring 반환.
  const std::wstring &as_string() const;

  // Subtree 순회 (range-based for 지원). subtree가 아니면 빈 range.
  Map::const_iterator begin() const;
  Map::const_iterator end() const;
  size_t size() const;

  bool contains(const std::wstring &key) const;

private:
  Variant data_;
};

class VdfParser {
public:
  // 파일 경로에서 읽어 파싱. 실패하면 빈 subtree 노드 반환.
  // 예외 던지지 않음 (모든 std::exception 캐치 후 로그).
  VdfNode parse(const std::wstring &path);

  // 메모리 안의 UTF-8 컨텐츠를 파싱. 테스트/재사용용.
  VdfNode parse_string(std::string_view utf8_content);
};

// UTF-8 → UTF-16 변환 헬퍼. 빈 입력이면 빈 wstring 반환.
// 잘못된 UTF-8 시퀀스는 MultiByteToWideChar 기본 동작에 따라 무시/대체.
std::wstring utf8_to_utf16(std::string_view utf8);

} // namespace securecast
