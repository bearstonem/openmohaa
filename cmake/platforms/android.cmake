# Android specific settings.
#
# Configured with the NDK's android.toolchain.cmake, e.g.
#
#   cmake -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#
# Nothing here is Quest specific; a phone build works the same way.

if(NOT ANDROID)
    return()
endif()

# renderergl1 is fixed function and would need a GL-over-GLES translation
# layer underneath it. renderergl2 already speaks GLES, so it is the only
# renderer worth building here.
set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL "")
set(BUILD_RENDERER_GL2 ON CACHE INTERNAL "")

# Android's linker namespaces are particular about where a dlopen'd library
# may live. Link the renderer into the client rather than fight them.
set(USE_RENDERER_DLOPEN OFF CACHE INTERNAL "")
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")

# The QVM toolchain is not part of an Android build.
set(BUILD_GAME_QVMS OFF CACHE INTERNAL "")

# Only for downloading content off a server; not needed to get the game up.
set(USE_HTTP OFF CACHE INTERNAL "")

# The whole point of the Android build here is the headset.
set(USE_OPENXR ON CACHE INTERNAL "")

# There is no bundled SDL binary for Android; it is built from source.
set(USE_INTERNAL_SDL OFF CACHE INTERNAL "")

# No shell to attach a console to, and no desktop to install into.
set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "" FORCE)

# Everything the engine prints ends up in logcat.
list(APPEND COMMON_LIBRARIES log)

# Android extracts only lib*.so out of an APK, so everything shipped inside one
# needs the prefix - but the top level CMakeLists clears it for the whole
# project. Targets bound for the APK opt back in individually with this.
set(ANDROID_APK_LIBRARY_PREFIX "lib")

# Gradle wants every native library for an ABI in one directory, so gather
# them there once everything has been declared.
list(APPEND POST_CONFIGURE_FUNCTIONS android_stage_apk_libraries)

function(android_stage_apk_libraries)
    set(APK_LIB_DIR ${CMAKE_BINARY_DIR}/apk-libs/${ANDROID_ABI})

    # Several of our libraries use the STL, so it is the shared one and has to
    # be shipped alongside them.
    set(APK_LIBRARIES ${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/libc++_shared.so)
    set(APK_TARGETS)

    foreach(APK_TARGET IN ITEMS ${CLIENT_NAME} SDL2 OpenAL openxr_loader ${CGAME_MODULE} ${GAME_MODULE})
        if(TARGET ${APK_TARGET})
            list(APPEND APK_TARGETS ${APK_TARGET})
            list(APPEND APK_LIBRARIES $<TARGET_FILE:${APK_TARGET}>)
        endif()
    endforeach()

    if(NOT APK_TARGETS)
        return()
    endif()

    add_custom_target(apk-libs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APK_LIB_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${APK_LIBRARIES} ${APK_LIB_DIR}
        COMMENT "Staging APK libraries in ${APK_LIB_DIR}"
        VERBATIM)

    add_dependencies(apk-libs ${APK_TARGETS})

    # The APK needs the org.libsdl.app Java classes belonging to the very SDL
    # the engine was linked against, so the gradle build reads the path from
    # here rather than guessing.
    file(WRITE ${CMAKE_BINARY_DIR}/android-build.properties
        "# Written by the CMake build; consumed by misc/android\n"
        "sdlJavaDir=${SDL2_JAVA_DIR}\n")
endfunction()
