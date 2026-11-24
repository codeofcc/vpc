cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "vpc")
project(${PROJECT_NAME} LANGUAGES CXX C ASM)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(Git REQUIRED)

# 通用 git clone 函数
# 用法: git_clone_if_needed(<url> <tag> <dest_dir>)
function(git_clone_if_needed _url _tag _dest)
    if(EXISTS "${_dest}/.git")
        message(STATUS "[Git] ${_dest} already exists, skipping clone.")
        return()
    endif()

    message(STATUS "[Git] Cloning ${_url} (tag: ${_tag}) -> ${_dest}")
    file(MAKE_DIRECTORY "${_dest}")

    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --depth 1 -b "${_tag}" "${_url}" "${_dest}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
    )

    if(NOT _result EQUAL 0)
        # clone 失败时清理残留空目录，避免下次误判为"已存在"
        file(REMOVE_RECURSE "${_dest}")
        message(FATAL_ERROR "[Git] Clone failed (${_result}): ${_error}")
    endif()

    message(STATUS "[Git] Clone succeeded: ${_dest}")
endfunction()


include(FetchContent)



set(FFI_PACKAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps")



# ── 下载 libgo ──
git_clone_if_needed(
    "https://github.com/codeofcc/libgo.git"
    "master"
    "${FFI_PACKAGE_DIR}/libgo"
)





# 在 MakeAvailable 之前设置 libgo 的选项（覆盖默认值）
set(LIBGO_ENABLE_DEBUGGER OFF CACHE BOOL "" FORCE)
set(LIBGO_DISABLE_HOOK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libgo
    SOURCE_DIR "${FFI_PACKAGE_DIR}/libgo"
)


FetchContent_MakeAvailable(libgo)


# include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../ffi/include")


list(APPEND LIBRARIES
    # SDL2::SDL2
    libgo::libgo
)



file(GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/utils/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# fvp build fix: Windows-only sources (D3D renderer, DXVA2, SoundTouch via vcpkg)
# are globbed unconditionally; exclude them on non-Windows hosts.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    list(FILTER SOURCES EXCLUDE REGEX "/d3dRender\\.c$")
    list(FILTER SOURCES EXCLUDE REGEX "/ffmpeg_dxva2\\.c$")
endif()

add_library(${PROJECT_NAME} STATIC ${SOURCES})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)


target_compile_options(${PROJECT_NAME} PRIVATE -pthread)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads
    # android
)

# ✅ 添加模块文件,导出的接口
target_sources(${PROJECT_NAME}
    PUBLIC
    FILE_SET CXX_MODULES
    FILES
    "src/video_player_core.cppm" # ✅ 更新为新的文件名
)



message(STATUS "CMAKE_HOST_SYSTEM_NAME:${CMAKE_HOST_SYSTEM_NAME}")



if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(${PROJECT_NAME} PUBLIC
        dxva2
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE COBJMACROS)
endif()




