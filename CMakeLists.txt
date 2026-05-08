# =============================================================================
# CMakeLists.txt — SecureCast (OBS Studio 32.x 플러그인) 빌드 정의
#
# 빌드 절차 (개발자):
#   1) 의존성 다운로드+OBS source build  →  cmake --preset windows-x64
#      (cmake/common/buildspec_common.cmake가 buildspec.json을 읽어 OBS source
#       와 obs-deps를 .deps/에 받고, libobs.lib 등을 install)
#   2) DLL 빌드  →  cmake --build --preset windows-x64
#      결과물: build_x64/RelWithDebInfo/securecast.dll
#   3) 배포 경로:
#      OBS 32 user plugin 경로 = %PROGRAMDATA%\obs-studio\plugins\securecast\
#        bin\64bit\securecast.dll
#        data\locale\en-US.ini
# =============================================================================

cmake_minimum_required(VERSION 3.28...3.30)

# SecureCast는 Windows 전용 플러그인이다.
# Windows.Media.Ocr(WinRT), DwmGetWindowAttribute(DWM), D3D11/DXGI, SetWinEventHook 등
# Windows 전용 API에 직접 의존한다. 다른 플랫폼은 지원하지 않는다.
if(NOT WIN32)
  message(FATAL_ERROR
    "SecureCast requires Windows.\n"
    "The plugin depends on Windows-only APIs:\n"
    "  - Windows.Media.Ocr  (WinRT OCR engine)\n"
    "  - DwmGetWindowAttribute  (window tracking)\n"
    "  - D3D11 / DXGI  (GPU readback)\n"
    "  - SetWinEventHook  (foreground window events)\n"
    "Other platforms are not supported in V1."
  )
endif()

# bootstrap.cmake가 buildspec.json을 읽어 _name, _version, _author 등의 변수를 설정.
# NO_POLICY_SCOPE: bootstrap이 설정한 cmake_policy를 이 파일에서도 그대로 쓰게 한다.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

# project name과 version은 buildspec.json에서 주입된 값 사용.
# 직접 "securecast 1.0.0"을 박지 않고 buildspec.json에서 한 번만 관리한다.
project(${_name} VERSION ${_version})

# OBS 프론트엔드 API 사용 여부.
# 향후 패닉 버튼 단축키, 메뉴 통합 등에 사용할 수 있다.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)

# Qt6 UI 위젯 사용 여부.
# 현재 미사용이라 OFF. OBS Properties API는 Qt와 무관하다.
option(ENABLE_QT "Use Qt functionality" OFF)

# 표준 OBS 플러그인 템플릿의 빌드 도우미들.
#   compilerconfig : 경고 수준, MSVC/Clang 별 플래그 등 컴파일러 공통 설정
#   defaults       : 플랫폼별 default include/lib 경로
#   helpers        : set_target_properties_plugin 같은 매크로 + plugin-support 라이브러리
include(compilerconfig)
include(defaults)
include(helpers)

# OBS 플러그인은 .dll 형태의 동적 라이브러리.
# MODULE 키워드는 다른 라이브러리에 link되지 않는 동적 모듈을 의미한다.
add_library(${CMAKE_PROJECT_NAME} MODULE)

# OBS 코어 라이브러리(libobs)를 찾아서 우리 DLL에 link한다.
# OBS::libobs 타겟은 .deps/cmake/libobsConfig.cmake에서 정의된다.
find_package(libobs REQUIRED)

# C++17 이상을 명시한다.
# C++/WinRT와 modern C++ 코드 사용을 위해 필요하다.
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# =============================================================================
# Google RE2 탐색
#
# 기존 문제:
#   - VCPKG_INSTALLATION_ROOT 또는 C:/vcpkg 경로를 사실상 전제로 두고
#     find_package(re2 CONFIG REQUIRED)를 호출했다.
#   - vcpkg가 없는 Linux/macOS, 다른 패키지 매니저, 직접 설치 환경에서 빌드가 깨진다.
#
# 수정 방향:
#   1) RE2_DIR 환경 변수가 있으면 우선 힌트로 사용
#   2) Windows에서는 vcpkg 경로가 실제 존재할 때만 힌트로 추가
#   3) find_package(re2 CONFIG QUIET)를 먼저 시도
#   4) 실패하면 pkg-config(re2)로 fallback
# =============================================================================

# 사용자가 RE2_DIR을 지정한 경우 우선 사용한다.
# 예:
#   Windows PowerShell:
#     $env:RE2_DIR="C:/vcpkg/installed/x64-windows/share/re2"
#   Linux/macOS:
#     export RE2_DIR=/usr/local/lib/cmake/re2
if(DEFINED ENV{RE2_DIR})
  list(APPEND CMAKE_PREFIX_PATH "$ENV{RE2_DIR}")
endif()

