cmake_minimum_required(VERSION 3.20)
project(LcdFusion-WpeRenderer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)

set(WPE_UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wallpaper-scene-renderer")
set(WPE_SRC "${WPE_UPSTREAM}/src")
set(WPE_TP "${WPE_UPSTREAM}/third_party")

# SPIRV-Reflect static library from upstream submodule
add_library(spirv-reflect-static STATIC
    "${WPE_TP}/SPIRV-Reflect/spirv_reflect.c"
    "${WPE_TP}/SPIRV-Reflect/spirv_reflect.cpp"
)
target_include_directories(spirv-reflect-static PUBLIC "${WPE_TP}")
target_compile_definitions(spirv-reflect-static PRIVATE SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# Core scene renderer static library (Windows headless Vulkan subset, no Qt/OpenGL/miniaudio)
add_library(wescene-core STATIC
    # Utils
    "${WPE_SRC}/Utils/Algorism.cpp"
    "${WPE_SRC}/Utils/DynamicLibrary.cpp"
    "${WPE_SRC}/Utils/FpsCounter.cpp"
    "${WPE_SRC}/Utils/FrameTimer.cpp"
    "${WPE_SRC}/Utils/Logging.cpp"
    "${WPE_SRC}/Utils/Sha.cpp"
    # Timer
    "${WPE_SRC}/Timer/FrameTimer.cpp"
    "${WPE_SRC}/Timer/ThreadTimer.cpp"
    # Audio stub (M2: silent stub; M4: WASAPI loopback + FFT)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SoundManagerStub.cpp"
    # Scene
    "${WPE_SRC}/Scene/Scene.cpp"
    "${WPE_SRC}/Scene/SceneCamera.cpp"
    "${WPE_SRC}/Scene/SceneImageEffectLayer.cpp"
    "${WPE_SRC}/Scene/SceneIndexArray.cpp"
    "${WPE_SRC}/Scene/SceneNode.cpp"
    "${WPE_SRC}/Scene/SceneShader.cpp"
    "${WPE_SRC}/Scene/SceneVertexArray.cpp"
    # Particle
    "${WPE_SRC}/Particle/ParticleEmitter.cpp"
    "${WPE_SRC}/Particle/ParticleModify.cpp"
    "${WPE_SRC}/Particle/ParticleSystem.cpp"
    "${WPE_SRC}/Particle/WPParticleRawGener.cpp"
    # RenderGraph
    "${WPE_SRC}/RenderGraph/DependencyGraph.cpp"
    "${WPE_SRC}/RenderGraph/PassNode.cpp"
    "${WPE_SRC}/RenderGraph/RenderGraph.cpp"
    "${WPE_SRC}/RenderGraph/TexNode.cpp"
    # Vulkan
    "${WPE_SRC}/Vulkan/Device.cpp"
    "${WPE_SRC}/Vulkan/GraphicsPipeline.cpp"
    "${WPE_SRC}/Vulkan/Instance.cpp"
    "${WPE_SRC}/Vulkan/Parameters.cpp"
    "${WPE_SRC}/Vulkan/Shader.cpp"
    "${WPE_SRC}/Vulkan/StagingBuffer.cpp"
    "${WPE_SRC}/Vulkan/Swapchain.cpp"
    "${WPE_SRC}/Vulkan/TextureCache.cpp"
    "${WPE_SRC}/Vulkan/Vma.cpp"
    "${WPE_SRC}/Vulkan/vulkan_wrapper.cpp"
    # VulkanRender
    "${WPE_SRC}/VulkanRender/CopyPass.cpp"
    "${WPE_SRC}/VulkanRender/CustomShaderPass.cpp"
    "${WPE_SRC}/VulkanRender/FinPass.cpp"
    "${WPE_SRC}/VulkanRender/PrePass.cpp"
    "${WPE_SRC}/VulkanRender/SceneToRenderGraph.cpp"
    "${WPE_SRC}/VulkanRender/VulkanRender.cpp"
    # Wallpaper Engine parsers
    "${WPE_SRC}/Type.cpp"
    "${WPE_SRC}/WPJson.cpp"
    "${WPE_SRC}/WPPkgFs.cpp"
    "${WPE_SRC}/wpscene/WPImageObject.cpp"
    "${WPE_SRC}/wpscene/WPParticleObject.cpp"
    "${WPE_SRC}/wpscene/WPMaterial.cpp"
    "${WPE_SRC}/wpscene/WPScene.cpp"
    "${WPE_SRC}/wpscene/WPLightObject.cpp"
    "${WPE_SRC}/WPParticleParser.cpp"
    "${WPE_SRC}/WPShaderParser.cpp"
    "${WPE_SRC}/WPSceneParser.cpp"
    "${WPE_SRC}/WPShaderValueUpdater.cpp"
    "${WPE_SRC}/WPTexImageParser.cpp"
    "${WPE_SRC}/WPSoundParser.cpp"
    "${WPE_SRC}/WPMdlParser.cpp"
    "${WPE_SRC}/WPPuppet.cpp"
)

target_include_directories(wescene-core PUBLIC
    "${WPE_SRC}"
    "${WPE_SRC}/Swapchain"
    "${WPE_SRC}/Fs/include"
    "${WPE_SRC}/Fs/include/Fs"
    "${WPE_SRC}/Utils/include"
    "${WPE_SRC}/Timer/include"
    "${WPE_SRC}/Timer/include/Timer"
    "${WPE_SRC}/Audio/include"
    "${WPE_SRC}/Audio/include/Audio"
    "${WPE_SRC}/Scene/include"
    "${WPE_SRC}/Scene/include/Scene"
    "${WPE_SRC}/Particle/include"
    "${WPE_SRC}/Particle/include/Particle"
    "${WPE_SRC}/RenderGraph/include"
    "${WPE_SRC}/RenderGraph/include/RenderGraph"
    "${WPE_SRC}/Vulkan/include"
    "${WPE_SRC}/Vulkan/include/Vulkan"
    "${WPE_SRC}/Vulkan/include/vvk"
    "${WPE_SRC}/VulkanRender/include"
    "${WPE_SRC}/VulkanRender/include/VulkanRender"
    "${WPE_TP}"
)

target_compile_definitions(wescene-core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(wescene-core PUBLIC
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/win_compat.h"
)
target_compile_options(wescene-core PRIVATE
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-switch
)

# Locate static archives for self-contained Windows executable
find_library(GLSLANG_STATIC_LIB NAMES libglslang.a glslang REQUIRED)
find_library(GLSLANG_RES_STATIC_LIB NAMES libglslang-default-resource-limits.a glslang-default-resource-limits REQUIRED)
find_library(SPIRV_STATIC_LIB NAMES libSPIRV.a SPIRV REQUIRED)
find_library(SPIRV_TOOLS_OPT_STATIC_LIB NAMES libSPIRV-Tools-opt.a SPIRV-Tools-opt REQUIRED)
find_library(SPIRV_TOOLS_STATIC_LIB NAMES libSPIRV-Tools.a SPIRV-Tools REQUIRED)
find_library(LZ4_STATIC_LIB NAMES liblz4.a lz4 REQUIRED)

target_link_libraries(wescene-core PUBLIC
    Eigen3::Eigen
    nlohmann_json::nlohmann_json
    spirv-reflect-static
    ${GLSLANG_STATIC_LIB}
    ${GLSLANG_RES_STATIC_LIB}
    ${SPIRV_STATIC_LIB}
    ${SPIRV_TOOLS_OPT_STATIC_LIB}
    ${SPIRV_TOOLS_STATIC_LIB}
    ${LZ4_STATIC_LIB}
    Vulkan::Vulkan
    Threads::Threads
)

# QuickJS-ng static library (MIT, embedded ES2023/ESM runtime for SceneScript)
set(QJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs-ng")
add_library(qjs-static STATIC
    "${QJS_DIR}/dtoa.c"
    "${QJS_DIR}/libregexp.c"
    "${QJS_DIR}/libunicode.c"
    "${QJS_DIR}/quickjs.c"
)
set_target_properties(qjs-static PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)
target_include_directories(qjs-static PUBLIC "${QJS_DIR}")
target_compile_definitions(qjs-static PRIVATE
    QUICKJS_NG_BUILD
    _GNU_SOURCE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0601
)
target_compile_options(qjs-static PRIVATE
    -funsigned-char
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-unused-but-set-variable
    -Wno-unused-result
    -Wno-stringop-truncation
    -Wno-array-bounds
)

# Standalone offscreen renderer executable (x64)
add_executable(wpe-renderer
    src/AudioSpectrumWin32.cpp
    src/GpuSelectWin32.cpp
    src/VideoPlayerWin32.cpp
    src/WebWallpaperWin32.cpp
    src/TextLayerRenderer.cpp
    src/SceneScriptEngine.cpp
    src/ShmWriter.cpp
    src/RendererEngine.cpp
    src/main.cpp
)

target_link_libraries(wpe-renderer PRIVATE
    wescene-core
    qjs-static
    dwmapi
    shlwapi
    gdiplus
    gdi32
    mfplat
    mfreadwrite
    mfuuid
    ole32
    uuid
)

if(MINGW)
    find_library(WINPTHREAD_STATIC_LIB NAMES libwinpthread.a REQUIRED)
    target_link_libraries(wpe-renderer PRIVATE ${WINPTHREAD_STATIC_LIB})
    target_link_options(wpe-renderer PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,--stack,8388608
        -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic
    )
endif()
