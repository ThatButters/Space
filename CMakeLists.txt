cmake_minimum_required(VERSION 3.25)
project(Space VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPACE_ENABLE_VALIDATION "Enable Vulkan validation layers in Debug builds" ON)
option(SPACE_ENABLE_DLSS "Build with NVIDIA DLSS (NGX SDK) if it is present in external/dlss" ON)

# ---------------------------------------------------------------------------
# Dependencies (Vulkan from the LunarG SDK, everything else from vcpkg)
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V at build time, output next to the executable
# ---------------------------------------------------------------------------
file(GLOB SPACE_SHADER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_SOURCE_DIR}/shaders/*.comp)

set(SPACE_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPACE_SHADER_OUT_DIR})

set(SPACE_SHADER_BINARIES)
foreach(SHADER ${SPACE_SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV ${SPACE_SHADER_OUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -g -I ${CMAKE_SOURCE_DIR}/shaders -MD -MF ${SPV}.d ${SHADER} -o ${SPV}
        DEPENDS ${SHADER}
        DEPFILE ${SPV}.d
        COMMENT "glslc ${SHADER_NAME}"
        VERBATIM)
    list(APPEND SPACE_SHADER_BINARIES ${SPV})
endforeach()
add_custom_target(space_shaders DEPENDS ${SPACE_SHADER_BINARIES})

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SPACE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
add_executable(Space ${SPACE_SOURCES})
add_dependencies(Space space_shaders)

target_include_directories(Space PRIVATE ${CMAKE_SOURCE_DIR}/src ${Stb_INCLUDE_DIR})
target_link_libraries(Space PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    imgui::imgui
    spdlog::spdlog
    draco::draco
    WebP::webp
    $<$<PLATFORM_ID:Windows>:ole32>)

target_compile_definitions(Space PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_NONE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    SPACE_SOURCE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    $<$<CONFIG:Debug>:SPACE_DEBUG=1>
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${SPACE_ENABLE_VALIDATION}>>:SPACE_VALIDATION=1>)

if(MSVC)
    target_compile_options(Space PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
    # Windowed app but keep a console attached in Debug for logs.
    set_target_properties(Space PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
    target_link_options(Space PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
endif()

# ---------------------------------------------------------------------------
# NVIDIA DLSS Super Resolution (NGX SDK, Vulkan)
# Drop the SDK into external/dlss (see external/dlss/README.md).
# ---------------------------------------------------------------------------
set(SPACE_DLSS_DIR ${CMAKE_SOURCE_DIR}/external/dlss)
if(SPACE_ENABLE_DLSS AND EXISTS ${SPACE_DLSS_DIR}/include/nvsdk_ngx_vk.h AND EXISTS ${SPACE_DLSS_DIR}/lib/nvsdk_ngx_d.lib)
    message(STATUS "DLSS SDK found: NGX integration enabled")
    target_include_directories(Space PRIVATE ${SPACE_DLSS_DIR}/include)
    target_compile_definitions(Space PRIVATE SPACE_HAS_DLSS=1)
    # Dynamic-CRT import libraries; the _dbg flavour matches the Debug runtime.
    target_link_libraries(Space PRIVATE
        $<IF:$<CONFIG:Debug>,${SPACE_DLSS_DIR}/lib/nvsdk_ngx_d_dbg.lib,${SPACE_DLSS_DIR}/lib/nvsdk_ngx_d.lib>)
    add_custom_command(TARGET Space POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPACE_DLSS_DIR}/lib/nvngx_dlss.dll $<TARGET_FILE_DIR:Space>)
else()
    message(STATUS "DLSS SDK not found in external/dlss: building without DLSS")
endif()

# Shaders sit next to the exe. Assets are read from the source tree (multi-GB maps are not copied);
# an assets/ folder next to the exe takes precedence when present, for redistributable builds.
add_custom_command(TARGET Space POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPACE_SHADER_OUT_DIR} $<TARGET_FILE_DIR:Space>/shaders)

# ---------------------------------------------------------------------------
# TexBake: offline texture baker (JPEG/PNG -> mipmapped BC7 DDS via DirectXTex, GPU-accelerated)
# ---------------------------------------------------------------------------
add_executable(TexBake ${CMAKE_SOURCE_DIR}/tools/texbake.cpp)
target_include_directories(TexBake PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(TexBake PRIVATE Microsoft::DirectXTex d3d11 dxgi ole32)
target_compile_definitions(TexBake PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(TexBake PRIVATE /W3 /utf-8 /MP /bigobj)
endif()
