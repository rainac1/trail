cmake_minimum_required(VERSION 3.16)
project(trail LANGUAGES CXX)

add_executable(trail WIN32
  src/main.cpp
  src/mouse_history.cpp
  src/cursor_texture.cpp
  src/diag.cpp
  src/overlay_renderer.cpp
)

target_compile_features(trail PRIVATE cxx_std_17)
target_compile_definitions(trail PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

if(MSVC)
  # 注意：不要无条件加 /O2 —— Debug 配置（/Od /RTC1）会与之冲突，
  # 优化标志按配置应用。MSVC 无 /O3（会被忽略并告警 D9002），故用 /O2。
  target_compile_options(trail PRIVATE /utf-8 /W4 /EHsc /permissive-)
  target_compile_options(trail PRIVATE $<$<CONFIG:Release>:/O2>)
else()
  target_compile_options(trail PRIVATE -Wall -Wextra)
  target_compile_options(trail PRIVATE $<$<CONFIG:Release>:-O3>)
endif()

target_link_libraries(trail PRIVATE d2d1 d3d11 dxgi dcomp dwmapi winmm)
