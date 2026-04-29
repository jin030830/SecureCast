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

# bootstrap.cmake가 buildspec.json을 읽어 _name, _version, _author 등의 변수를 설정.
# NO_POLICY_SCOPE: bootstrap이 설정한 cmake_policy를 이 파일에서도 그대로 쓰게.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

# project name과 version은 buildspec.json에서 주입된 값 사용.
# (직접 "securecast 1.0.0" 박지 않는 이유: 한 곳 (buildspec.json) 에서 관리)
project(${_name} VERSION ${_version})

# OBS 프론트엔드 API (메뉴/단축키 등) 사용 여부. 향후 패닉 버튼 단축키 등 위해 ON.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
# Qt6 UI 위젯 사용 여부. 현재 미사용이라 OFF. (OBS Properties API는 Qt와 무관)
option(ENABLE_QT "Use Qt functionality" OFF)

# 표준 OBS 플러그인 템플릿의 빌드 도우미들.
#   compilerconfig : 경고 수준, MSVC/Clang 별 플래그 등 컴파일러 공통 설정
#   defaults       : 플랫폼별 default include/lib 경로
#   helpers        : set_target_properties_plugin 같은 매크로 + plugin-support 라이브러리
include(compilerconfig)
include(defaults)
include(helpers)

# OBS 플러그인은 .dll 형태의 동적 라이브러리. exe도 정적 lib도 아님.
# MODULE 키워드는 "다른 라이브러리에 link되지 않는 동적 모듈"을 의미.
add_library(${CMAKE_PROJECT_NAME} MODULE)

# OBS 코어 라이브러리 (libobs) 찾아서 우리 dll에 link.
# OBS::libobs 타겟은 .deps/cmake/libobsConfig.cmake에서 정의됨.
find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# helpers_common.cmake가 만든 plugin-support 정적 라이브러리는 obs.h를 #include
# (blogva 등) 하므로 자기 자신도 libobs에 link해야 컴파일된다.
# 이 if 블록이 없으면 plugin-support 컴파일 시 obs.h를 못 찾아 에러.
if(TARGET plugin-support)
  target_link_libraries(plugin-support PRIVATE OBS::libobs)
endif()

# OBS 메인 UI와 통합 (메뉴 항목 추가, 글로벌 단축키 등록 등) 시 필요.
# 현재 코드는 호출하지 않지만 Role A 패닉 버튼 단축키에서 쓸 예정.
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# Qt6 위젯 기반 커스텀 다이얼로그가 필요한 경우만 ON.
# AUTOMOC/AUTOUIC/AUTORCC: Qt 메타 컴파일러를 자동 호출 (수동 moc 호출 회피).
if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Windows 전용 시스템 라이브러리.
#   d3d11/dxgi : Role C의 N-Frame Delay Queue가 GPU 텍스처를 staging buffer로
#                옮길 때 필요 (직접 D3D 호출은 아직 없지만 미리 link).
#   dwmapi     : Role A의 window_tracker가 DwmGetWindowAttribute로 정확한
#                창 좌표를 얻기 위해 필수.
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE d3d11 dxgi dwmapi)
endif()

# 컴파일 단위로 묶을 소스 파일들.
# 헤더(.h)도 명시: Visual Studio 솔루션 트리에 표시 + IDE 내비게이션 개선.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp           # OBS 진입점 / 모듈 등록
    src/securecast-filter.cpp     # 필터 lifecycle 콜백 + dispatch table
    src/securecast-filter.h       # 공유 타입 (SecureCastFilter, BlurRect 등)
    src/plugin-support.h          # PLUGIN_NAME / obs_log 선언 (정의는 .c.in)
    
    src/ocr-engine.cpp  #    추가 OCR
)

if(WIN32)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/window_tracker.cpp      # Role A: 블랙리스트 앱 좌표 스캐너
      src/window_tracker.h
  )
endif()

# OBS 표준 매크로: 출력 파일명을 "${_name}.dll" 로 강제, install 타겟 자동 생성.
# (CMake 기본은 lib 접두사가 붙는데 OBS는 그걸 안 좋아함)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
