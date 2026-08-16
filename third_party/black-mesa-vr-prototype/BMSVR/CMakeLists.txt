cmake_minimum_required(VERSION 3.20)
project(BMSVR LANGUAGES CXX)

# Black Mesa is Win32 (x86). Configure with: cmake -A Win32 ...
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BMSVR_EMBED_DXVK "Produce d3d9.dll linked with DXVK VR" OFF)

set(BMSVR_SOURCES
  dllmain.cpp
  game.cpp
  hooks.cpp
  vr.cpp
  openxr_backend.cpp
  sdk/checksum_crc.cpp
  d3d9_vr_stub.cpp
)

add_library(bmsvr SHARED ${BMSVR_SOURCES})
target_compile_definitions(bmsvr PRIVATE WIN32 NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(bmsvr PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/sdk
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/minhook/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openxr/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/vulkan/include
)

set(OPENXR_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openxr/pkg/native/Win32/release/lib")
set(OPENXR_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openxr/pkg/native/Win32/release/bin")
set(MINHOOK_LIB "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/minhook/lib/Release/libMinHook.x86.lib")

target_link_directories(bmsvr PRIVATE ${OPENXR_LIB_DIR})
target_link_libraries(bmsvr PRIVATE openxr_loader ${MINHOOK_LIB} d3d9)

set_target_properties(bmsvr PROPERTIES OUTPUT_NAME "bmsvr" PREFIX "")

add_custom_command(TARGET bmsvr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENXR_BIN_DIR}/openxr_loader.dll"
    $<TARGET_FILE_DIR:bmsvr>
)

if(BMSVR_EMBED_DXVK)
  message(STATUS "Set BMSVR_EMBED_DXVK only after integrating thirdparty/dxvk sources")
endif()
