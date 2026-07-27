if(NOT ANDROID OR NOT BUILD_CLIENT OR NOT USE_GL4ES)
    return()
endif()

# gl4es translates the fixed function OpenGL the original renderer speaks into
# the OpenGL ES the device actually has.
#
# This is here because renderergl1 is the renderer this game's art was authored
# for. renderergl2 is the rend2 rewrite: every surface goes through a GLSL
# program with dozens of uniforms set on it, once per surface per eye, and the
# frame timing found tens of milliseconds of CPU going into that while the GPU
# finished in three. None of what it buys is visible on content that has no
# normal or specular maps for it to read - so it costs the frame and returns a
# picture further from the original than the plain path gives.
#
# The alternative was to port renderergl1's fixed function calls to ES by hand.
# That is the same work gl4es already does, more thoroughly, and it is the same
# choice RTCWQuest made for the same reason.

include(FetchContent)

set(GL4ES_VERSION 1.1.6)

set(GL4ES_SOURCE_PATH "" CACHE PATH
    "Existing gl4es source tree to build against; downloaded if empty")

if(GL4ES_SOURCE_PATH)
    FetchContent_Declare(gl4es SOURCE_DIR ${GL4ES_SOURCE_PATH})
else()
    FetchContent_Declare(gl4es
        GIT_REPOSITORY https://github.com/ptitSeb/gl4es.git
        GIT_TAG v${GL4ES_VERSION}
        GIT_SHALLOW TRUE)
endif()

# No X11 and no GBM on Android, and ES 2 as the backend. NO_INIT_CONSTRUCTOR
# keeps it from initialising itself behind the engine's back: the context is
# created by SDL and handed to OpenXR, and gl4es has to attach to that one
# rather than make its own. These are the flags RTCWQuest ships.
set(NOX11 ON CACHE BOOL "" FORCE)
set(GBM OFF CACHE BOOL "" FORCE)
set(STATICLIB OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(gl4es)

# The target is called GL, because that is what gl4es presents itself as - it
# takes over the OpenGL entry points the renderer calls. Named back to gl4es on
# disk so the APK does not ship something calling itself libGL.so, which is a
# name the platform has its own ideas about.
if(NOT TARGET GL)
    message(FATAL_ERROR
        "gl4es did not declare its GL target; nothing would translate the GL1 "
        "renderer's calls. Configure with -DUSE_GL4ES=OFF to build without it.")
endif()

target_compile_definitions(GL PRIVATE
    NOX11
    NO_GBM
    DEFAULT_ES=2
    NO_INIT_CONSTRUCTOR)

set_target_properties(GL PROPERTIES
    # Shipped inside the APK, so it needs the prefix the top level CMakeLists
    # cleared for the engine's own libraries.
    OUTPUT_NAME gl4es
    PREFIX ${ANDROID_APK_LIBRARY_PREFIX}

    # gl4es builds itself as libGL.so.1 in the manner of a system OpenGL
    # (src/CMakeLists.txt forces the suffix). Android has no versioned sonames:
    # the package manager only extracts lib*.so out of an APK, and the linker
    # would not load the result if it did. Back to a plain suffix.
    SUFFIX ".so"

    # gl4es's own CMakeLists points CMAKE_LIBRARY_OUTPUT_DIRECTORY at
    # ${CMAKE_SOURCE_DIR}/lib, and under FetchContent that is *this* project's
    # source root - so without this the artifact is written into the engine's
    # source tree, where nobody thinks to look for it and git has to ignore it.
    LIBRARY_OUTPUT_DIRECTORY ${gl4es_BINARY_DIR}/lib)

# The renderer includes <GL/gl.h>, which on Android only exists because gl4es
# provides it.
list(APPEND RENDERER_INCLUDE_DIRS ${gl4es_SOURCE_DIR}/include)
list(APPEND RENDERER_LIBRARIES GL)
list(APPEND RENDERER_DEFINITIONS USE_GL4ES)