# ── 平台相关配置 ──
# 依赖获取方式：vcpkg 统一处理 Windows / Linux / macOS 以及 Android 交叉编译
# 所有平台统一走 vcpkg；已移除手编 ffmpeg / git clone SDL2 的旧分支。



    #    "-DCMAKE_SYSTEM_NAME=Android" ^
    # [        ]       "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" ^
    # [        ]       "-DCMAKE_SYSTEM_VERSION=24" ^
    # [        ]       "-DANDROID_PLATFORM=android-24" ^
    # [        ]       "-DANDROID_ABI=arm64-v8a" ^
    # [        ]       "-DCMAKE_ANDROID_ARCH_ABI=arm64-v8a" ^
    message(STATUS "ANDROID_ABI=${ANDROID_ABI}")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(VCPKG_TARGET_TRIPLET "arm64-android")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(VCPKG_TARGET_TRIPLET "arm-neon-android")
    elseif(ANDROID_ABI STREQUAL "x86")
        set(VCPKG_TARGET_TRIPLET "x86-android")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(VCPKG_TARGET_TRIPLET "x64-android")
    elseif(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static-md")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx")
        else()
            set(VCPKG_TARGET_TRIPLET "x64-osx")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(VCPKG_TARGET_TRIPLET "arm64-linux")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-linux")
    endif()

    # message("VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}")

    set(VCPKG_DIR "${FFI_PACKAGE_DIR}/vcpkg")
    set(FFMPEG_VERSION_TAG "2026.07.29") # 👈 指定目标版本 8.1.2 //查看方式：在vcpkg的git查看ffmpeg包的版本对应的tag
    find_package(Git REQUIRED)

    # 平台差异：Windows 用 bootstrap-vcpkg.bat / vcpkg.exe，其它平台用 .sh / vcpkg
    if(WIN32)
        set(_vcpkg_bootstrap "${VCPKG_DIR}/bootstrap-vcpkg.bat")
        set(VCPKG_EXEC "${VCPKG_DIR}/vcpkg.exe")
    else()
        set(_vcpkg_bootstrap "${VCPKG_DIR}/bootstrap-vcpkg.sh")
        set(VCPKG_EXEC "${VCPKG_DIR}/vcpkg")
    endif()

    if(NOT EXISTS "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
        execute_process(COMMAND ${GIT_EXECUTABLE} clone -b ${FFMPEG_VERSION_TAG} --depth=1 https://github.com/microsoft/vcpkg.git "${VCPKG_DIR}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${_vcpkg_bootstrap}" -disableMetrics WORKING_DIRECTORY "${VCPKG_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    # 3. 注入 toolchain 并安装（保持 HEAD 最新状态）
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
    set(VCPKG_TARGET_TRIPLET "${VCPKG_TARGET_TRIPLET}" CACHE STRING "" FORCE)

    if(ANDROID)
        # vcpkg 的 *-android triplet 需要 NDK：优先沿用已有的 ANDROID_NDK
        if(NOT DEFINED ENV{ANDROID_NDK_HOME} AND DEFINED ANDROID_NDK)
            set(ENV{ANDROID_NDK_HOME} "${ANDROID_NDK}")
        endif()
        if(DEFINED ANDROID_NDK)
            set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${ANDROID_NDK}/build/cmake/android.toolchain.cmake" CACHE FILEPATH "" FORCE)
        endif()
        set(ENV{ANDROID_PLATFORM} "android-${ANDROID_NATIVE_API_LEVEL}")
    endif()
    execute_process(
        COMMAND "${VCPKG_EXEC}" install "ffmpeg[avcodec,avformat,swresample,swscale]:${VCPKG_TARGET_TRIPLET}"
        WORKING_DIRECTORY "${VCPKG_DIR}"
        COMMAND_ECHO STDOUT
        COMMAND_ERROR_IS_FATAL ANY
    )

    list(APPEND CMAKE_MODULE_PATH "${VCPKG_DIR}/installed/${VCPKG_TARGET_TRIPLET}/share/ffmpeg")
    list(APPEND CMAKE_MODULE_PATH "${VCPKG_DIR}/installed/${VCPKG_TARGET_TRIPLET}/share/sdl2")
    list(APPEND CMAKE_MODULE_PATH "${VCPKG_DIR}/installed/${VCPKG_TARGET_TRIPLET}/share/soundtouch")


    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_DIR}/installed/${VCPKG_TARGET_TRIPLET}")
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
    find_package(FFMPEG MODULE REQUIRED)
    list(APPEND INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
    # message(STATUS "FFMPEG_INCLUDE_DIRS=${FFMPEG_INCLUDE_DIRS}")
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})

    # message(STATUS "ANDROID_NDK_HOME = $ENV{ANDROID_NDK_HOME}")
    # message(STATUS "ANDROID_NDK_HOME = ${ANDROID_NDK_HOME}")
    execute_process(
        COMMAND "${VCPKG_EXEC}" install "sdl2[core]:${VCPKG_TARGET_TRIPLET}"
        WORKING_DIRECTORY "${VCPKG_DIR}"
        COMMAND_ECHO STDOUT
        COMMAND_ERROR_IS_FATAL ANY
    )


    find_package(SDL2 CONFIG REQUIRED)

    target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL2::SDL2-static

    )

    execute_process(
        COMMAND "${VCPKG_EXEC}" install "soundtouch:${VCPKG_TARGET_TRIPLET}"
        WORKING_DIRECTORY "${VCPKG_DIR}"
        COMMAND_ECHO STDOUT
        COMMAND_ERROR_IS_FATAL ANY
    )

    find_package(SoundTouch CONFIG REQUIRED)


    target_link_libraries(${PROJECT_NAME} PRIVATE
        SoundTouch::SoundTouch
    )




target_include_directories(${PROJECT_NAME} PRIVATE
    ${INCLUDE_DIRS}
)
target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/D3D/include"
)

target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden)





target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARIES})


