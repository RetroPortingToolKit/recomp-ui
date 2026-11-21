# System OpenGL discovery for recomp-ui consumers.
#
# Why this is not just `find_package(OpenGL REQUIRED)` + `OpenGL::GL`:
#
# CMake's FindOpenGL defines the legacy `OpenGL::GL` target only when EITHER a
# legacy libGL was found (`OPENGL_gl_LIBRARY`), OR BOTH GLVND targets exist —
# and `OpenGL::GLX` itself needs `OPENGL_glx_LIBRARY` AND a `GL/glx.h` include
# dir. A GLVND host that ships libOpenGL.so but no legacy libGL.so link and/or
# no glx.h therefore reports OpenGL as *found* while `OpenGL::GL` is never
# created, so a hardcoded link to it dies at generate time with
#
#   Target "psx-runtime" links to: OpenGL::GL but the target was not found.
#
# That is exactly the Steam Deck / SteamOS failure (`Found OpenGL:
# /usr/lib/libOpenGL.so`, no OpenGL::GL). REQUIRED does not catch it, because
# the package genuinely was found.
#
# libOpenGL (GLVND) is a complete substitute for our use: SDL creates the
# context and modern entry points come from SDL_GL_GetProcAddress, so we only
# need the core GL symbols to resolve at link time. Prefer OpenGL::GL when it
# exists (keeps the legacy/mesa-dev path byte-identical), fall back to GLVND,
# then to the raw library paths, and only then fail — with a message that says
# what to install rather than "target was not found".
#
# Keep this file free of project()/enable_language() calls; it is included from
# both recomp_ui.cmake and the standalone CMakeLists.txt.

# Included from recomp_ui.cmake, the standalone CMakeLists.txt and psxrecomp's
# runtime.cmake — all three can appear in one tree, so guard the definition.
include_guard(GLOBAL)

function(recomp_resolve_gl out_var)
    if(WIN32)
        set(${out_var} opengl32 PARENT_SCOPE)
        return()
    endif()

    find_package(OpenGL REQUIRED)

    if(TARGET OpenGL::GL)
        set(${out_var} OpenGL::GL PARENT_SCOPE)
    elseif(TARGET OpenGL::OpenGL)
        message(STATUS
            "recomp-ui: OpenGL::GL unavailable (GLVND host without legacy libGL/glx.h)"
            " — linking OpenGL::OpenGL")
        set(${out_var} OpenGL::OpenGL PARENT_SCOPE)
    elseif(OPENGL_gl_LIBRARY)
        message(STATUS "recomp-ui: no OpenGL:: imported target — linking ${OPENGL_gl_LIBRARY}")
        set(${out_var} "${OPENGL_gl_LIBRARY}" PARENT_SCOPE)
    elseif(OPENGL_opengl_LIBRARY)
        message(STATUS "recomp-ui: no OpenGL:: imported target — linking ${OPENGL_opengl_LIBRARY}")
        set(${out_var} "${OPENGL_opengl_LIBRARY}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "recomp-ui: OpenGL was found but no usable GL library resolved.\n"
            "  OPENGL_gl_LIBRARY     = ${OPENGL_gl_LIBRARY}\n"
            "  OPENGL_opengl_LIBRARY = ${OPENGL_opengl_LIBRARY}\n"
            "  OPENGL_glx_LIBRARY    = ${OPENGL_glx_LIBRARY}\n"
            "  OPENGL_INCLUDE_DIR    = ${OPENGL_INCLUDE_DIR}\n"
            "Install the GL development files (Debian/Ubuntu: libgl-dev or "
            "libglvnd-dev; Arch/SteamOS: libglvnd), or point CMAKE_PREFIX_PATH "
            "at a sysroot that has them.")
    endif()
endfunction()
