# cpp-httplib for arn-core.
#
# arn-core is the only provider of cpp-httplib in any build that contains it.
# It needs a small patch (Request::interrupt, per-request cancellation; see
# third_party/cpp-httplib/README.md), and every translation unit must see the
# same httplib.h: two copies would give different httplib::Request layouts
# (an ODR violation). So this module
#   1. refuses to configure when another httplib target already exists,
#   2. fetches pristine cpp-httplib v0.18.3 and verifies httplib.h by SHA-256,
#   3. writes the patched header into the build tree (the fetched sources are
#      never modified) and verifies the result by SHA-256,
#   4. defines the INTERFACE target arn_core_httplib (exported as
#      arn::httplib, also reachable as httplib::httplib).
# Hashes are taken over the text with LF line endings, so neither
# core.autocrlf nor CMake's own CRLF output on Windows changes them.
# include/arn/core/net/http_client.hpp additionally fails to compile unless the
# patched header is the one found, which catches any bypass of this module.

set(ARN_CORE_HTTPLIB_VERSION "0.18.3")
# Commit of tag v0.18.3. The tag is fetched; the content hash below is what is
# trusted.
set(ARN_CORE_HTTPLIB_COMMIT "a7bc00e3307fecdb4d67545e93be7b88cfb1e186")
set(ARN_CORE_HTTPLIB_SOURCE_SHA256
    "a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5")
set(ARN_CORE_HTTPLIB_PATCHED_SHA256
    "bf66c9bb2ce9319bceb73fd8a24553a247d5419c81b98b3c0739a40095c3a441")
set(ARN_CORE_HTTPLIB_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/cpp-httplib/v0.18.3-arn-interrupt.patch")
get_filename_component(ARN_CORE_HTTPLIB_PATCH "${ARN_CORE_HTTPLIB_PATCH}" ABSOLUTE)

# Reads a text file with CRLF converted to LF.
function(_arn_core_read_lf path out_var)
    file(READ "${path}" _content)
    string(REPLACE "\r\n" "\n" _content "${_content}")
    set(${out_var} "${_content}" PARENT_SCOPE)
endfunction()

# Writes the patched httplib.h into <out_dir>. Safe to run on every configure:
# the source is always verified, and a correct existing output is kept.
function(_arn_core_prepare_httplib source_dir out_dir)
    set(_source "${source_dir}/httplib.h")
    if (NOT EXISTS "${_source}")
        message(FATAL_ERROR "arn-core: cpp-httplib source not found at ${_source}.")
    endif()
    _arn_core_read_lf("${_source}" _pristine)
    string(SHA256 _source_hash "${_pristine}")
    if (NOT _source_hash STREQUAL ARN_CORE_HTTPLIB_SOURCE_SHA256)
        message(FATAL_ERROR
            "arn-core: ${_source} is not pristine cpp-httplib v${ARN_CORE_HTTPLIB_VERSION} "
            "(SHA-256 of LF text ${_source_hash}, expected ${ARN_CORE_HTTPLIB_SOURCE_SHA256}). "
            "Delete the stale FetchContent directory or remove the "
            "FETCHCONTENT_SOURCE_DIR_ARN_CORE_CPP_HTTPLIB override.")
    endif()

    # Hashes are always compared on LF text: on Windows file(WRITE) itself
    # writes CRLF, so raw file hashes differ between platforms.
    set(_header "${out_dir}/httplib.h")
    if (EXISTS "${_header}")
        _arn_core_read_lf("${_header}" _existing)
        string(SHA256 _have "${_existing}")
        if (_have STREQUAL ARN_CORE_HTTPLIB_PATCHED_SHA256)
            return()
        endif()
        file(REMOVE "${_header}")
    endif()

    find_package(Git REQUIRED)
    set(_work "${out_dir}-work")
    file(REMOVE_RECURSE "${_work}")
    file(MAKE_DIRECTORY "${_work}")
    file(WRITE "${_work}/httplib.h" "${_pristine}")
    _arn_core_read_lf("${ARN_CORE_HTTPLIB_PATCH}" _patch)
    file(WRITE "${_work}/arn-interrupt.patch" "${_patch}")
    # The build tree may sit inside a git checkout (for example build-*/ in
    # arn-core). Stop git from discovering it, so `git apply` acts like plain
    # `patch` on the work directory. Line-ending conversion is turned off; the
    # result is checked as LF text below in any case.
    get_filename_component(_ceiling "${_work}" DIRECTORY)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf
                apply --whitespace=nowarn arn-interrupt.patch
        WORKING_DIRECTORY "${_work}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output)
    if (NOT _result EQUAL 0)
        message(FATAL_ERROR "arn-core: applying ${ARN_CORE_HTTPLIB_PATCH} failed:\n${_output}")
    endif()
    _arn_core_read_lf("${_work}/httplib.h" _patched)
    string(SHA256 _patched_hash "${_patched}")
    if (NOT _patched_hash STREQUAL ARN_CORE_HTTPLIB_PATCHED_SHA256)
        message(FATAL_ERROR
            "arn-core: patched httplib.h has SHA-256 ${_patched_hash}, expected "
            "${ARN_CORE_HTTPLIB_PATCHED_SHA256}.")
    endif()
    file(MAKE_DIRECTORY "${out_dir}")
    file(WRITE "${_header}.tmp" "${_patched}")
    file(RENAME "${_header}.tmp" "${_header}")
    file(REMOVE_RECURSE "${_work}")
    message(STATUS "arn-core: generated patched cpp-httplib v${ARN_CORE_HTTPLIB_VERSION} at ${_header}")
endfunction()

function(arn_core_add_httplib)
    foreach(_existing httplib httplib::httplib)
        if (TARGET ${_existing})
            message(FATAL_ERROR
                "arn-core: target '${_existing}' already exists. arn-core must be the only "
                "provider of cpp-httplib because it uses a patched httplib.h "
                "(Request::interrupt); a second copy would give different httplib::Request "
                "layouts in one program. Remove the other cpp-httplib "
                "(FetchContent/find_package) and link httplib::httplib from arn-core instead.")
        endif()
    endforeach()

    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)

    include(FetchContent)
    # A name of its own, so a parent project's `cpp_httplib` declaration can
    # never replace this one.
    FetchContent_Declare(
        arn_core_cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v${ARN_CORE_HTTPLIB_VERSION}
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(arn_core_cpp_httplib)
    if (NOT arn_core_cpp_httplib_POPULATED)
        FetchContent_Populate(arn_core_cpp_httplib)
    endif()

    set(_include_dir "${CMAKE_CURRENT_BINARY_DIR}/arn_core_httplib/include")
    _arn_core_prepare_httplib("${arn_core_cpp_httplib_SOURCE_DIR}" "${_include_dir}")
    # Re-run configure when the patch or the fetched source changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ARN_CORE_HTTPLIB_PATCH}" "${arn_core_cpp_httplib_SOURCE_DIR}/httplib.h")

    add_library(arn_core_httplib INTERFACE)
    set_target_properties(arn_core_httplib PROPERTIES EXPORT_NAME httplib)
    target_include_directories(arn_core_httplib INTERFACE
        $<BUILD_INTERFACE:${_include_dir}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/arn/core/third_party/cpp-httplib>
    )
    target_compile_definitions(arn_core_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(arn_core_httplib INTERFACE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:crypt32>
    )
    add_library(arn::httplib ALIAS arn_core_httplib)
    add_library(httplib::httplib ALIAS arn_core_httplib)

    set(ARN_CORE_HTTPLIB_HEADER "${_include_dir}/httplib.h" PARENT_SCOPE)
endfunction()
