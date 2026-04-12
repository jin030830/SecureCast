cmake_minimum_required(VERSION 3.28...3.30)

project(securecast VERSION 1.0.0)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)

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
