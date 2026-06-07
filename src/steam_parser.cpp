// =============================================================================
// steam_parser.cpp — VDF/ACF 파서 구현
//
// 토큰 종류:
//   String   : "quoted" 또는 unquoted identifier
//   LBrace   : '{'
//   RBrace   : '}'
//   End      : 입력 끝
//   Error    : 잘못된 입력 (예: unterminated string)
//
// 파싱 흐름:
//   parse_subtree(top=true) — 키 토큰, 값 토큰(LBrace면 재귀, String이면 leaf).
//   RBrace를 만나면 부모로 복귀. End를 만나면 종료.
//
// 토크나이저는 string_view 기반 zero-copy — 파싱 중 원본 바이트 버퍼가
// 유효해야 한다. parse()/parse_string()이 같은 스택 프레임 안에서 모두
// 처리하므로 안전.
// =============================================================================

#include "steam_parser.h"
#include "plugin-support.h" // obs_log

#include <obs.h>

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace securecast {

namespace {

enum class TokenKind {
  String,
  LBrace,
  RBrace,
  End,
  Error,
};

struct Token {
  TokenKind kind = TokenKind::End;
  std::wstring text;
};

class Tokenizer {
public:
  explicit Tokenizer(std::string_view src) : src_(src) {
    // UTF-8 BOM 스킵 (있을 수도 없을 수도 있음 — Steam은 보통 BOM 없음).
    if (src_.size() >= 3 && (uint8_t)src_[0] == 0xEF &&
        (uint8_t)src_[1] == 0xBB && (uint8_t)src_[2] == 0xBF) {
      pos_ = 3;
    }
  }

  Token next() {
    skip_ws_and_comments();
    if (pos_ >= src_.size())
      return {TokenKind::End, {}};

    char c = src_[pos_];
    if (c == '{') {
      ++pos_;
      return {TokenKind::LBrace, L"{"};
    }
    if (c == '}') {
      ++pos_;
      return {TokenKind::RBrace, L"}"};
    }
    if (c == '"')
      return read_quoted_string();
    return read_unquoted_string();
  }

private:
  void skip_ws_and_comments() {
    while (pos_ < src_.size()) {
      char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
        // // 라인 주석 — 줄 끝까지 스킵.
        pos_ += 2;
        while (pos_ < src_.size() && src_[pos_] != '\n')
          ++pos_;
      } else {
        break;
      }
    }
  }

  Token read_quoted_string() {
    // pos_가 여는 " 위에 있다고 가정.
    ++pos_;
    std::string raw;
    raw.reserve(32);
    while (pos_ < src_.size()) {
      char c = src_[pos_];
      if (c == '\\' && pos_ + 1 < src_.size()) {
        char e = src_[pos_ + 1];
        switch (e) {
        case '"':
          raw += '"';
          break;
        case '\\':
          raw += '\\';
          break;
        case 'n':
          raw += '\n';
          break;
        case 't':
          raw += '\t';
          break;
        case 'r':
          raw += '\r';
          break;
        default:
          // 정의되지 않은 이스케이프는 그대로 보존 (Valve의 실제 동작).
          raw += e;
          break;
        }
        pos_ += 2;
        continue;
      }
      if (c == '"') {
        ++pos_;
        return {TokenKind::String, utf8_to_utf16(raw)};
      }
      raw += c;
      ++pos_;
    }
    // 닫는 " 못 만나고 EOF — Error.
    return {TokenKind::Error, utf8_to_utf16(raw)};
  }

  Token read_unquoted_string() {
    // 최소 1자는 무조건 소비 — 진행 보장. read_unquoted_string에 진입한 시점에
    // c가 stop char가 아니어도 (예: 단독 '/'), 무한 루프 방지를 위해 강제 진행.
    size_t start = pos_;
    ++pos_;
    while (pos_ < src_.size()) {
      char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' ||
          c == '}' || c == '"') {
        break;
      }
      // // 주석 시작이면 여기서 token 종료.
      if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/')
        break;
      ++pos_;
    }
    return {TokenKind::String,
            utf8_to_utf16(std::string_view(src_.data() + start, pos_ - start))};
  }

  std::string_view src_;
  size_t pos_ = 0;
};