# Windows에서는 vcpkg가 실제로 있는 경우에만 힌트로 추가한다.
# 없으면 무시하고 시스템 패키지 또는 pkg-config fallback으로 넘어간다.
if(WIN32)
  if(DEFINED ENV{VCPKG_INSTALLATION_ROOT}
     AND EXISTS "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows")
  elseif(EXISTS "C:/vcpkg/installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "C:/vcpkg/installed/x64-windows")
  endif()
endif()

find_package(re2 CONFIG QUIET)

if(re2_FOUND)
  message(STATUS "RE2 found via CMake config package.")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE re2::re2)
else()
  message(STATUS "RE2 CMake config package not found. Trying pkg-config fallback.")

  find_package(PkgConfig QUIET)

  if(PkgConfig_FOUND)
    pkg_check_modules(RE2 QUIET re2)
  endif()

  if(RE2_FOUND)
    message(STATUS "RE2 found via pkg-config.")

    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${RE2_INCLUDE_DIRS})
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${RE2_CFLAGS_OTHER})

    if(RE2_LIBRARY_DIRS)
      target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${RE2_LIBRARY_DIRS})
    endif()

    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${RE2_LIBRARIES})
  else()
    message(FATAL_ERROR
      "RE2 not found. Install RE2 or set RE2_DIR.\n"
      "Windows vcpkg example:\n"
      "  vcpkg install re2:x64-windows\n"
      "  set RE2_DIR=C:/vcpkg/installed/x64-windows/share/re2\n"
      "Linux example:\n"
      "  sudo apt install libre2-dev pkg-config\n"
      "macOS example:\n"
      "  brew install re2 pkg-config"
    )
  endif()
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
  OBS::libobs
)

# helpers_common.cmake가 만든 plugin-support 정적 라이브러리는 obs.h를 include한다.
# 이 if 블록이 없으면 plugin-support 컴파일 시 obs.h를 못 찾아 에러가 날 수 있다.
if(TARGET plugin-support)
  target_link_libraries(plugin-support PRIVATE OBS::libobs)
endif()

# OBS 메인 UI와 통합할 때 필요하다.
# 현재 코드는 직접 호출하지 않지만 Role A 패닉 버튼 단축키에서 사용할 수 있다.
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# Qt6 위젯 기반 커스텀 다이얼로그가 필요한 경우만 ON.
# AUTOMOC/AUTOUIC/AUTORCC: Qt 메타 컴파일러를 자동 호출한다.
if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)

  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )

  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
      AUTOMOC ON
      AUTOUIC ON
      AUTORCC ON
  )
endif()

# Windows 전용 시스템 라이브러리.
#   d3d11/dxgi : Role C의 N-Frame Delay Queue가 GPU 텍스처를 staging buffer로
#                옮길 때 필요할 수 있다.
#   dwmapi     : Role A의 window_tracker가 DwmGetWindowAttribute로 정확한
#                창 좌표를 얻기 위해 필요하다.
#   windowsapp : C++/WinRT / Windows Runtime API 사용 시 필요할 수 있다.
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dwmapi
    windowsapp
  )
endif()

# 컴파일 단위로 묶을 소스 파일들.
# 헤더(.h)도 명시하여 Visual Studio 솔루션 트리에 표시하고 IDE 내비게이션을 개선한다.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp           # OBS 진입점 / 모듈 등록
    src/securecast-filter.cpp     # 필터 lifecycle 콜백 + dispatch table
    src/securecast-filter.h       # 공유 타입 (SecureCastFilter, BlurRect 등)
    src/securecast-types.h        # 순환 참조 방지용 공용 타입
    src/plugin-support.h          # PLUGIN_NAME / obs_log 선언 (정의는 .c.in)
    src/pixel-hash.cpp            # Role C: FNV-1a 픽셀 해시 캐시
    src/pixel-hash.h
    src/pipeline-health.cpp       # [NEW] 파이프라인 상태 모니터
    src/pipeline-health.h
    src/ocr-engine.cpp            # Role B: OCR + RE2 기반 PII 탐지
    src/ocr-engine.h              # Role B: OCR 엔진 인터페이스
)

if(WIN32)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/window_tracker.cpp      # Role A: 블랙리스트 앱 좌표 스캐너
      src/window_tracker.h
      src/win_event_listener.cpp  # Role A: SetWinEventHook push 감지
      src/win_event_listener.h
      src/gpu-readback.cpp        # Role C: GPU-CPU 비동기 Readback
      src/gpu-readback.h
  )
endif()


# OBS 표준 매크로: 출력 파일명을 "${_name}.dll" 로 강제, install 타겟 자동 생성.
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# 빌드 후 OBS 플러그인 경로에 자동 배포.
# CMAKE_INSTALL_PREFIX = C:/ProgramData/obs-studio/plugins (defaults.cmake에서 설정)
if(WIN32)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}/bin/64bit"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}/bin/64bit/"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/data"
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}/data"
    COMMENT "Deploy ${CMAKE_PROJECT_NAME} to OBS plugins: ${CMAKE_INSTALL_PREFIX}"
    VERBATIM
  )
endif()
# [핵심 해결] downsample.effect 등 필수 리소스를 OBS가 인식하는 표준 데이터 폴더로 복사
install(DIRECTORY data/ DESTINATION "data/obs-plugins/${CMAKE_PROJECT_NAME}")
