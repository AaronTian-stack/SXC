cmake_minimum_required(VERSION 3.21)
project(SXC VERSION 0.5.1 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SlangCompiler.cmake)

set(SXC_TBB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/tbb")
if(WIN32)
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-win/oneapi-tbb-2022.3.0/lib/cmake/tbb")
elseif(APPLE)
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-mac/oneapi-tbb-2022.3.0/lib/cmake/tbb")
else()
    set(TBB_DIR "${SXC_TBB_ROOT}/oneapi-tbb-2022.3.0-lin/oneapi-tbb-2022.3.0/lib/cmake/tbb")
endif()

find_package(TBB CONFIG REQUIRED COMPONENTS tbb)

add_library(SXCShaderBlob INTERFACE)
add_library(SXC::ShaderBlob ALIAS SXCShaderBlob)
set_target_properties(SXCShaderBlob PROPERTIES EXPORT_NAME ShaderBlob)
target_compile_features(SXCShaderBlob INTERFACE cxx_std_20)
target_include_directories(SXCShaderBlob INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_executable(SXC
    main.cpp
    compiler_job.cpp
    compiler_job.h
    src/slang_shader_compiler.cpp
    src/slang_shader_compiler.h
    include/sxc/shader_blob.h
    include/sxc/shader_compiler.h
)
add_executable(SXC::Compiler ALIAS SXC)
set_target_properties(SXC PROPERTIES EXPORT_NAME Compiler)

target_compile_features(SXC PRIVATE cxx_std_20)
target_compile_definitions(SXC PRIVATE SXC_VERSION="${PROJECT_VERSION}")
target_include_directories(SXC PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
)
target_link_libraries(SXC PRIVATE TBB::tbb ${SXC_SLANG_LINK_TARGET})

if(APPLE)
    set_property(TARGET SXC APPEND PROPERTY BUILD_RPATH "@loader_path")
    set_property(TARGET SXC APPEND PROPERTY INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_property(TARGET SXC APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET SXC APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    target_link_libraries(SXC PRIVATE dl)
endif()

if(MSVC)
    target_compile_options(SXC PRIVATE /MP /EHsc)
else()
    target_compile_options(SXC PRIVATE -fexceptions)
endif()

if(WIN32)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:SXC>
            $<TARGET_FILE_DIR:SXC>
        COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SXC_SLANG_GLSLANG_FILE}"
            $<TARGET_FILE_DIR:SXC>
    )
    sxc_stage_dxc_runtime(SXC)
    if(EXISTS "${SXC_SLANG_BINARIES_DIR}/d3dcompiler_47.dll")
        add_custom_command(TARGET SXC POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SXC_SLANG_BINARIES_DIR}/d3dcompiler_47.dll"
                $<TARGET_FILE_DIR:SXC>
        )
    endif()
elseif(UNIX)
    add_custom_command(TARGET SXC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${SXC_SLANG_LINK_TARGET}>
            "${SXC_SLANG_GLSLANG_FILE}"
            $<TARGET_FILE:TBB::tbb>
            $<TARGET_FILE_DIR:SXC>
    )
endif()

install(TARGETS SXC SXCShaderBlob
    EXPORT SXCTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/sxc/shader_blob.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sxc
)

set(SXC_RUNTIME_FILES
    $<TARGET_FILE:${SXC_SLANG_LINK_TARGET}>
    ${SXC_SLANG_GLSLANG_FILE}
    $<TARGET_FILE_DIR:SXC>/$<TARGET_FILE_NAME:TBB::tbb>
)
if(WIN32)
    list(APPEND SXC_RUNTIME_FILES
        "${SXC_SLANG_BINARIES_DIR}/dxcompiler.dll"
        "${SXC_SLANG_BINARIES_DIR}/dxil.dll"
    )
    if(EXISTS "${SXC_SLANG_BINARIES_DIR}/d3dcompiler_47.dll")
        list(APPEND SXC_RUNTIME_FILES
            "${SXC_SLANG_BINARIES_DIR}/d3dcompiler_47.dll")
    endif()
endif()
install(FILES ${SXC_RUNTIME_FILES}
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

configure_package_config_file(
    cmake/SXCConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/SXCConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SXC"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SXCConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT SXCTargets
    FILE SXCTargets.cmake
    NAMESPACE SXC::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SXC"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SXCConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SXCConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SXC"
)
