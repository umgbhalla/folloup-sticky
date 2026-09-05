cmake_minimum_required(VERSION 3.16)
project(folloup_native_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB EPAPER_UI_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../components/epaper_ui/*.cpp")
file(GLOB PROJECT_ASSET_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../components/project_assets/*.cpp")

add_executable(renderer native/renderer.cpp ${EPAPER_UI_SOURCES} ${PROJECT_ASSET_SOURCES})
target_include_directories(renderer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/epaper_ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/epaper_ui/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/design_tokens/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/project_assets")
