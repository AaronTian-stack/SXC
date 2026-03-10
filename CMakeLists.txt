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
    "${REPO_ROOT}/QhenkiX/qhenki"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
)

target_link_libraries(SXC PRIVATE QhenkiX)
target_link_libraries(SXC PRIVATE TBB::tbb)

if(WIN32)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:SXC>
            $<TARGET_FILE_DIR:SXC>
        COMMAND_EXPAND_LISTS
    )
endif()
