cmake_minimum_required(VERSION 3.20)
project(OpenXRHelper64 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "OpenXRHelper64 is x64. Configure with: cmake -A x64 -B build_openxr_helper64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DXVK "${ROOT}/third_party/l4d2vr/dxvk_new")

add_executable(OpenXRHelper64 openxr_helper_main.cpp)

target_compile_definitions(OpenXRHelper64 PRIVATE
  WIN32
  _CONSOLE
  NOMINMAX
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  $<$<CONFIG:Release>:NDEBUG>
)

target_include_directories(OpenXRHelper64 PRIVATE
  "${ROOT}/src"
  "${ROOT}/third_party/openxr/include"
  "${DXVK}/include/vulkan/include"
)

target_compile_options(OpenXRHelper64 PRIVATE
  /MP
  /bigobj
  /W3
  /wd4244
  /wd4267
  /wd4996
)

target_link_libraries(OpenXRHelper64 PRIVATE
  d3d11
  d3dcompiler
  dxgi
)

set_target_properties(OpenXRHelper64 PROPERTIES
  WIN32_EXECUTABLE FALSE
)
