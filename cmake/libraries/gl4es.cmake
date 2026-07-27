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

# Built the way RTCWQuest builds it, because that combination is known to render
# on this device and this one does not.
#
# Their Android.mk (SupportLibs/gl4es/Android.mk) compiles with exactly:
#
#     -DBCMHOST -DNOX11 -DNO_GBM -DDEFAULT_ES=2 -DNO_INIT_CONSTRUCTOR
#     -O3 -fcommon -fvisibility=hidden -funwind-tables
#
# Two differences from what CMake produces on its own are worth naming.
#
# ANDROID is *not* among them. gl4es's own CMakeLists adds it whenever it is
# built with the NDK toolchain, and it is not a harmless label: it selects
# different code in the loader, in glx, and in hardext - which is where gl4es
# compiles a probe shader to decide what its whole fixed function pipeline is
# allowed to emit. ndk-build never defines it, so the reference has been
# exercised without it and this has not.
#
# BCMHOST is the Raspberry Pi switch, which looks wrong on a Quest and is
# carried anyway, because everything it gates is additionally guarded by
# !defined(ANDROID). Matching the reference matters more here than tidiness.
#
# add_definitions() in gl4es's CMakeLists is a directory property, so the
# ANDROID it adds has to be removed at the directory it was set on rather than
# overridden on the target.
foreach(GL4ES_DIR ${gl4es_SOURCE_DIR} ${gl4es_SOURCE_DIR}/src)
    set_property(DIRECTORY ${GL4ES_DIR} PROPERTY COMPILE_DEFINITIONS
        BCMHOST
        NOX11
        NO_GBM
        DEFAULT_ES=2
        NO_INIT_CONSTRUCTOR)
endforeach()

target_compile_options(GL PRIVATE
    -O3
    # gl4es is old C that expects tentative definitions to be merged. Clang has
    # defaulted to -fno-common since 15; the NDK here is newer than that and the
    # reference build is not.
    -fcommon
    -fvisibility=hidden
    -funwind-tables)

# What their LOCAL_LDLIBS names. gl4es dlopen()s the driver itself, so these are
# not needed to resolve anything; they are here so the library is built against
# the same set the reference links.
target_link_libraries(GL PRIVATE EGL GLESv3 dl log)

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
