if(NOT DEFINED RECOMP_UI_ROOT)
    message(FATAL_ERROR "RECOMP_UI_ROOT is required")
endif()

set(SOURCE_PATH
    "${RECOMP_UI_ROOT}/src/common/backends/imgui/launcher_imgui.cpp")
file(READ "${SOURCE_PATH}" SOURCE_TEXT)

function(require_between TOKEN BEGIN_MARKER END_MARKER LABEL)
    string(FIND "${SOURCE_TEXT}" "${BEGIN_MARKER}" BEGIN_POS)
    string(FIND "${SOURCE_TEXT}" "${END_MARKER}" END_POS)
    string(FIND "${SOURCE_TEXT}" "${TOKEN}" TOKEN_POS)
    if(BEGIN_POS EQUAL -1 OR END_POS EQUAL -1)
        message(FATAL_ERROR "Missing ${LABEL} setup-copy markers")
    endif()
    if(TOKEN_POS EQUAL -1 OR TOKEN_POS LESS BEGIN_POS OR TOKEN_POS GREATER END_POS)
        message(FATAL_ERROR "${LABEL} setup copy is missing: ${TOKEN}")
    endif()
endfunction()

set(WINDOWS_BEGIN "/* SETUP_TOOLCHAIN_WINDOWS_BEGIN */")
set(WINDOWS_END "/* SETUP_TOOLCHAIN_WINDOWS_END */")
require_between("Download latest portable toolchain##tc"
                "${WINDOWS_BEGIN}" "${WINDOWS_END}" "Windows")
require_between("cmake-clang-v1-*.zip"
                "${WINDOWS_BEGIN}" "${WINDOWS_END}" "Windows")

set(POSIX_BEGIN "/* SETUP_TOOLCHAIN_POSIX_BEGIN */")
set(POSIX_END "/* SETUP_TOOLCHAIN_POSIX_END */")
require_between("1. Native build tools"
                "${POSIX_BEGIN}" "${POSIX_END}" "POSIX")
require_between("Install CMake, Ninja, Python 3, and either Clang or GCC"
                "${POSIX_BEGIN}" "${POSIX_END}" "POSIX")
require_between("Check tools."
                "${POSIX_BEGIN}" "${POSIX_END}" "POSIX")

message(STATUS "Platform-specific setup toolchain copy: PASS")
