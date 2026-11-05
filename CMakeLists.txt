cmake_minimum_required(VERSION 3.24)
project(VRCapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS "${CMAKE_SOURCE_DIR}/deps")
set(OBS_SRC "${DEPS}/obs-studio")
set(IMPLIBS "${CMAKE_BINARY_DIR}/implibs")   # made by build.ps1 from the installed OBS DLLs
set(GENERATED "${CMAKE_BINARY_DIR}/generated")

add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W3 /utf-8)

# ---------------------------------------------------------------- OpenXR layer (loaded into the game)
add_library(XR_APILAYER_VRCAPTURE_obs SHARED layer/layer.cpp)
target_include_directories(XR_APILAYER_VRCAPTURE_obs PRIVATE "${DEPS}/openxr-sdk/include")
target_link_libraries(XR_APILAYER_VRCAPTURE_obs PRIVATE d3d11 dxgi shell32 ole32)
# static CRT: nothing extra has to exist next to the game
set_property(TARGET XR_APILAYER_VRCAPTURE_obs PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_custom_command(TARGET XR_APILAYER_VRCAPTURE_obs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/layer/XR_APILAYER_VRCAPTURE_obs.json"
            "$<TARGET_FILE_DIR:XR_APILAYER_VRCAPTURE_obs>/XR_APILAYER_VRCAPTURE_obs.json")

# ---------------------------------------------------------------- OBS plugin
file(WRITE "${GENERATED}/obsconfig.h"
"#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

add_library(obs_import SHARED IMPORTED)
set_target_properties(obs_import PROPERTIES IMPORTED_IMPLIB "${IMPLIBS}/obs.lib")
add_library(obs_frontend_import SHARED IMPORTED)
set_target_properties(obs_frontend_import PROPERTIES IMPORTED_IMPLIB "${IMPLIBS}/obs-frontend-api.lib")

set(OBS_INCLUDES "${OBS_SRC}/libobs" "${OBS_SRC}/frontend/api" "${GENERATED}")

add_library(vr-capture MODULE obs-plugin/plugin.cpp)
target_include_directories(vr-capture PRIVATE ${OBS_INCLUDES} "${DEPS}/openvr/headers")
target_link_libraries(vr-capture PRIVATE obs_import obs_frontend_import user32 d3d11)
set_property(TARGET vr-capture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
# OpenVR SDK v2.15.6 redistributable, loaded at runtime from the plugin's own folder
add_custom_command(TARGET vr-capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DEPS}/openvr/bin/win64/openvr_api.dll"
            "$<TARGET_FILE_DIR:vr-capture>/openvr_api.dll")

# ---------------------------------------------------------------- proof tests
add_executable(layer_mock_test tests/layer_mock_test.cpp)
target_include_directories(layer_mock_test PRIVATE "${DEPS}/openxr-sdk/include")
target_link_libraries(layer_mock_test PRIVATE d3d11 d3d12 dxgi)

add_executable(plugin_render_test tests/plugin_render_test.cpp)
target_include_directories(plugin_render_test PRIVATE ${OBS_INCLUDES})
target_link_libraries(plugin_render_test PRIVATE obs_import d3d11 dxgi)
set_property(TARGET plugin_render_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
