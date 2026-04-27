cmake_minimum_required(VERSION 3.28...3.30)

project(securecast VERSION 1.0.0)

# ---------------------------------------------------------------
# C++ 표준 설정 (Role C: std::atomic, std::thread 등 필수)
# ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------
# 빌드 옵션
# ---------------------------------------------------------------
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)

# ---------------------------------------------------------------
# OBS SDK 탐색
#
# 로컬 빌드: external/obs-studio/ 또는 시스템에 설치된 OBS에서 자동 탐색
# CI 빌드(GitHub Actions): -DCMAKE_PREFIX_PATH 로 SDK 경로를 전달하면 됨
# ---------------------------------------------------------------
find_package(libobs REQUIRED)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# Windows OS specific libraries for DWM & DirectX (Role A, Role C)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE d3d11 dxgi dwmapi)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE 
  src/plugin-main.cpp
  src/securecast-filter.cpp
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
