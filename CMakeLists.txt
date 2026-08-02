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

if(APPLE)
    set_property(TARGET SXC APPEND PROPERTY BUILD_RPATH "@loader_path")
elseif(UNIX)
    set_property(TARGET SXC APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endif()

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
target_link_libraries(SXC PRIVATE ${QHENKIX_SLANG_LINK_TARGET})

if(WIN32)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<FILTER:$<TARGET_RUNTIME_DLLS:SXC>,EXCLUDE,SDL3\\.dll$>
            $<TARGET_FILE_DIR:SXC>
        COMMAND_EXPAND_LISTS
    )

    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QHENKIX_SLANG_GLSLANG_FILE}"
            $<TARGET_FILE_DIR:SXC>
    )

    # Slang dynamically loads its pinned compiler runtimes for DXIL and DXBC.
    qhenkix_stage_dxc_runtime(SXC)
    if(EXISTS "${QHENKIX_SLANG_BINARIES_DIR}/d3dcompiler_47.dll")
        add_custom_command(TARGET SXC POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QHENKIX_SLANG_BINARIES_DIR}/d3dcompiler_47.dll"
                $<TARGET_FILE_DIR:SXC>
        )
    endif()
elseif(UNIX)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${QHENKIX_SLANG_LINK_TARGET}>
            "${QHENKIX_SLANG_GLSLANG_FILE}"
            $<TARGET_FILE_DIR:SXC>
    )

endif()
