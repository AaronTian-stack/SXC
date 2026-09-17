include(FetchContent)

set(SXC_TBB_VERSION "2023.1.0")

if(WIN32)
    set(_sxc_tbb_platform "win")
    set(_sxc_tbb_archive_extension "zip")
    set(_sxc_tbb_sha256 "cf6ee0c600fcb5c3a9b65e3e6e4781669d06f1bb1e37970d145fcde08eed8da9")
elseif(APPLE)
    set(_sxc_tbb_platform "mac")
    set(_sxc_tbb_archive_extension "tgz")
    set(_sxc_tbb_sha256 "7093fb44a793989b4d52ac3881dfa312dccf81f4d829f554da31528741f5e47d")
elseif(UNIX)
    set(_sxc_tbb_platform "lin")
    set(_sxc_tbb_archive_extension "tgz")
    set(_sxc_tbb_sha256 "349d0e8b08cae4a5ab2668d54ff4e90b0fa012a332de6fb156961ddc119cd617")
else()
    message(FATAL_ERROR "Unsupported platform for oneTBB")
endif()

FetchContent_Declare(
    oneTBB
    URL
        "https://github.com/uxlfoundation/oneTBB/releases/download/v${SXC_TBB_VERSION}/oneapi-tbb-${SXC_TBB_VERSION}-${_sxc_tbb_platform}.${_sxc_tbb_archive_extension}"
    URL_HASH "SHA256=${_sxc_tbb_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR "_prebuilt_package"
)
FetchContent_MakeAvailable(oneTBB)
FetchContent_GetProperties(oneTBB SOURCE_DIR _sxc_tbb_source_dir)

set(TBB_DIR "${_sxc_tbb_source_dir}/lib/cmake/tbb" CACHE PATH "oneTBB package directory" FORCE)
find_package(TBB CONFIG REQUIRED COMPONENTS tbb PATHS "${TBB_DIR}" NO_DEFAULT_PATH)

unset(_sxc_tbb_archive_extension)
unset(_sxc_tbb_platform)
unset(_sxc_tbb_sha256)
unset(_sxc_tbb_source_dir)
