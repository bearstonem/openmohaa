if(NOT BUILD_CLIENT)
    return()
endif()

if(EMSCRIPTEN)
    # Emscripten provides its own self contained SDL setup
    list(APPEND CLIENT_COMPILE_OPTIONS -sUSE_SDL=2)
    list(APPEND CLIENT_LINK_OPTIONS -sUSE_SDL=2)
    return()
endif()

set(INTERNAL_SDL_VERSION 2.32.8)
set(INTERNAL_SDL_DIR ${SOURCE_DIR}/thirdparty/SDL2-${INTERNAL_SDL_VERSION})

include(utils/arch)

if(ANDROID)
    # There is no prebuilt SDL for Android, so it is built from source next to
    # the engine. The same tree also carries the org.libsdl.app Java classes
    # the APK needs, so the unpacked source is worth keeping hold of.
    include(FetchContent)

    set(SDL2_SOURCE_PATH "" CACHE PATH
        "Existing SDL2 source tree to build against; downloaded if empty")

    if(SDL2_SOURCE_PATH)
        FetchContent_Declare(SDL2 SOURCE_DIR ${SDL2_SOURCE_PATH})
    else()
        FetchContent_Declare(SDL2
            URL https://github.com/libsdl-org/SDL/releases/download/release-${INTERNAL_SDL_VERSION}/SDL2-${INTERNAL_SDL_VERSION}.tar.gz
            URL_HASH SHA256=0ca83e9c9b31e18288c7ec811108e58bac1f1bb5ec6577ad386830eac51c787e)
    endif()

    # SDLActivity loads libSDL2.so from the APK by name, so it has to be shared.
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(SDL2)

    # The top level CMakeLists clears the shared library prefix, which the
    # fetched project inherits. Put it back, or the APK ships an SDL2.so the
    # package manager will not extract and SDLActivity cannot load.
    set_target_properties(SDL2 PROPERTIES PREFIX ${ANDROID_APK_LIBRARY_PREFIX})

    # Where the APK build picks up SDLActivity and friends
    set(SDL2_JAVA_DIR ${sdl2_SOURCE_DIR}/android-project/app/src/main/java
        CACHE INTERNAL "")

    list(APPEND CLIENT_LIBRARIES SDL2::SDL2)
    list(APPEND RENDERER_LIBRARIES SDL2::SDL2)
    return()
endif()

if(WIN32 OR APPLE)
    # On Windows and macOS we have internal SDL binaries we can use
    set(HAVE_INTERNAL_SDL true)
endif()

if(USE_INTERNAL_SDL AND HAVE_INTERNAL_SDL)
    set(SDL2_INCLUDE_DIRS ${INTERNAL_SDL_DIR}/include)
    list(APPEND CLIENT_DEFINITIONS USE_INTERNAL_SDL_HEADERS)
    list(APPEND RENDERER_DEFINITIONS USE_INTERNAL_SDL_HEADERS)

    if(WIN32)
        if(ARCH STREQUAL "x86_64")
            set(LIB_DIR ${SOURCE_DIR}/thirdparty/libs/win64)
        elseif(ARCH STREQUAL "x86")
            set(LIB_DIR ${SOURCE_DIR}/thirdparty/libs/win32)
        else()
            message(FATAL_ERROR "Unknown ARCH")
        endif()

        if(MINGW)
            set(SDL2_LIBRARIES
                ${LIB_DIR}/libSDL2main.a
                ${LIB_DIR}/libSDL2.dll.a)
        elseif(MSVC)
            set(SDL2_LIBRARIES
                ${LIB_DIR}/SDL2main.lib
                ${LIB_DIR}/SDL2.lib)
        endif()

        list(APPEND CLIENT_DEPLOY_LIBRARIES ${LIB_DIR}/SDL2.dll)
    elseif(APPLE)
        set(SDL2_LIBRARIES
            ${SOURCE_DIR}/thirdparty/libs/macos/libSDL2main.a
            ${SOURCE_DIR}/thirdparty/libs/macos/libSDL2-2.0.0.dylib)
        list(APPEND CLIENT_DEPLOY_LIBRARIES
            ${SOURCE_DIR}/thirdparty/libs/macos/libSDL2-2.0.0.dylib)
    else()
        message(FATAL_ERROR "HAVE_INTERNAL_SDL set incorrectly; file a bug")
    endif()
else()
    find_package(SDL2 REQUIRED)
endif()

list(APPEND CLIENT_LIBRARIES ${SDL2_LIBRARIES})
list(APPEND CLIENT_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
list(APPEND CLIENT_COMPILE_OPTIONS ${SDL2_CFLAGS_OTHER})
list(APPEND RENDERER_LIBRARIES ${SDL2_LIBRARIES})
list(APPEND RENDERER_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
list(APPEND RENDERER_COMPILE_OPTIONS ${SDL2_CFLAGS_OTHER})
