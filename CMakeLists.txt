cmake_minimum_required(VERSION 3.24)
project(AHiTVR LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A Hat in Time's Win64 build requires a 64-bit target (-A x64)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# minhook
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

# imgui, used for the vr menu
# keep this pinned so updates dont randomly break the build
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.6
)
FetchContent_MakeAvailable(imgui)

add_library(hatvr_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(hatvr_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# openxr
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.63
)
FetchContent_MakeAvailable(openxr_sdk)

# vulkan
find_package(Vulkan REQUIRED)
message(STATUS "HatVR Vulkan SDK include: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "HatVR Vulkan SDK version: ${Vulkan_VERSION}")
target_link_libraries(hatvr_imgui PUBLIC Vulkan::Vulkan)

# =============================================================================
# HatVR.dll
#
# main mod dll, most of hatvr lives here
# =============================================================================
add_library(HatVR SHARED
    src/dllmain.cpp
    src/hatvr_exports.cpp
    src/graphics_backend.cpp
    src/d3d9_backend.cpp
    src/dxvk_intercept.cpp
    src/dxvk_trace.cpp
    src/vulkan_intercept.cpp
    src/vulkan_backend.cpp
    src/d3d9_hook.cpp
    src/resources/hatvr.rc
    src/core/shared_state.inl
    src/core/runtime_control.inl
    src/include_order/controller_input_order.inl
    src/include_order/openxr_runtime_order.inl
    src/camera/camera_hooks.inl
    src/ui/ui_discovery.inl
    src/rendering/d3d9_rendering.inl
    src/ui/ui_rendering.inl
    src/rendering/device_hooks.inl
)

set_source_files_properties(
    src/core/shared_state.inl
    src/core/runtime_control.inl
    src/include_order/controller_input_order.inl
    src/include_order/openxr_runtime_order.inl
    src/camera/camera_hooks.inl
    src/ui/ui_discovery.inl
    src/rendering/d3d9_rendering.inl
    src/ui/ui_rendering.inl
    src/rendering/device_hooks.inl
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_include_directories(HatVR PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(HatVR PRIVATE
    minhook
    hatvr_imgui
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d9.lib
    d3d12.lib
    dxgi.lib
    Advapi32.lib
    Psapi.lib
    Gdi32.lib
)

set_target_properties(HatVR PROPERTIES
    OUTPUT_NAME "HatVR"
    PREFIX ""
)

# =============================================================================
# d3d9.dll
#
# small d3d9 proxy that forwards the normal exports and loads HatVR.dll
# =============================================================================

# do this directly since cmakes masm support can have issues with vs2022
get_filename_component(HATVR_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(HATVR_ML64 "${HATVR_MSVC_BIN_DIR}/ml64.exe")
if(NOT EXISTS "${HATVR_ML64}")
    message(FATAL_ERROR "ml64.exe was not found beside the MSVC compiler: ${HATVR_ML64}")
endif()

set(HATVR_PROXY_STUBS_OBJ "${CMAKE_CURRENT_BINARY_DIR}/proxy_stubs.obj")
add_custom_command(
    OUTPUT "${HATVR_PROXY_STUBS_OBJ}"
    COMMAND "${HATVR_ML64}" /nologo /c "/Fo${HATVR_PROXY_STUBS_OBJ}" "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_stubs.asm"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_stubs.asm"
    COMMENT "Assembling proxy_stubs.asm"
    VERBATIM
)
set_source_files_properties("${HATVR_PROXY_STUBS_OBJ}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/proxy_exports.cpp
    "${HATVR_PROXY_STUBS_OBJ}"
    src/proxy.def
)

target_include_directories(d3d9 PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
)

# building the proxy should build hatvr too
add_dependencies(d3d9 HatVR)
