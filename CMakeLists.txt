cmake_minimum_required(VERSION 3.16)
project(subframe_cursor_trail LANGUAGES CXX)

add_executable(subframe_cursor_trail WIN32
  src/main.cpp
  src/cursor_sampler.cpp
  src/cursor_texture.cpp
  src/diag.cpp
  src/overlay_renderer.cpp
)

target_compile_features(subframe_cursor_trail PRIVATE cxx_std_17)
target_compile_definitions(subframe_cursor_trail PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

if(MSVC)
  target_compile_options(subframe_cursor_trail PRIVATE /utf-8 /W4 /O2 /EHsc /permissive-)
else()
  target_compile_options(subframe_cursor_trail PRIVATE -O2 -Wall -Wextra)
endif()

target_link_libraries(subframe_cursor_trail PRIVATE d2d1 d3d11 dxgi dcomp dwmapi winmm)
