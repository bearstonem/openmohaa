# Building for Android

This covers building OpenMoHAA as an Android APK. It targets `arm64-v8a` and
was brought up against a Meta Quest 3, but nothing here is headset specific —
a phone or tablet builds the same way.

VR itself is not part of this. What you get is the ordinary game running as a
normal Android app.

## What you need

| | |
|---|---|
| Android NDK | r26 or later |
| Android SDK | platform 34, build-tools 34 |
| JDK | 17 (required by the Android Gradle Plugin) |
| Gradle | 8.0 or later |
| CMake | 3.25 or later |
| flex, bison | on the host, to generate the script parser |

Set `ANDROID_NDK_HOME` and `ANDROID_HOME` before you start.

## 1. Build the engine

Gradle does not drive the native build; CMake does, and gradle only packages
the result. Configure with the NDK toolchain file:

```sh
cmake -B build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF

cmake --build build-android-arm64 -j$(nproc)
```

`ANDROID_STL=c++_shared` matters: the APK ships several libraries that use the
C++ standard library, and they have to share one copy of it.

SDL2 and OpenAL Soft have no Android binaries to fall back on, so both are
downloaded and built from source. To build against trees you already have,
pass `-DSDL2_SOURCE_PATH=...` and `-DOPENAL_SOURCE_PATH=...`.

Several defaults change on Android and are forced in
`cmake/platforms/android.cmake`:

- **`renderergl1` is not built.** It is fixed function and would need a
  GL-over-GLES translation layer. `renderergl2` speaks GLES directly.
- **`USE_RENDERER_DLOPEN=OFF`** — the renderer is linked into the client
  rather than loaded from a path the Android linker may object to.
- **`USE_HTTP=OFF`** — only used for downloading content from a server.
- **`BUILD_GAME_QVMS=OFF`** — the QVM toolchain is not part of this build.

The build stages everything the APK needs into
`build-android-arm64/apk-libs/arm64-v8a/`.

## 2. Build the APK

```sh
cd misc/android
gradle assembleDebug
```

The result is `misc/android/app/build/outputs/apk/debug/app-debug.apk`.

If your build tree is somewhere else, point gradle at it:

```sh
gradle assembleDebug -PnativeLibsDir=/path/to/build/apk-libs
```

Gradle picks up SDL's `org.libsdl.app` Java classes from the SDL tree CMake
built against — it reads the path out of `android-build.properties` in the
build directory. They have to match the `libSDL2.so` that was linked, so do
not point them at a different SDL.

## 3. Install and add the game data

```sh
adb install -r misc/android/app/build/outputs/apk/debug/app-debug.apk
```

No game data ships with the app. It goes in the app's own external files
directory, which is also where the engine writes its config and saves:

```
/sdcard/Android/data/org.openmoh.openmohaa/files/
```

Copy your retail `main/` directory there:

```sh
adb shell mkdir -p /sdcard/Android/data/org.openmoh.openmohaa/files/main
adb push main/pak0.pk3 /sdcard/Android/data/org.openmoh.openmohaa/files/main/
```

### Directory permissions

`adb push` **cannot create directories** under an app's data directory — it
fails with `secure_mkdirs failed: Operation not permitted` partway through the
transfer. Create them first, in one go:

```sh
cd /path/to/mohaa/main
adb shell mkdir -p $(find . -type d | sed 's|^\./|/sdcard/Android/data/org.openmoh.openmohaa/files/main/|' | tr '\n' ' ')
adb push --sync . /sdcard/Android/data/org.openmoh.openmohaa/files/main/
adb shell chmod -R 775 /sdcard/Android/data/org.openmoh.openmohaa/files
```

`chmod` has to come *after* `adb push`, or push fails with
`remote fchown failed`.

**Only push directories the game reads.** A directory created by
`adb shell mkdir` belongs to `shell`, not to the app, and no amount of
chmod'ing changes that — 775 lets the app *read* through it, but the app is
`other` there and still cannot *write*. Pre-creating `main/configs` or
`main/save` therefore leaves the game unable to save its config or your
progress, reporting only `Couldn't write omconfig.cfg`. Leave those two out
and the engine creates them itself, owned by the app and writable:

```sh
adb shell rm -rf /sdcard/Android/data/org.openmoh.openmohaa/files/main/configs \
                 /sdcard/Android/data/org.openmoh.openmohaa/files/main/save
```

The failure mode to watch for is silence: files pushed by adb are 644 and
world readable, so game data loads perfectly while writes fail, which reads
as "the data is fine".

## 4. Run it

Launch it from the device. Do not use `adb shell am start`; on a Quest it
intermittently strands the headset in Meta's loading environment.

## Logging

Everything the engine prints goes to logcat under the `openmohaa` tag:

```sh
adb logcat -s openmohaa
```

`stdout` and `stderr` are redirected there too, so output from the C library
and from third party code is not lost.

A crash reports its own stack: bionic has no `<execinfo.h>`, so the backtrace
is walked with `_Unwind_Backtrace` and resolved through `dladdr`
(`code/sys/new/sys_android_new.c`). This matters because `tombstoned` does not
reliably put a backtrace in logcat, and when it does not, that handler is the
only account of where the engine died. When even that is silent,
`adb shell dumpsys activity exit-info` still reports the signal.

Verify that logging works *before* you rely on it. A crash handler that has
been quietly displaced reports nothing, which is indistinguishable from a
crash that never happened.

## How it fits together

`SDLActivity` loads `libSDL2.so` and `libopenmohaa.so`, then calls `SDL_main`
in the latter — which is the engine's `main`, renamed by `SDL_main.h`. The
engine then `dlopen`s the game modules by name.

Those modules ship inside the APK as `libgame.so` and `libcgame.so` rather
than beside the game data. The prefix is not cosmetic: Android extracts only
`lib*.so` out of an APK, and having them in the native library directory means
the loader finds them by name, with no path to get wrong. `DLL_PREFIX` in
`q_platform.h` is what keeps the engine's name for them in step.

| Library | Loaded by |
|---|---|
| `libSDL2.so` | `SDLActivity`, by name |
| `libopenmohaa.so` | `SDLActivity`, then `SDL_main` is called in it |
| `libopenal.so`, `libc++_shared.so` | the linker, as dependencies |
| `libgame.so`, `libcgame.so` | the engine, at runtime |

## Dedicated server

The server builds too, and needs neither SDL nor a renderer:

```sh
cmake -B build-android-server \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF
```

It produces an ordinary `omohaaded` executable rather than an APK.
