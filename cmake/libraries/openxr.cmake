if(NOT USE_OPENXR OR NOT BUILD_CLIENT)
    return()
endif()

# The Khronos loader is the vendor-neutral one: it finds whatever runtime the
# device provides (Meta, Pico, SteamVR) rather than binding us to a vendor SDK.
include(FetchContent)

set(OPENXR_VERSION 1.1.43)

set(OPENXR_SOURCE_PATH "" CACHE PATH
    "Existing OpenXR-SDK source tree to build against; downloaded if empty")

if(OPENXR_SOURCE_PATH)
    FetchContent_Declare(OpenXR SOURCE_DIR ${OPENXR_SOURCE_PATH})
else()
    FetchContent_Declare(OpenXR
        URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${OPENXR_VERSION}.tar.gz
        URL_HASH SHA256=fd0834e4bc75c935248d82ac044f472ab3af4d8ff3b191a3ccace3436975a4cf)
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(OpenXR)

if(ANDROID)
    # Shipped in the APK, so it needs the prefix the top level CMakeLists cleared
    set_target_properties(openxr_loader PROPERTIES PREFIX ${ANDROID_APK_LIBRARY_PREFIX})
endif()

list(APPEND CLIENT_DEFINITIONS USE_OPENXR)
list(APPEND CLIENT_LIBRARIES OpenXR::openxr_loader)
list(APPEND CLIENT_LIBRARY_SOURCES ${SOURCE_DIR}/vr/vr_openxr.c)

if(ANDROID)
    # The VR layer owns the eye framebuffers and hands the session its EGL
    # context, so unlike the renderer it calls GL and EGL directly rather than
    # through SDL's loader.
    list(APPEND CLIENT_LIBRARIES EGL GLESv3)
endif()

# The renderer needs to know which framebuffer to draw an eye into
list(APPEND RENDERER_DEFINITIONS USE_OPENXR)
