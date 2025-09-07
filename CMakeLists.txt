cmake_minimum_required(VERSION 3.18)
project(SXC LANGUAGES CXX)

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
    target_compile_options(SXC PRIVATE /MP)
endif()

target_include_directories(SXC PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${REPO_ROOT}/QhenkiX/include"
    "${REPO_ROOT}/QhenkiX/qhenkiX"
)

target_link_libraries(SXC PRIVATE QhenkiX)

target_link_libraries(SXC PRIVATE 
    $<IF:$<CONFIG:Debug>,${CMAKE_CURRENT_SOURCE_DIR}/lib/tbb12_debug.lib,${CMAKE_CURRENT_SOURCE_DIR}/lib/tbb12.lib>
)

add_custom_command(TARGET SXC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/redist/tbb12_debug.dll>"
        "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_CURRENT_SOURCE_DIR}/redist/tbb12.dll>"
        $<TARGET_FILE_DIR:SXC>
)
