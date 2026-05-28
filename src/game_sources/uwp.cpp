// =============================================================================
// game_sources/uwp.cpp — UWP 게임 enum 구현
//
// 워커 스레드(autodetect_worker)에서 호출됨 → 자체 apartment 초기화.
// 단일 package에서 InstalledLocation 접근 실패는 그 package만 skip.
// =============================================================================

#include "uwp.h"
#include "common.h"
#include "../plugin-support.h" // obs_log

#include <obs.h>

#include <windows.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include <exception>
#include <string>
#include <vector>

namespace securecast {

namespace {
using namespace securecast::game_src_common;
} // namespace

std::vector<GameInfo> enum_uwp_games() {
  std::vector<GameInfo> out;

  // 워커 스레드는 fresh — multithread apartment 초기화. 이미 다른 mode로
  // 초기화된 환경(드물지만 OBS 일부 경로)에서 throw할 수 있어 try.
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (...) {
    // 이미 초기화돼 있어도 PackageManager 호출은 가능.
  }

  try {
    using namespace winrt::Windows::Management::Deployment;
    using namespace winrt::Windows::ApplicationModel;

    PackageManager pm;
    auto packages = pm.FindPackagesForUser(L"");
    size_t scanned = 0;
    for (auto const &pkg : packages) {
      ++scanned;
      try {
        if (pkg.IsFramework() || pkg.IsResourcePackage())
          continue;

        // InstalledLocation 접근은 패키지가 이미 제거된 상태에서 throw 가능.
        std::wstring path;
        try {
          path = std::wstring(pkg.InstalledLocation().Path());
        } catch (...) {
          continue;
        }
        if (path.empty() || !directory_exists(path))
          continue;

        std::wstring display = std::wstring(pkg.DisplayName());
        if (display.empty()) {
          try {
            display = std::wstring(pkg.Id().Name());
          } catch (...) {
            display = basename_of(path);
          }
        }

        // emit_games_for_installdir가 .exe 크기/exclude 필터로 비-게임을
        // 자연스럽게 거른다. Microsoft.* 시스템 앱들은 대부분 큰 .exe가 없어
        // 자동 skip.
        emit_games_for_installdir(display, path, L"UWP", out);
      } catch (...) {
        // 단일 package 실패는 전체 fail시키지 않음.
        continue;
      }
    }
    blog(LOG_INFO, "[uwp_enum] scanned %zu package(s) → %zu candidate(s)",
         scanned, out.size());
  } catch (winrt::hresult_error const &e) {
    blog(LOG_WARNING, "[uwp_enum] WinRT error: 0x%08X",
         static_cast<unsigned>(e.code().value));
  } catch (std::exception const &e) {
    blog(LOG_WARNING, "[uwp_enum] exception: %s", e.what());
  } catch (...) {
    blog(LOG_WARNING, "[uwp_enum] unknown exception");
  }
  return out;
}

} // namespace securecast