// 재귀 subtree 파서.
// at_top_level=true: 파일 최상위. End까지 읽고 종료. RBrace는 syntax error로 로그.
// at_top_level=false: '{' 직후. RBrace 만날 때까지 읽고 복귀. End는 truncated.
VdfNode::Map parse_subtree(Tokenizer &tk, bool at_top_level) {
  VdfNode::Map out;
  while (true) {
    Token key = tk.next();
    if (key.kind == TokenKind::End) {
      if (!at_top_level) {
        blog(LOG_WARNING,
             "[steam_parser] unexpected EOF inside subtree — file may be "
             "truncated");
      }
      return out;
    }
    if (key.kind == TokenKind::RBrace) {
      if (at_top_level) {
        blog(LOG_WARNING, "[steam_parser] unexpected '}' at top level");
      }
      return out;
    }
    if (key.kind != TokenKind::String) {
      blog(LOG_WARNING,
           "[steam_parser] expected key string, got kind=%d — stop subtree",
           static_cast<int>(key.kind));
      return out;
    }

    Token val = tk.next();
    if (val.kind == TokenKind::LBrace) {
      // '{' 소비됨 → 자식 subtree 재귀.
      out[std::move(key.text)] = VdfNode(parse_subtree(tk, false));
    } else if (val.kind == TokenKind::String) {
      out[std::move(key.text)] = VdfNode(std::move(val.text));
    } else {
      blog(LOG_WARNING,
           "[steam_parser] expected value or '{' after key, got kind=%d",
           static_cast<int>(val.kind));
      return out;
    }
  }
}

// raw 바이트 읽기 — UTF-8 변환은 이후 단계에서. 실패 시 빈 string.
std::string read_file_utf8(const std::wstring &path) {
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
    size_t read = fread(buf.data(), 1, buf.size(), f);
    buf.resize(read);
  }
  fclose(f);
  return buf;
}

// 누락 노드 싱글톤 — operator[] 체인 시 안전한 fallback.
const VdfNode &missing_node() {
  static const VdfNode kMissing;
  return kMissing;
}

} // namespace

// ───────────────────────── VdfNode ─────────────────────────

const VdfNode &VdfNode::operator[](const wchar_t *key) const {
  if (!key || !is_subtree())
    return missing_node();
  const auto &m = std::get<Map>(data_);
  auto it = m.find(key);
  return it == m.end() ? missing_node() : it->second;
}

const VdfNode &VdfNode::operator[](const std::wstring &key) const {
  return (*this)[key.c_str()];
}

const std::wstring &VdfNode::as_string() const {
  static const std::wstring kEmpty;
  if (is_leaf())
    return std::get<std::wstring>(data_);
  return kEmpty;
}

namespace {
// 비-subtree 노드에서 begin/end 호출 시 같은 컨테이너의 begin/end를 돌려줘야
// range-based for가 종료될 수 있다 (서로 다른 컨테이너의 iterator 비교는 UB).
const VdfNode::Map &empty_map_singleton() {
  static const VdfNode::Map kEmpty;
  return kEmpty;
}
} // namespace

VdfNode::Map::const_iterator VdfNode::begin() const {
  return is_subtree() ? std::get<Map>(data_).begin()
                      : empty_map_singleton().begin();
}

VdfNode::Map::const_iterator VdfNode::end() const {
  return is_subtree() ? std::get<Map>(data_).end()
                      : empty_map_singleton().end();
}

size_t VdfNode::size() const {
  return is_subtree() ? std::get<Map>(data_).size() : 0;
}

bool VdfNode::contains(const std::wstring &key) const {
  if (!is_subtree())
    return false;
  return std::get<Map>(data_).count(key) > 0;
}

// ───────────────────────── utf8_to_utf16 ─────────────────────────

std::wstring utf8_to_utf16(std::string_view utf8) {
  if (utf8.empty())
    return {};
  int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0)
    return {};
  std::wstring out;
  out.resize(static_cast<size_t>(needed));
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), needed);
  return out;
}

// ───────────────────────── VdfParser ─────────────────────────

VdfNode VdfParser::parse_string(std::string_view utf8_content) {
  try {
    Tokenizer tk(utf8_content);
    return VdfNode(parse_subtree(tk, /*at_top_level=*/true));
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[steam_parser] parse_string exception: %s", e.what());
    return VdfNode(VdfNode::Map{});
  } catch (...) {
    blog(LOG_WARNING, "[steam_parser] parse_string unknown exception");
    return VdfNode(VdfNode::Map{});
  }
}

VdfNode VdfParser::parse(const std::wstring &path) {
  try {
    std::string content = read_file_utf8(path);
    if (content.empty()) {
      blog(LOG_INFO, "[steam_parser] file empty or unreadable: %ls",
           path.c_str());
      return VdfNode(VdfNode::Map{});
    }
    return parse_string(content);
  } catch (const std::exception &e) {
    blog(LOG_WARNING, "[steam_parser] parse(file) exception: %s", e.what());
    return VdfNode(VdfNode::Map{});
  } catch (...) {
    blog(LOG_WARNING, "[steam_parser] parse(file) unknown exception");
    return VdfNode(VdfNode::Map{});
  }
}

} // namespace securecast
