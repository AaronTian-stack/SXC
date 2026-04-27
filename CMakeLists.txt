cmake_minimum_required(VERSION 3.18)
project(SXC LANGUAGES CXX)
include(${CMAKE_CURRENT_SOURCE_DIR}/../QhenkiX/cmake/ScopedBuildSharedLibs.cmake)

set(SXC_TBB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/tbb")
if(WIN32)
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-win/oneapi-tbb-2022.3.0/lib/cmake/tbb")
elseif(APPLE)
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-mac/oneapi-tbb-2022.3.0/lib/cmake/tbb")
else()
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-lin/oneapi-tbb-2022.3.0/lib/cmake/tbb")
endif()

find_package(TBB CONFIG REQUIRED COMPONENTS tbb)

set(SXC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/compiler_job.cpp"
)

set(SXC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/compiler_job.h"
)

add_executable(SXC ${SXC_SOURCES} ${SXC_HEADERS})

target_compile_features(SXC PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(SXC PRIVATE /MP /EHsc)
else()
    target_compile_options(SXC PRIVATE -fexceptions)
endif()

target_include_directories(SXC PRIVATE
    "${REPO_ROOT}/QhenkiX/include"
    "${REPO_ROOT}/QhenkiX/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
)

target_link_libraries(SXC PRIVATE QhenkiX)
target_link_libraries(SXC PRIVATE TBB::tbb)

if(WIN32)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<FILTER:$<TARGET_RUNTIME_DLLS:SXC>,EXCLUDE,SDL3\\.dll$>
            $<TARGET_FILE_DIR:SXC>
        COMMAND_EXPAND_LISTS
    )

    set(SXC_DXC_ROOT "${REPO_ROOT}/QhenkiX/external/DirectXShaderCompiler")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SXC_DXC_BIN_DIR "${SXC_DXC_ROOT}/dxc_2026_02_20/bin/x64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(SXC_DXC_BIN_DIR "${SXC_DXC_ROOT}/dxc_2026_02_20/bin/x86")
    else()
        message(WARNING "SXC: Unknown pointer size; defaulting DXC binary directory to x64")
        set(SXC_DXC_BIN_DIR "${SXC_DXC_ROOT}/dxc_2026_02_20/bin/x64")
    endif()

    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SXC_DXC_BIN_DIR}/dxcompiler.dll"
            "${SXC_DXC_BIN_DIR}/dxil.dll"
            $<TARGET_FILE_DIR:SXC>
    )
elseif(UNIX)
    set(SXC_DXC_ROOT "${REPO_ROOT}/QhenkiX/external/DirectXShaderCompiler")
    set(SXC_DXC_BIN_DIR "${SXC_DXC_ROOT}/linux_dxc_2026_02_20.x86_64/lib")

    # libdxil.so ships with SONAME "libdxildll.so"
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SXC_DXC_BIN_DIR}/libdxcompiler.so"
            $<TARGET_FILE_DIR:SXC>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SXC_DXC_BIN_DIR}/libdxil.so"
            "$<TARGET_FILE_DIR:SXC>/libdxildll.so"
    )
endif()
