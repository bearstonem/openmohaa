if(NOT USE_OPENAL)
    return()
endif()

if(NOT BUILD_CLIENT)
    return()
endif()

set(INTERNAL_OPENAL_VERSION 1.24.3)
set(INTERNAL_OPENAL_DIR ${SOURCE_DIR}/thirdparty/openal-soft-${INTERNAL_OPENAL_VERSION})

if(ANDROID)
    # There is no system OpenAL on Android, and OpenAL is not optional here:
    # it is the engine's only sound driver (see SOUND_DRIVER in
    # snd_local_new.h). Build OpenAL Soft from source; its OpenSL ES backend is
    # what actually reaches the headset.
    include(FetchContent)

    set(OPENAL_SOURCE_PATH "" CACHE PATH
        "Existing OpenAL Soft source tree to build against; downloaded if empty")

    if(OPENAL_SOURCE_PATH)
        FetchContent_Declare(OpenAL SOURCE_DIR ${OPENAL_SOURCE_PATH})
    else()
        FetchContent_Declare(OpenAL
            URL https://github.com/kcat/openal-soft/archive/refs/tags/${INTERNAL_OPENAL_VERSION}.tar.gz
            URL_HASH SHA256=7e1fecdeb45e7f78722b776c5cf30bd33934b961d7fd2a11e0494e064cc631ce)
    endif()

    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(OpenAL)

    # See the same note in sdl.cmake: the cleared prefix is inherited from the
    # top level and has to be put back for anything shipped inside the APK.
    set_target_properties(OpenAL PROPERTIES PREFIX ${ANDROID_APK_LIBRARY_PREFIX})

    # OpenAL::OpenAL carries its own include directory, so <AL/al.h> resolves
    # without the vendored copy.
    list(APPEND CLIENT_DEFINITIONS USE_OPENAL)
    list(APPEND CLIENT_LIBRARIES OpenAL::OpenAL)
    return()
endif()

find_package(OpenAL QUIET)

if(NOT OpenAL_FOUND)
    set(OPENAL_DEFINITIONS USE_INTERNAL_OPENAL_HEADERS)
    set(OPENAL_INCLUDE_DIR ${INTERNAL_OPENAL_DIR}/include)
    set(OPENAL_LIBRARY openal)
endif()

list(APPEND CLIENT_DEFINITIONS ${OPENAL_DEFINITIONS} USE_OPENAL)
list(APPEND CLIENT_INCLUDE_DIRS ${OPENAL_INCLUDE_DIR})

if(USE_OPENAL_DLOPEN)
    list(APPEND CLIENT_DEFINITIONS USE_OPENAL_DLOPEN)
else()
    find_package(Threads REQUIRED)
    list(APPEND CLIENT_LIBRARIES Threads::Threads ${OPENAL_LIBRARY})
endif()
