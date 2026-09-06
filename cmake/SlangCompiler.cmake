include_guard(GLOBAL)

set(SXC_SLANG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/slang")
if(NOT EXISTS "${SXC_SLANG_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Slang submodule is missing. Run: git submodule update --init --recursive")
endif()

set(SXC_SLANG_OPTIONS
    "-DSLANG_ENABLE_CUDA=OFF"
    "-DSLANG_ENABLE_OPTIX=OFF"
    "-DSLANG_ENABLE_NVAPI=OFF"
    "-DSLANG_ENABLE_AFTERMATH=OFF"
    "-DSLANG_ENABLE_GFX=OFF"
    "-DSLANG_ENABLE_SLANG_RHI=OFF"
    "-DSLANG_ENABLE_SLANGD=OFF"
    "-DSLANG_ENABLE_SLANGC=OFF"
    "-DSLANG_ENABLE_SLANGI=OFF"
    "-DSLANG_ENABLE_SLANGRT=OFF"
    "-DSLANG_ENABLE_SLANG_GLSLANG=ON"
    "-DSLANG_ENABLE_TESTS=OFF"
    "-DSLANG_ENABLE_EXAMPLES=OFF"
    "-DSLANG_ENABLE_REPLAYER=OFF"
    "-DSLANG_ENABLE_DXIL=ON"
    "-DSLANG_EXCLUDE_DAWN=ON"
    "-DSLANG_EXCLUDE_TINT=ON"
    "-DSLANG_SLANG_LLVM_FLAVOR=DISABLE"
    "-DSLANG_LIB_TYPE=SHARED"
)

if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    set(SXC_SLANG_BINARY_DIR "${CMAKE_BINARY_DIR}/_slang")
    set(SXC_SLANG_CONFIGURE_COMMAND
        "${CMAKE_COMMAND}"
        -S "${SXC_SLANG_SOURCE_DIR}"
        -B "${SXC_SLANG_BINARY_DIR}"
        -G "${CMAKE_GENERATOR}"
        "-DCMAKE_POLICY_DEFAULT_CMP0141=NEW"
        "-DCMAKE_C_FLAGS=/MP"
        "-DCMAKE_CXX_FLAGS=/MP"
        ${SXC_SLANG_OPTIONS}
    )
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND SXC_SLANG_CONFIGURE_COMMAND -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND SXC_SLANG_CONFIGURE_COMMAND -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()

    execute_process(
        COMMAND ${SXC_SLANG_CONFIGURE_COMMAND}
        RESULT_VARIABLE SXC_SLANG_CONFIGURE_RESULT
        COMMAND_ECHO STDOUT
    )
    if(NOT SXC_SLANG_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to configure the nested Slang build (${SXC_SLANG_CONFIGURE_RESULT})")
    endif()

    add_library(sxc_slang SHARED IMPORTED GLOBAL)
    set_target_properties(sxc_slang PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS SLANG_DYNAMIC
        INTERFACE_INCLUDE_DIRECTORIES "${SXC_SLANG_SOURCE_DIR}/include"
    )
    foreach(SXC_SLANG_CONFIG Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${SXC_SLANG_CONFIG}" SXC_SLANG_CONFIG_UPPER)
        set_property(TARGET sxc_slang PROPERTY
            "IMPORTED_IMPLIB_${SXC_SLANG_CONFIG_UPPER}"
            "${SXC_SLANG_BINARY_DIR}/${SXC_SLANG_CONFIG}/lib/slang-compiler.lib")
        set_property(TARGET sxc_slang PROPERTY
            "IMPORTED_LOCATION_${SXC_SLANG_CONFIG_UPPER}"
            "${SXC_SLANG_BINARY_DIR}/${SXC_SLANG_CONFIG}/bin/slang-compiler.dll")
    endforeach()

    add_custom_target(sxc_slang_build
        COMMAND "${CMAKE_COMMAND}" --build "${SXC_SLANG_BINARY_DIR}"
            --config $<CONFIG>
            --target slang slang-glslang copy-dxcompiler copy-dxil
            --parallel
        COMMENT "Building the Slang compiler dependency"
        VERBATIM
    )
    set_property(TARGET sxc_slang_build PROPERTY FOLDER "_Dependencies")
    add_dependencies(sxc_slang sxc_slang_build)
    set(SXC_SLANG_LINK_TARGET sxc_slang)
    set(SXC_SLANG_GLSLANG_FILE
        "${SXC_SLANG_BINARY_DIR}/$<CONFIG>/bin/slang-glslang.dll")
    set(SXC_SLANG_BINARIES_DIR "${SXC_SLANG_BINARY_DIR}/$<CONFIG>/bin")
else()
    set(SLANG_ENABLE_CUDA OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_OPTIX OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_NVAPI OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_GFX OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANG_RHI OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGD OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGC OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGI OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGRT OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANG_GLSLANG ON CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_REPLAYER OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_DXIL ON CACHE BOOL "" FORCE)
    set(SLANG_EXCLUDE_DAWN ON CACHE BOOL "" FORCE)
    set(SLANG_EXCLUDE_TINT ON CACHE BOOL "" FORCE)
    set(SLANG_SLANG_LLVM_FLAVOR DISABLE CACHE STRING "" FORCE)
    set(SLANG_LIB_TYPE SHARED CACHE STRING "" FORCE)

    add_subdirectory(
        "${SXC_SLANG_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/external/slang"
        EXCLUDE_FROM_ALL
    )
    set(SXC_SLANG_LINK_TARGET slang)
    set(SXC_SLANG_GLSLANG_FILE "$<TARGET_FILE:slang-glslang>")
    if(WIN32)
        add_dependencies(slang copy-dxcompiler copy-dxil)
        set(SXC_SLANG_BINARIES_DIR
            "${CMAKE_BINARY_DIR}/external/slang/$<CONFIG>/bin")
    endif()
endif()

function(sxc_stage_dxc_runtime TARGET_NAME)
    if(WIN32)
        set(_sxc_dxc_runtime_files
            "${SXC_SLANG_BINARIES_DIR}/dxcompiler.dll"
            "${SXC_SLANG_BINARIES_DIR}/dxil.dll"
        )
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_sxc_dxc_runtime_files}
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying Slang's pinned DXC runtime for ${TARGET_NAME}"
        )
    endif()
endfunction()
