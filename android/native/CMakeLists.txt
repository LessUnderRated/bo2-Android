# libmain.so: the library SDLActivity loads (SDL_main), plus SDL3 built from the same checkout whose
# Java glue the APK compiles (android/app/build.gradle.kts passes FETCHCONTENT_SOURCE_DIR_SDL3), so the
# two cannot disagree. Pattern from LessUnderRated's Chronicle-rs android/native.cmake.
#
# Batteries not included: nothing here is derived from the game. The app, the game host, the Vulkan renderer and its
# shader translator are built from this repository's sources; the game core (the runtime and the game recompiled from
# the player's own exe, libbo2core.so, scripts/package-game.sh) is built on the player's machine and loaded at run
# time from the app's storage (src/core_loader.cpp). Without it, main.cpp runs the diagnostics screen.
cmake_minimum_required(VERSION 3.22)
project(bo2_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../sdl3-tag.txt sdl3_tag LIMIT_COUNT 1)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG ${sdl3_tag}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ---- The shader translator: HLSLcc (this repository's copy) and glslang (the NDK's) ---------------------------------
set(HLSLCC ${REPO_ROOT}/third_party/HLSLcc)
file(GLOB HLSLCC_CPP ${HLSLCC}/src/*.cpp)
file(GLOB HLSLCC_C ${HLSLCC}/src/cbstring/*.c)
add_library(bo2_hlslcc STATIC ${HLSLCC_CPP} ${HLSLCC_C})
target_include_directories(bo2_hlslcc PRIVATE ${HLSLCC} ${HLSLCC}/include ${HLSLCC}/src ${HLSLCC}/src/internal_includes
    ${HLSLCC}/src/cbstring)
set_source_files_properties(${HLSLCC_CPP} PROPERTIES COMPILE_OPTIONS "-std=c++14")
target_compile_options(bo2_hlslcc PRIVATE -O2 -w)

set(GLSLANG ${ANDROID_NDK}/sources/third_party/shaderc/third_party/glslang)
set(GLSLANG_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(READ ${GLSLANG}/build_info.h.tmpl glslang_tmpl)
string(FIND "${glslang_tmpl}" "#define GLSLANG_VERSION_GREATER_THAN" glslang_at)
string(SUBSTRING "${glslang_tmpl}" ${glslang_at} -1 glslang_tail)
file(WRITE ${GLSLANG_GEN}/glslang/build_info.h
    "#ifndef GLSLANG_BUILD_INFO\n#define GLSLANG_BUILD_INFO\n#define GLSLANG_VERSION_MAJOR 11\n"
    "#define GLSLANG_VERSION_MINOR 11\n#define GLSLANG_VERSION_PATCH 0\n#define GLSLANG_VERSION_FLAVOR \"\"\n"
    "${glslang_tail}")
set(GLSLANG_SRC
    glslang/GenericCodeGen/CodeGen.cpp glslang/GenericCodeGen/Link.cpp OGLCompilersDLL/InitializeDll.cpp
    glslang/OSDependent/Unix/ossource.cpp SPIRV/GlslangToSpv.cpp SPIRV/InReadableOrder.cpp SPIRV/Logger.cpp
    SPIRV/SpvBuilder.cpp SPIRV/SpvPostProcess.cpp SPIRV/doc.cpp SPIRV/disassemble.cpp SPIRV/SpvTools.cpp)
foreach(f attribute Constant glslang_tab InfoSink Initialize Intermediate intermOut IntermTraverse iomapper limits
          linkValidate parseConst ParseContextBase ParseHelper PoolAlloc propagateNoContraction reflection RemoveTree
          Scan ShaderLang SpirvIntrinsics SymbolTable Versions)
    list(APPEND GLSLANG_SRC glslang/MachineIndependent/${f}.cpp)
endforeach()
foreach(f PpAtom PpContext Pp PpScanner PpTokens)
    list(APPEND GLSLANG_SRC glslang/MachineIndependent/preprocessor/${f}.cpp)
endforeach()
list(TRANSFORM GLSLANG_SRC PREPEND ${GLSLANG}/)
add_library(bo2_glslang STATIC ${GLSLANG_SRC})
target_include_directories(bo2_glslang PRIVATE ${GLSLANG} ${GLSLANG}/glslang/MachineIndependent ${GLSLANG_GEN})
target_compile_definitions(bo2_glslang PRIVATE GLSLANG_OSINCLUDE_UNIX)
target_compile_options(bo2_glslang PRIVATE -O2 -w -include cstdint)

# ---- The Vulkan renderer (runtime/gfx/vk) and the DXBC translator ------------------------------------------------
# The translation cache on the phone is keyed on what decides a translation (dxbc_to_vk.cpp and HLSLcc), not on the
# build: a new APK with the same translator keeps the phone's cache.
file(GLOB_RECURSE HLSLCC_KEYED ${HLSLCC}/src/*.cpp ${HLSLCC}/src/*.c ${HLSLCC}/src/*.h ${HLSLCC}/include/*.h)
list(SORT HLSLCC_KEYED)
set(tr_hashes "")
foreach(f ${REPO_ROOT}/runtime/gfx/dxbc_to_vk.cpp ${HLSLCC_KEYED})
    file(SHA1 ${f} h)
    string(APPEND tr_hashes ${h})
endforeach()
string(SHA1 tr_version "${tr_hashes}")
string(SUBSTRING ${tr_version} 0 12 tr_version)
file(GLOB VK_SRC ${REPO_ROOT}/runtime/gfx/vk/*.cpp)
add_library(bo2_render STATIC ${VK_SRC} ${REPO_ROOT}/runtime/gfx/dxbc_to_vk.cpp)
target_include_directories(bo2_render PRIVATE ${REPO_ROOT}/runtime/gfx/vk ${REPO_ROOT}/runtime/win32 ${HLSLCC}/include
    ${GLSLANG})
target_compile_definitions(bo2_render PRIVATE BO2_TR_VERSION="${tr_version}")
target_compile_options(bo2_render PRIVATE -O2 -w)
set_target_properties(bo2_hlslcc bo2_glslang bo2_render PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- libmain.so ----------------------------------------------------------------------------------------------------
add_library(main SHARED
    src/main.cpp
    src/touch_model.cpp
    src/touch_pad.cpp
    src/log.cpp
    src/remote_log.cpp
    src/jni.cpp
    src/perf.cpp
    src/device_report.cpp
    src/display_plan.cpp
    src/test_card.cpp
    src/graphics.cpp
    src/graphics_apply.cpp
    src/adpf.cpp
    src/prof.cpp
    src/game_host.cpp
    src/core_loader.cpp
    ${REPO_ROOT}/runtime/input/pad_sdl.cpp)
target_include_directories(main PRIVATE src ${REPO_ROOT}/runtime/gfx/vk)
target_compile_options(main PRIVATE -Wall -Wextra -Wno-unused-parameter)
# The renderer and translator whole: the game core (loaded later) calls into them, so nothing may be dropped as unused,
# and their symbols are exported for it (the core names libmain.so as a dependency and binds against it on load).
target_link_libraries(main PRIVATE -Wl,--whole-archive bo2_render bo2_hlslcc bo2_glslang -Wl,--no-whole-archive
    SDL3::SDL3-shared vulkan log android nativewindow dl)
