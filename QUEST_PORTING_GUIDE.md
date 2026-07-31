# Porting an old engine to Android arm64 and Quest 3, natively, on OpenXR

A working guide, written after three ports rather than before them. It is aimed
at Quake-lineage engines (id Tech 2/3 and everything descended from them —
ioquake3, RTCW, MOH:AA, Jedi Knight, Quake II/III forks), because that is the
family where the advice is most exact. Most of sections 3–10 transfer to any C
or C++ engine of that era.

Everything here is either **measured on a Quest 3** or **read out of source and
verified**. Where something is believed but unproven it says so. The point of
the document is that none of it should have to be discovered twice.

## The three ports it comes from

| Port | Engine | Graphics path | Build | State |
|---|---|---|---|---|
| **OpenMoHAA** (this tree) | id Tech 3 / MOH:AA, SDL2 | `renderergl1` (fixed function) on gl4es, ES 3.0 | CMake | Stereo VR, 90 fps, in-hand weapon |
| **Homeworld: Unbound** | Homeworld 1999 SDL, SDL2 | GL 1.x on gl4es, ES 3.0 | meson | Shipped; full VR interaction layer |
| **openblack VR** | openblack (modern C++), bgfx | bgfx GLES backend, no gl4es | CMake + vcpkg | Stereo + VR input; game itself incomplete upstream |

And one reference port that is *finished* and that all three lean on:

**Team Beef's RTCWQuest** — `github.com/Team-Beef-Studios/RTCWQuest`, cloned
locally at `/home/berkybear/RTCWQuest` (clean at `4c714b9`). Same Quake 3
lineage, same headset, same gl4es. `Projects/Android/jni/RTCWVR/TBXR_Common.c`
is a reusable OpenXR layer; `VrInputDefault.c` is movement and weapon handling;
`VrInputWeaponAlign.c` is per-weapon aim calibration.

---

## 0. Read the finished reference port first, every time

This is the single highest-value instruction in the document, and it is the one
most often skipped.

On the OpenMoHAA renderer swap, the answer to the largest blocker — a gl4es that
could not draw a triangle — was sitting in RTCWQuest's `Android.mk` the entire
time (§8.5). Days went into engine-side experiments that could not have worked.
The standing rule that came out of it:

> **For any problem the port is trying to solve, go look at how the reference
> solved it before solving it yourself.** Not as a suggestion — as the first
> step.

The corollary is worth stating too: the reference is not always right, and is
sometimes not even *live*. RTCWQuest computes `controllerYawHeading` and never
reads it; its `XR_FB_display_refresh_rate` entry points are declared, nulled and
never resolved, and `TBXR_GetRefreshRate` returns a hardcoded 90. Read what is
*called*, not what is *present*.

---

## 1. Decisions to take before writing any code

Each of these shapes everything after it, and each was got wrong at least once.

### 1.1 Which renderer — and do not assume the modern one

If the engine has both a fixed-function renderer and a shader rewrite (ioquake3
ships `renderergl1` and `renderergl2`/rend2), the instinct is to take the modern
one because it speaks GLES natively. **Measure before believing that.**

OpenMoHAA's numbers, taken on the device, per eye:

| Phase | rend2 | renderergl1 + gl4es |
|---|---|---|
| `xrWaitFrame` | 0 ms | — |
| Building the scene | 2–3 ms | — |
| **Issuing the scene to GL** | **50–60 ms** | **4–6 ms** |
| GPU (`glFinish`) | 3 ms | — |
| Result | 10–15 fps | **90 fps** |

The GPU finished in 3 ms and waited. Rendering at 36% of the pixels bought only
23% back, so it was never fill rate — it was CPU in the back end: rend2 puts
every surface through a GLSL program with dozens of uniforms, 139
`GLSL_BindProgram`/`GLSL_SetUniform` call sites in the surface path, once per
surface **per eye**. None of what that buys is visible on 2002 art with no
normal or specular maps.

The heuristic: **content authored for fixed-function hardware should be rendered
by a fixed-function path.** A per-surface shader pipeline is a per-eye tax on a
device with half the CPU budget of a desktop and twice the passes.

### 1.2 gl4es, or port the GL calls by hand

Once you choose the fixed-function renderer on a device that only has GLES, you
need [gl4es](https://github.com/ptitSeb/gl4es) — a `glBegin`/`glMatrixMode`-era
GL implementation on top of ES 2.

The alternative is hand-porting the renderer's fixed-function calls to ES, which
is the same work gl4es already does, more thoroughly. Both OpenMoHAA and
Homeworld made the same choice, as did RTCWQuest.

It is not free. gl4es keeps its own copy of GL state that can silently disagree
with the driver, and that boundary cost several days on OpenMoHAA alone. **§8 is
the whole chapter on it. Read it before you debug anything that looks like "the
geometry is correct and nothing is drawn".**

openblack skipped gl4es entirely because bgfx already has a GLES backend — which
is the other valid answer when the engine is modern enough to have one.

### 1.3 SDL, or a NativeActivity

Two live arrangements:

- **SDL2** (OpenMoHAA, Homeworld). `org.libsdl.app.SDLActivity` loads
  `libSDL2.so` and your `libmain.so`, and calls `SDL_main`. SDL creates the EGL
  context; the VR layer *binds OpenXR to that existing context* rather than
  making its own. One GL context in the process, no thread juggling, and the
  flat desktop path stays intact and testable.
- **NativeActivity + OpenXR directly** (RTCWQuest). No SDL at all. The VR layer
  owns everything.

**Prefer SDL if the engine already uses it.** The cost of dropping SDL is every
input, audio, timing and window path in the engine; the cost of keeping it is
one awkward context handshake, documented in §4.2. But know that SDL introduces
two specific hazards you will meet (§4.2 and §4.9), and that the reference port
does not have them because it has no SDL.

### 1.4 Game modules: `dlopen` or static

Quake-lineage engines load `cgame`/`game` as separate modules. On Android:

- **Static linking is usually not an option.** In OpenMoHAA, `cgame` and `fgame`
  each compile `q_shared`, `q_math` and the script system; linking both into one
  binary collides at every symbol.
- **`dlopen` works, if the modules ship inside the APK with a `lib` prefix.**
  Android extracts only `lib*.so` out of an APK, and putting them in the native
  library directory means the loader finds them **by name**, with no path to get
  wrong. In OpenMoHAA that is `libgame.so` / `libcgame.so`, kept in step with
  the engine's naming by `DLL_PREFIX` in `q_platform.h`.

The generalisation: **anything shipped in an APK must be named `lib*.so`**,
including third-party libraries whose own build system names them otherwise.
gl4es builds `libGL.so.1` by default — a versioned soname Android does not have
and will not extract (§8 and `cmake/libraries/gl4es.cmake`).

### 1.5 One APK or two, and never rename the package id casually

Homeworld ships **one APK carrying two engine builds** (`libmain.so` and
`libmainDemo.so`) because its demo/full switch reaches too far into the engine
to be a runtime flag: it selects the `.big` opened, the music and speech
filenames, and the mission sequence array. A subclassed activity picks the
library in `onCreate` *before* `super`, based on whether the retail data is
present.

Both Homeworld and openblack use gradle **product flavours** (`flat` and `vr`)
sharing one `applicationId`:

- the `vr` manifest adds `android.hardware.vr.headtracking` and the
  `com.oculus.intent.category.VR` launch category, and **removes** the flat
  activity with `tools:node="remove"`. Leaving it in puts a second entry in the
  Quest library that hangs on a document picker nobody can see in a headset.
- one native library can serve both, if the engine attempts OpenXR at startup
  and falls back to flat rendering when there is no runtime.

**`applicationId` is also the asset path under `/sdcard/Android/data/`.**
Renaming it orphans an existing install, its settings and its game data — a
gigabyte the user has to copy back over USB. Homeworld's `install.py` moves the
old directory across on the device instead. If you rename, do the same.

---

## 2. Phase 1 — build for arm64

Goal: an APK that launches, logs, and reaches the main menu. No VR yet.

### 2.1 Toolchain

| | |
|---|---|
| Android NDK | r26 or later |
| Android SDK | platform 34, build-tools 34 |
| JDK | **17** (required by the Android Gradle Plugin) |
| Gradle | 8.0+ |
| CMake | 3.25+ |
| flex, bison | on the host, for generated parsers |

Quest 3 is **Android 14 / API 34**, not 12. Target `arm64-v8a` and
`android-29` or later.

CMake has first-class NDK support; use it rather than a hand-written cross file
if you have the choice:

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

`ANDROID_STL=c++_shared` matters as soon as more than one shipped library uses
the C++ standard library — they have to share one copy, and `libc++_shared.so`
has to be staged into the APK alongside them.

Meson works too (Homeworld) but you write the cross file yourself, and you must
point `pkg-config` at the cross-built SDL prefix or the **host's** SDL leaks
into the cross build.

Two JDK traps, both from openblack:

- `JAVA_HOME` must point at a JDK **with `jlink`**. Debian's
  `java-17-openjdk-amd64` does not have it, and AGP needs it to transform
  `core-for-system-modules.jar`. The failure names `JdkImageTransform`, which
  does not obviously mean "wrong JDK".
- `android/local.properties` with `sdk.dir` is required and is not in git.

### 2.2 Prune the dependency surface early

Every dependency is a cross-compile. Decide before you start:

- **HTTP/curl** — off. It is only for downloading content from a server.
- **QVM toolchain** — off. Not part of an Android build.
- **Dedicated server** — off for the client APK (it builds separately and needs
  neither SDL nor a renderer).
- **Audio** — check whether "just use SDL audio" is actually available before
  planning around it. In OpenMoHAA it is not: `snd_local_new.h` hardcodes
  `#define SOUND_DRIVER OPENAL` and `USE_OPENAL=OFF` does not even compile,
  because `qal.h` is included regardless. OpenAL Soft cross-compiles fine and
  uses its OpenSL ES backend.
- **Codecs** — check what the retail data actually ships before building all of
  vorbis, opus and mad.

Fetch what has no Android binary (SDL2, OpenAL Soft, gl4es, the OpenXR loader)
from source via `FetchContent`, with a `*_SOURCE_PATH` cache variable so a local
tree can be substituted for debugging.

### 2.3 Do not force platform defaults into the cache

In `cmake/platforms/android.cmake`, this line cost a day:

```cmake
set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL "")
```

**`CACHE INTERNAL` implies `FORCE`.** Every `-DBUILD_RENDERER_GL1=ON` on the
command line was silently overwritten on each configure; the cache read
`BUILD_RENDERER_GL1:INTERNAL=OFF` and `build.ninja` never mentioned the renderer
at all. A previous handoff claimed that renderer "compiles clean" — it had never
been compiled once.

> **Check the cache, not the command line.** `grep` the option out of
> `CMakeCache.txt` and confirm the target is in the build graph before believing
> that you built what you asked for.

### 2.4 Stage the native libraries, and write down what gradle needs

Gradle should not drive the native build. Let CMake build everything and stage
it into one directory per ABI, then have gradle package whatever is there:

```cmake
foreach(APK_TARGET IN ITEMS ${CLIENT_NAME} SDL2 OpenAL openxr_loader GL
                            ${CGAME_MODULE} ${GAME_MODULE})
    ...copy $<TARGET_FILE:${APK_TARGET}> into ${CMAKE_BINARY_DIR}/apk-libs/${ANDROID_ABI}
endforeach()
```

Make the directory overridable (`gradle assembleDebug -PnativeLibsDir=...`) so
several build trees can be packaged without editing anything.

**The APK's `org.libsdl.app` Java classes must come from the same SDL tree that
built the `libSDL2.so` you linked.** Have CMake write the path into a properties
file the gradle build reads, rather than letting anyone guess.

### 2.5 Logging and crash reporting, before you need them

Route everything the engine prints to logcat under one tag, and redirect
`stdout`/`stderr` there too so C library and third-party output is not lost:

```sh
adb logcat -s openmohaa
```

Homeworld's tag is `SDL/APP` (not `SDL`), because it logs via `SDL_Log`:

```sh
adb logcat -v time "SDL/APP:V" VrApi:I "*:S"
```

`VrApi:I` is worth adding to the filter on any Meta headset — it gives a
per-second frame report (`FPS`, `Stale`, `App` ms, `LCnt` layer count), which is
the cheapest way to confirm layers are composing and to catch a regression.

**Write your own crash handler.** bionic has no `<execinfo.h>`, and `tombstoned`
does not reliably put a backtrace in logcat. OpenMoHAA installs a `sigaction`
with `SA_SIGINFO` on a private `sigaltstack` and reports the faulting address
plus `pc`/`lr` (`code/sys/new/sys_android_new.c`). Two details:

- **A real `sigaltstack` is mandatory** or a stack overflow silences the handler.
- **`_Unwind_Backtrace` cannot walk out of the kernel's signal trampoline.**
  Before that was understood, every crash reported four frames of the handler
  and nothing else. Reporting `pc`/`lr` from the `ucontext` is what actually
  places the fault.

When even that is silent, `adb shell dumpsys activity exit-info` still reports
the signal.

> **Verify that logging works before you rely on it.** A crash handler that has
> been quietly displaced reports nothing, which is indistinguishable from a crash
> that never happened. Add a startup self-test that exercises the same path.

A release build has symbols but no line numbers, so `addr2line` returns `??:0`.
To place a crash: `llvm-nm` for the symbol address, add the `+offset` from the
backtrace, then `llvm-objdump` at that address.

### 2.6 Warnings you cannot afford to ignore

Homeworld's build sets `-Wno-implicit-function-declaration`, so a missing header
compiles silently — that has hidden three real latent breakages. When you touch
a file, re-run its compile command from `compile_commands.json` with
`-Werror=implicit-function-declaration` swapped in.

On the OpenMoHAA renderer swap, `glIndex_t` is `unsigned short` under gl4es
(§8), and the compiler said so under sixty other warnings:

```
tr_surface.c:401: warning: incompatible pointer types assigning to 'unsigned int *'
                  from 'glIndex_t *' (aka 'unsigned short *')
```

That was the entire "world smeared into streaks" bug. **Read the warning log
after touching an index or vertex typedef** — changing it is not local to the
draw call, and the warning log is the only thing that finds the other users.

---

## 3. Phase 2 — onto the device, flat

Get the game running as an ordinary Android app before adding a stereo
compositor. Debugging engine problems is far easier without one in the way.
(OpenMoHAA skipped this phase deliberately at the user's direction and paid for
it in §4.9.)

### 3.1 Install and launch

```sh
adb install -r misc/android/app/build/outputs/apk/debug/app-debug.apk
```

**`adb install -r` does not kill a running process.** The old code keeps
running, the next launch resumes it, and a run silently tests the previous
build. Always:

```sh
adb shell am force-stop <package>
adb shell pidof <package>          # must come back empty before starting again
adb logcat -c
```

**Launch from inside the headset, not with `adb shell am start`.** This is
recorded in all three ports. openblack diagnosed the mechanism: `am start`
produces *two* launches — the adb one (uid 2000) and the Quest shell re-issuing
it through its own pending intent (`realCallingUid=1000`). The shell records the
immersive activity when `onImmersiveActivityAppeared` fires, and if the app
process has not spawned yet it stores `pid: 0` and never re-queries:

```
InterstitialController: onImmersiveActivityAppeared: ... pid: 0, ...
InterstitialManager: ... renderingEnabled: 0. submittedFrames: 0
Shell: ImmersiveTransitionSystem: Changed state from TransitionIn to Interstitial
```

`renderingEnabled` stays 0, the transition never reaches `TransitionOut`, and
the headset sits on Meta's purple loading nebula **forever while the app runs
perfectly behind it** — 72 fps, valid poses, no OpenXR errors. It is a race on
process spawn, so it reproduces intermittently, and chasing it in the render
path is a dead end.

If you must launch from adb, check whether it took rather than debugging the
renderer:

```sh
grep -E "renderingEnabled|Changed state" <(adb logcat -d)
# want: renderingEnabled: 1, and Interstitial -> TransitionOut -> Inactive
```

`LCnt=` in the `VrApi` stats line corroborates: 2 layers means the interstitial
is still composited over the app, 1 means only the app is on screen.

Also: **the VR activity's `launchMode` should be `singleTask`, not the
`singleInstance` SDL's flat activity uses.** Under `singleInstance` the shell's
second launch fails to find the reusable task and creates a second one, which
makes the stall considerably more likely on top of the race.

### 3.2 Game data, and the permission trap that eats a day

No game data ships with the app (and none may be — these are GPL engines with
retail assets). It goes in the app's own external files directory, which needs
no permission and is also where the engine writes config and saves:

```
/sdcard/Android/data/<package>/files/
```

**Nothing under `/sdcard/Android/data/<pkg>/` exists until the app has run
once.** Verified by uninstall/reinstall on a Quest 3: straight after
`adb install`, neither `files/` nor the package folder above it is there. The
app creates them on first run through `getExternalFilesDir`.

Everything below follows from one fact: **a directory created by
`adb shell mkdir` is owned by `shell`, not the app, and no chmod changes that.**

- **`adb push` cannot create directories** under an app's data directory — it
  fails with `secure_mkdirs failed: Operation not permitted` partway through the
  transfer. Create them first, in one go:

  ```sh
  cd /path/to/game/main
  adb shell mkdir -p $(find . -type d | sed 's|^\./|/sdcard/Android/data/<pkg>/files/main/|' | tr '\n' ' ')
  adb push --sync . /sdcard/Android/data/<pkg>/files/main/
  adb shell chmod -R 775 /sdcard/Android/data/<pkg>/files
  ```

- **`chmod` must come *after* `adb push`**, or push fails with
  `remote fchown failed`.

- **Use the setgid bit — `2775`, not `775`.** The setgid bit is what carries the
  `ext_data_rw` group down into directories the app creates later; without it, a
  subsequent `adb push` into those subdirectories fails with `remote fchown
  failed`.

- **The app is `other`, so world bits are the only ones that apply.** `run-as`
  reports the app in `ext_data_rw` and it is *not*: read `/proc/<pid>/status` of
  the running game and the groups are `3003 9997 20213 50213`. Against a
  `shell`-owned directory the app is neither owner nor group. So **`2775` and
  `664` mean read-only to the game.** For anything the game writes into, that
  has to be `2777` on directories and `666` on files. Nothing is given away —
  Android already fences `Android/data/<pkg>` off to this package and `shell`.

- **Better: only push directories the game reads, and let the engine create the
  rest.** Pre-creating `main/configs` or `main/save` leaves the game unable to
  write its config or your progress. In MOH:AA, which autosaves on mission start
  and treats the failure as fatal, the symptom is a black screen after New Game
  with `FS_CreatePath: failed to create path` in the log.

  ```sh
  adb shell rm -rf /sdcard/Android/data/<pkg>/files/main/configs \
                   /sdcard/Android/data/<pkg>/files/main/save
  ```

- **Running the game once before copying anything sidesteps the whole problem.**
  The folder then exists, correctly owned, and no chmod is needed at all.

- **Never restore a backup of the data directory with `adb push` alone.** Read-
  only asset files are fine; config files and save directories the game rewrites
  are not, and restoring those makes the game crash at startup before it logs
  anything.

**The failure mode to watch for is silence.** Files pushed by adb are 644 and
world-readable, so game data loads perfectly while writes fail — which reads as
"the data is fine". Homeworld's version of this: the save screen lists every
existing save and loads them, and only *creating* a new one fails with "error
writing to file, check disk space", because `r-x` is enough to traverse and read
a directory but not to add to it.

Write an `install.py` that does all of this. Both Homeworld and openblack ended
up with one, and it is the difference between a tester who succeeds and a tester
who files a bug about your game being broken.

### 3.3 Wireless adb

The endpoint moves in **both halves**. The port changes every time wireless
debugging is toggled; the IP changes whenever the DHCP lease turns over. And
`adb mdns services` will happily keep advertising a dead endpoint, which makes a
stale one look authoritative — read both off the headset's own Wireless
Debugging screen.

The two failure messages mean different things:

- **"Connection refused"** — right host, no `adbd` on it. Wireless debugging is
  off or the port moved.
- **"No route to host"** — nothing answers at that IP. The lease moved; an
  `nmap -sn` sweep of the /24 will find it.

The connection dies whenever the headset sleeps: `adb disconnect` then
`adb connect`. A logcat wrapper that survives the sleep/wake cycle is worth
keeping — a plain `adb logcat` dies with the connection.

### 3.4 The gradle staleness trap

**Gradle's up-to-date check does not reliably notice a replaced `jniLibs/*.so`.**
Copying a freshly built library over the old one and re-running `assembleVrDebug`
can report `BUILD SUCCESSFUL` while packaging the *previous* library — the APK
mtime stays put and you spend the afternoon debugging a build you are not
running. Check that the APK is newer than the `.so`:

```sh
ls -la --time-style=+%H:%M:%S app/src/main/jniLibs/.../lib*.so app/build/outputs/apk/**/*.apk
```

Also: the shell's working directory persists between commands in an agent
session. A stale `cd` into `android/project` deploys the wrong APK, and a stale
`cd` into `android/` sends `cmake --preset` looking for a `CMakePresets.json`
that is not there. Use absolute paths in deploy scripts.

---

## 4. Phase 3 — the OpenXR session

Goal: the world renders in tracked stereo at the display's real rate.

### 4.1 Loader, manifest, extensions

Link the **Khronos `openxr_loader`**, not a vendor SDK — it finds whatever
runtime the device provides (Meta, Pico, SteamVR) rather than binding you to
one. Build it from source with `BUILD_LOADER=ON`, `DYNAMIC_LOADER=ON`,
`BUILD_TESTS=OFF`, `BUILD_API_LAYERS=OFF`, and give it the `lib` prefix so the
APK ships it.

Two instance extensions are the whole baseline:

```c
XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME
```

Add `XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME` when the runtime offers it (§7.3).
Note RTCWQuest's is vestigial — this is original work, not a copy.

The manifest is what makes the headset launch you immersively:

```xml
<uses-feature android:glEsVersion="0x00030000" android:required="true" />
<uses-feature android:name="android.hardware.vr.headtracking"
              android:required="true" android:version="1" />
...
<intent-filter>
    <action android:name="android.intent.action.MAIN" />
    <category android:name="android.intent.category.LAUNCHER" />
    <category android:name="com.oculus.intent.category.VR" />
</intent-filter>
```

with `android:launchMode="singleTask"` (§3.1) and a `configChanges` list wide
enough that the system never recreates the activity under you.

### 4.2 Who owns the EGL context — the SDL handshake

The session binds to a GL context. If the engine uses SDL, **bind OpenXR to
SDL's existing context** rather than creating a private one: one GL context in
the process, no thread or context juggling, and the flat path stays intact.

Getting that context *actually current* is the awkward part, and it is worth
understanding rather than copying:

```c
/* SDL_EGL_MakeCurrent, handed no surface and without gl_allow_no_surface: */
if (!egl_context || (!egl_surface && !_this->gl_allow_no_surface)) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
return 0;   /* success, either way */
```

In a headset there is no window being presented, so that surface never arrives.
**SDL unbinds the context and reports success.** SDL and EGL then disagree
permanently, and asking SDL to bind it again does nothing — `SDL_GL_MakeCurrent`
sees its own bookkeeping agree and returns early with `/* We're already current. */`.

The fix is RTCWQuest's: make the context current against a **16×16 pbuffer**
(their `egl->TinySurface`), using the context SDL already created. The pbuffer
must outlive the session and be released at shutdown.

For a non-SDL engine, the same principle applies from the other direction —
openblack's `VrSystem` creates the EGL context itself and hands the *same* one to
both OpenXR (as `XrGraphicsBindingOpenGLESAndroidKHR`) and bgfx. That works
because bgfx creates its own context **only** when `PlatformData::context` is
NULL, and with an external context its `makeCurrent()` and `swap()` become
no-ops — which matters, since under OpenXR the compositor owns presentation and
an `eglSwapBuffers` here would fight it.

> **Why GLES and not Vulkan**, if you have the choice: OpenXR requires the
> `VkInstance` and `VkDevice` be created with extensions the runtime names at
> startup (`xrGetVulkanInstanceExtensionsKHR`). An engine or middleware that
> creates both internally with no injection hook can never adopt its device into
> an OpenXR session without being patched. GLES has the opposite property.

### 4.3 Session lifecycle

- **Only one process may hold an OpenXR session.** If the previous run has not
  fully exited, the new one falls back to flat mono rendering **with no error**.
  The tell is a log with no `OpenXR initialized` line. This is why §3.1 insists
  on confirming `pidof` is empty.
- **The headset must be awake**, or the session never reaches
  `XR_SESSION_STATE_FOCUSED` and the launch appears to hang.
- **Once you bind controllers, Horizon OS refuses to launch without one on:**

  ```
  ActivityLaunchInterceptorController: RequiresControllersLaunchInterceptor
  CaseDialogAnalytics: dialogId=common_system_dialog_app_launch_blocked_controller_required
  ```

  The activity never starts and the process never appears — which looks exactly
  like a crash if the headset is asleep and nobody is wearing it.
- **The session belongs to the GL context it was created with.** If the engine
  restarts its renderer (Quake engines do, during startup), the session must be
  torn down and rebuilt around it. In OpenMoHAA that bracketing is
  `CL_ShutdownRef` / `CL_StartHunkUsers`.

### 4.4 Swapchains and colour space

**Prefer `GL_SRGB8_ALPHA8`, then disable `GL_FRAMEBUFFER_SRGB` via
`GL_EXT_sRGB_write_control`.** This was measured on Homeworld and is one of the
most valuable findings in this document, because the symptom is subtle and gets
misdiagnosed as a lighting bug.

A player reported the fleet as "lit from all sides, where the original only had
parallel lighting from one distant source". The lighting was **bit-identical** to
the flat build — nothing in that path reads the camera, head pose or world scale.
The defect was presentation: the swapchain was created linear `GL_RGBA8` on the
reasoning that the game's output "is not sRGB-encoded". That has it backwards. A
1999 engine's colours are already **display-referred**, so declaring the buffer
linear makes the compositor gamma-encode them a second time.

Measured from a `screencap`, hull pixels only:

| | measured | expected from the data |
|---|---|---|
| darkest decile | 115/255 (0.45) | 0.19 |
| brightest decile | 189/255 (0.74) | ~1.0, clipping |
| bright:dark | **1.76 : 1** | ~5 : 1 |

sRGB-encoding 0.19 gives 0.473 against a measured 0.45 — that is the whole
diagnosis. Two controls ruled out brightness/contrast: the region outside the
lens barrel stayed exactly `(0,0,0)` and white still reached 255, so only the
midtones moved, which is what a gamma curve does.

The sequence matters, because GLES 3.0 **also** converts on *write* into an sRGB
target — including through `glBlitFramebuffer`:

1. check for `GL_EXT_sRGB_write_control`
2. `glDisable(GL_FRAMEBUFFER_SRGB)`
3. request the sRGB format

> **Do not stop after step 3.** Core ES 3.0 has no state to disable sRGB write
> conversion — `GL_EXT_sRGB_write_control` is what adds the switch, and it
> defaults to **on**. So the format on its own leaves the driver encoding the
> engine's already display-referred colour on write, which the compositor then
> passes through: **the same lifted mid-tones as the linear format, reached from
> the other side.** Writing the three cases out, with `V` the display-referred
> value the engine computes:
>
> | swapchain format | write conversion | stored | displayed |
> |---|---|---|---|
> | linear `RGBA8` | n/a | `V` | `encode(V)` — washed out |
> | `SRGB8_ALPHA8` | on (the ES default) | `encode(V)` | `encode(V)` — **washed out, identically** |
> | `SRGB8_ALPHA8` | **off** | `V` | `V` — correct |
>
> An earlier note in the Homeworld tree said the middle case would "over-darken
> instead". That is wrong in direction, and it matters: it makes a port that has
> only done step 3 look like it cannot possibly be the washed-out one. OpenMoHAA
> sat in exactly that middle case for months with `createInfo.format =
> GL_SRGB8_ALPHA8` and no write control, and read as flat.

There is also **no correct fallback** if the extension is absent and the engine
renders straight into the swapchain — both formats land on `encode(V)`. Log which
you got rather than silently choosing.

**All swapchains must take the same format** — eyes, panels, wrist cards — or the
overlays drift in colour away from the scene behind them. The one global
`glDisable` covers every one of them, blits included.

Expect a corrected build to look **considerably darker**. That is how the game is
supposed to look.

(This has not been re-measured on a modern renderer. openblack may already be
linear-correct, in which case the preference should be inverted there. `adb
exec-out screencap -p` settles it with numbers — see §9.6.)

Other swapchain facts:

- **A swapchain rotates through ~3 images**, and the runtime hands back a
  different index each frame. Anything the engine expects to persist frame to
  frame — a 2D screen the UI redraws incrementally — must be drawn into **one
  persistent buffer** and blitted into the swapchain image. Skipping that step
  is the classic menu flicker.
- **Bind swapchain images once, not per frame.** openblack gives each image its
  own framebuffer object whose colour attachment is permanently repointed at it,
  so the hot path is an array lookup.

### 4.5 The frame loop

`xrWaitFrame` **is the frame pacer** — it blocks until the runtime wants the next
frame, and that is what holds the app to display cadence. Do not add your own
sleep or vsync alongside it.

The ordering that is load-bearing (openblack's, generalised):

1. poll events → `xrWaitFrame` / `xrBeginFrame`, which yields the eye poses
2. render both eyes into their swapchain framebuffers
3. **make the renderer actually submit its GL commands**
4. release the swapchain images, then `xrEndFrame`

Step 3 must precede step 4. If the renderer defers or batches (bgfx records and
submits later; **gl4es batches geometry and issues it lazily**), releasing a
swapchain image as each eye finishes *recording* tells the runtime the image is
complete while its draw calls are still queued. Release both eyes at the end, in
one place.

Watch for **re-entrancy**. In Homeworld, `mrBuildShips` → `cmConstructionBegin` →
`rndClear()` → `rndFlush()` → `vrFrame()` re-enters the frame function three
times while `xrBeginFrame` is outstanding. It is guarded; the guard must not be
removed.

### 4.6 Two eye passes, and only two

Design the frame as **exactly two eye passes from day one**. Homeworld ended up
with a mono pass plus two eye passes because its 2D UI and camera capture were
entangled with the main view, and it still renders the world three times a frame.

The cleanest seam in a Quake-lineage engine is to **substitute the renderer's
idea of the screen**: point `FBO_Bind(NULL)` (or the equivalent default
framebuffer) at the eye's swapchain image, and the entire existing render path
lands in the headset without knowing anything has changed. OpenMoHAA's
`re.SetDefaultFramebuffer` does exactly this.

Do **not** route the eye pass through the renderer's own internal FBO stack on
the way. In VR each eye is already a swapchain image; routing through
`tr.renderFbo` first costs a second full-resolution pass and resolve per eye for
nothing. (OpenMoHAA sets `r_ext_framebuffer_object 0` on Android for this reason
while still loading the entry points.)

If the engine's renderer takes a camera and a target framebuffer as parameters
already (openblack's `RendererInterface::DrawScene`), per-eye rendering is a loop
over that struct rather than surgery.

### 4.7 Projection — the off-centre frustum, and the sign that will bite you

Take the per-eye FOV and pose from `xrLocateViews`. The runtime's frustum is
**asymmetric** — `angleLeft` and `angleRight` are not mirror images.

Right-handed (OpenGL convention, `m[11] = -1`, `w = -z`) — this is what
OpenMoHAA uses and it is correct:

```c
const float tanLeft  = tanf(fov->angleLeft);
const float tanRight = tanf(fov->angleRight);
const float tanWidth = tanRight - tanLeft;
...
m[8] = (tanRight + tanLeft) / tanWidth;
```

**If your engine is left-handed, that sign flips**, and this cost openblack a
debugging cycle. With `[2][3] = +1`, `w = +z`, and solving `x_ndc = 1` at the
right edge (`z = near`, `x = tanRight * near`) gives:

```
2*tanRight/tanWidth + [2][0] = 1   =>   [2][0] = -(tanRight + tanLeft)/tanWidth
```

With the right-handed sign, each eye's frustum is offset the wrong way by about
0.24 in NDC — a quarter of the image width, **mirrored per eye**, so the eyes
disagree by roughly half a screen.

> **The failure is deceptive because either eye alone looks completely
> correct.** Only convergence is broken, so it reads as an IPD or head-pose
> problem rather than a projection one. If the picture is fine with one eye
> closed and wrong with both, look at the frustum offset first.

**Frustum culling must use the runtime's asymmetric frustum, not the game's
fov.** In OpenMoHAA, `renderergl1`'s `R_SetupFrustum` culled against the game's
symmetric fov and clipped terrain that should have been visible at the edges.
(`renderergl2` does not have this bug — its version takes the projection edges.)
Grep for every place the engine derives a frustum, a LOD distance or a
visibility cone from `fov_x`/`fov_y`.

### 4.8 Head pose

Four rules, each learned by getting it wrong:

- **Orientation travels as axes, never Euler angles.** Converting the headset
  quaternion to pitch/yaw/roll and composing by *adding* angles only works while
  pitch and roll are near zero, and turns every head movement into a twist once
  they are not.
- **Measure position from where the head was when tracking started, and subtract
  the starting heading too.** Otherwise the player's real standing height stacks
  on top of the character's eye height, and the play space's arbitrary forward is
  added to the character's heading — which is why the player spawns facing
  backwards.
- **Apply the head pose at the lowest point in the render path** — OpenMoHAA does
  it in `RE_RenderScene` — so it follows *every* camera the game has: the
  player's, cutscenes, ladders, death animations, without each one needing to
  know about VR.
- **Parent the head pose to the game camera; do not replace it.** openblack's
  `VrCamera::SetEyeTransform` composes `eyeView * gameView`, so the existing
  camera logic still decides where in the world the player is and the headset only
  adds the local offset.

**Handedness.** OpenXR is right-handed with **-Z forward**. If the engine is
left-handed, mirror Z on **both sides** of the view matrix — applying the mirror
to the translation alone leaves the basis wrong and mirrors the world. Note that
the mirror is its own inverse, so `inverse(M T M) == M inverse(T) M`, which is
what lets one function serve both the eye view and the controller pose.

**World scale is a real parameter, and it can break rendering, not just feel.**
openblack's `k_VrWorldScale` started at 1.0, which put a controller 0.5 m from
the head at 0.5 game units from the eye — **inside the 1.0 near clip**, so the
hands were culled and invisible. It is now 100. It also sets how far a physical
lean moves you, and only the headset can settle that.

### 4.9 Render resolution, and restarts that destroy the session

**Do not set the render resolution through cvars.** In OpenMoHAA, `r_mode` is
`CVAR_LATCH` *and* registered by the renderer itself, so nothing the client sets
beforehand can win — an ordinary set is deferred, a forced set lands on a cvar
that does not exist yet. The renderer must **ask the client** instead, via a new
`refimport_t` entry point (`GetVRRenderResolution`).

And the bug that hid behind that, which cost most of a session:

**`sdl_input.c`'s `SDL_WINDOWEVENT_RESIZED` handler compares the window size
against `cls.glconfig.vidWidth/Height`, which in VR is the *eye buffer* and never
the Android surface.** It can never match, so it set
`r_customwidth`/`r_customheight`/`r_mode -1` and scheduled a `vid_restart` about
five seconds into every run — cvars the renderer then ignores. The restart
destroyed and rebuilt the GL context and the OpenXR session **under a running
frame loop**, and the framebuffer agreement between gl4es and the driver did not
survive it.

Measured: the menu drew a full `256/256` panel samples right up to
`CL_Vid_Restart_f` and nothing ever after. An earlier status document had called
this restart *"harmless … but wasteful"*. It was the entire bug.

> **Decline window-resize restarts under VR**, and treat any `vid_restart` in a
> headset as a session teardown that has to be handled explicitly.

---

## 5. Phase 4 — the flat content: HUD, menus, cutscenes

Screen-space 2D is uncomfortable in a headset: it does not converge, and at the
edges of an asymmetric frustum it is not even in the same place per eye.

### 5.1 Quad layers for anything full-screen

Put menus, loading screens and cutscenes on an **OpenXR quad layer** fixed in the
room. In OpenMoHAA that is everything with `clc.state != CA_ACTIVE`.

Place it once, in front of wherever the player is facing, **using only their
heading** so it stays upright. A panel that inherits head pitch and roll is
unusable.

**Submit a black projection layer *underneath* the quad on flat frames**
(RTCWQuest, `TBXR_Common.c:1576`) rather than submitting the quad alone. That is
also the mechanism a pointer ray and controller models need on a menu-only
frame: there is no 3D scene to draw a beam into otherwise.

**Draw the panel into one persistent buffer and blit it into the swapchain
image** — see §4.4. The engine's 2D path expects last frame's content to still be
there.

### 5.2 Watch for two coordinate spaces that do not match

Homeworld's framebuffer is 4128×2208, while `prim2d` and the font system draw in
the game's logical UI resolution of **1024×768** — measured from the swapchain
sizes the code derives, not assumed. Anything mixing framebuffer-space pixels
with UI-space pixels is ~4× out horizontally; that is what put the panel reticle
off-screen.

Worse, **the two spaces have different aspect ratios**: 1024×768 is 4:3 while the
framebuffer is ~1.87:1, so the scale factors differ — 4.03 horizontally against
2.87 vertically. Everything the 2D path draws is therefore stretched about 1.4×
horizontally, which makes circles into ellipses and text too wide. The stereo
world escapes this only because the per-eye projection substitutes the true
headset FOV.

### 5.3 The in-game HUD

Two positions, and the reference takes the less obvious one:

- **Wrist or weapon-mounted readouts** (Homeworld: three wrist cards as separate
  compositor layers — controls reference, fleet status, command wheel). Good for
  a strategy game and for anything the player consults rather than monitors.
- **RTCWQuest keeps the HUD in screen space, inside the eye buffers**, re-centred
  on each eye's off-centre frustum, given stereo parallax, and scaled in from the
  edges. For a shooter this is what they shipped, and it is less work than it
  looks.

### 5.4 There is no keyboard

If any menu needs text — a player name, a server address, chat — you have to draw
one. Homeworld's approach (`src/SDL/vrkeys.c`) avoids a fourth swapchain
entirely by drawing the key grid into the window framebuffer just before that
frame is copied to the panel quad, and hit-testing against the controller ray
already mapped into logical UI pixels.

Details worth copying:

- **Inject scancodes, not characters.** The engine derives the character itself
  (`SDL_GetKeyFromScancode` into an ASCII-indexed table). Injecting letters
  fights it.
- **Hold shift genuinely** — the engine records the shift state at press time, so
  a shifted glyph means `LSHIFT` down, the key, `LSHIFT` up.
- **A caps latch must apply to letters only.** One ray cannot point at shift and
  a letter at once, and a latch that shifted everything turns the number row into
  `!"#` — no good for an IP address.
- **Never cover what is being typed into.** Put the grid along the bottom and
  move it to the top when the focused field is down there.

---

## 6. Phase 5 — input and interaction

This is the largest and least mechanical phase. A wrong choice here causes
motion sickness, not a bug report.

### 6.1 Action sets and bindings

One action set, with **every action carrying both hands as subaction paths**, so
the hand is chosen when the state is read rather than by declaring each action
twice.

Suggest bindings for `oculus/touch_controller` and, as a fallback,
`khr/simple_controller`. Two facts:

- **Only aim and a trigger go on the simple profile.** It has no thumbstick, face
  buttons or grip, and **one path a profile does not recognise makes the runtime
  reject the *whole suggestion***, not the offending entry. A single stray
  binding costs every binding for that profile.
- `select/click` bound to a float action is legal; the runtime reports the digital
  source as 0 or 1.

**`xrSyncActions` is gated on `XR_SESSION_STATE_FOCUSED`.** Unfocused it still
*succeeds* but leaves every action inactive — which is indistinguishable from
absent hardware, so a naive `tracked` flag reads false rather than "not our turn".

Make action setup failure **non-fatal**: stereo rendering still works and the game
can fall back to its flat input path.

Size the suggested-binding array generously. OpenMoHAA's was 16 with 14 in use
before the weapon work needed more; it is now 32.

### 6.2 Locomotion

For a shooter: smooth movement, with **snap and smooth turn both available**, and
an adjustable vignette. Make all of it a cvar so it can be tuned on device — there
is no console in a headset, so anything not exposed is not tunable (§9.1).

Two findings that are not obvious:

**Steer with the off hand, not the gaze.** `controllerYawHeading = offhandYaw −
hmdYaw`. Tying travel to where the player looks means they cannot glance sideways
without veering, which is most of why naive VR movement feels like being dragged
around. (This is taken from RTCWQuest — but note it is *not* what they ship: at
their HEAD `controllerYawHeading` is computed and never read, gated behind a cvar
on a dead assignment. A good idea they seem to have abandoned, so try it on the
headset before trusting it.)

**Rotate the stick into the view's frame, not the hand's.** In a Quake engine
`forwardmove`/`rightmove` are resolved against the **view** angles — `PM_AirMove`
builds `wishvel` from `pml.flat_forward` and `pml.flat_left`. So steering with the
off hand means rotating the stick vector *back* by however far the hand leads the
head. **The opposite sign does not offset the steering, it mirrors it**: point the
hand left and the player walks right, and only dead ahead behaves. (Team Beef's
`rotateAboutOrigin` negates the angle it is handed for the same reason.)

**Take the deadzone radially on the raw stick, before that rotation.** Once the
two axes are mixed, testing them separately cuts a dead cross out of the middle of
the stick's travel, so a gentle push at forty-five degrees does nothing at all.

### 6.3 Weapon aim — the formula

Aim from the controller, not the head. This is non-negotiable for an FPS, and the
formula is RTCWQuest's (`rtcw/src/game/g_weapon.c:1904`), computed in the **game**,
where the trace is fired:

```c
VectorCopy(gVR->weaponangles, viewang);
viewang[YAW] = ent->client->ps.viewangles[YAW]
             + (gVR->weaponangles[YAW] - gVR->hmdorientation[YAW]);
```

Pitch and roll come straight from the controller. **Yaw is the view yaw plus how
far the controller leads the head** — the controller's *absolute* yaw is
meaningless, because the player can snap-turn on the stick and the play space has
an arbitrary forward. Only the trace moves; the camera stays on the head.

Getting those angles into the game module needs a channel. The usercmd has no
spare field — `angles[3]` is the view direction and `buttons` is packed — so
OpenMoHAA passes them through `gameImport_t` instead. A dedicated server answers
false and keeps the aim it had.

### 6.4 Weapon placement

The engine's first-person view model is positioned against the camera and given a
walk bob. Both are wrong in a headset: the hand has its own position, and shaking
the view model when the camera is the player's head is a way to make people ill.

RTCWQuest's `CG_CalculateVRWeaponPosition` is the model to copy, including the
part that is easy to miss: **drop the origin to the feet and add the measured head
height back**, so the weapon hangs at the height the player is really holding it
rather than off the camera, where crouching would drag it about.

**Per-weapon alignment is a real requirement, not a polish item.** Every weapon
model has its own muzzle offset and grip angle. RTCWQuest keeps one adjustment per
weapon (`vr_weapon_adjustment_<id>`); OpenMoHAA carries the same as
`vr_weaponAdjust`. Build a way to tune it **without replaying the level** — a cvar
read live, and a test hook that starts the map and grants weapons after a delay,
is worth the hour it takes.

### 6.5 Gestures, and the button budget

Six buttons cannot carry a 1999 game's verb list. Two answers:

- **Gestures for the obvious verbs.** Reload on a *tap* of the weapon hand's grip
  (press and release inside a timeout — and send `+reload` then `-reload` a frame
  later, because the game wants to see the key held for a tick). Two-handed hold
  on the off-hand grip when the hands are close: point the weapon along the line
  between them, which is both how a rifle is held and steadier, since a wobble at
  one hand is damped by the distance to the other. Take **roll at half** rather
  than from the line, which carries none.
- **A radial command wheel** for everything else (Homeworld). Hold a trigger, a
  radial appears at that hand, the stick picks a wedge, releasing runs it.
  Dwelling on a category descends into it. Keep wedge positions **fixed** and dim
  what the current selection cannot do rather than hiding it, so the wheel becomes
  a flick once learned. Take the wheel's **position from the wrist but its
  orientation from the head**, so stick direction always maps to the same wedge no
  matter how the wrist is turned.

**Weapon/inventory switching costs no button if you use a stick axis.** The
dominant thumbstick up/down, gated on the sideways component so it cannot be read
as a turn, and **edge-triggered** so a held stick does not run the whole
inventory.

**Never synthesize keystrokes to trigger a command.** Homeworld's `mrKeyPress`
routes mappable keys through `kbCheckBindings`, which returns 0 for a key the
player has unbound — so the command silently vanishes. Call the engine's own menu
callbacks, or the command wrappers, directly. In a game with multiplayer, call the
*wrapper* layer, not the bare command: that is what marshals packets and
recordings.

### 6.6 Comfort and the camera audit

**Any engine-driven camera motion is a nausea risk**, and every instance needs
auditing: scripted sequences, cutscenes, ladder climbs, death animations, screen
shake. Applying the head pose at the lowest point in the render path (§4.8) means
they all *work*, but working and comfortable are different questions. Grep for
view overrides early — it is the deciding factor in how pleasant the port is to
play.

Two rendering details that read as tracking bugs:

- **Billboards must face the viewer's *position*, not the view plane.** In
  stereo, a sprite oriented to the view plane rotates with head movement and the
  foliage appears to swivel. Fix `AutospriteDeform` and the
  `SPRITE_PARALLEL`/`SPRITE_PARALLEL_UPRIGHT` paths. Leave `Autosprite2Deform`
  alone — it pivots on a fixed axis for beams and rails and is far less sensitive.
- **N-LIPS-style depth-compensating scaling must be disabled.** Homeworld inflated
  small ships in proportion to camera depth to keep them readable on a monitor;
  nothing cancels that through a fixed headset projection, so it only made ships
  swell as the fleet was pushed away.

### 6.7 Input latency, if the loop is not in OpenXR order

If the engine's simulation runs *before* the frame begins, the input is one frame
stale — ~14 ms of pointing lag at 72 Hz, and an all-neutral first iteration.
Button *edges* are not lost, because a sync and a read still happen once per
iteration. Fixing it means hoisting the wait/begin above the simulation, which is
the conventional OpenXR order (wait, simulate, render) — but `xrWaitFrame` is the
frame pacer, so moving it changes the pacing. Know the tradeoff before you touch
it.

---

## 7. Phase 6 — performance

### 7.1 Measure the frame properly, or you will optimise the wrong thing

Three measurement traps in id Tech 3's own instrumentation:

- **`backEnd.pc.msec` is assigned, not accumulated.** Later empty flushes
  overwrite the scene's real number with 0.
- **`Set2DWindow` begins with `R_IssuePendingRenderCommands()`.** The 3D scene is
  *built* during the world pass but only *executed* when something forces a flush
  — and the first thing that does is the HUD. **The whole frame's rendering
  therefore appears to any timer to be HUD cost.**
- **`tr.frontEndMsec` covers only `RE_RenderScene`**, not the cgame work around
  it.

Split a VR frame into **wait / scene-build / issue / GPU**, with the GPU number
taken behind a `glFinish`. That split is what showed rend2 was CPU-bound in the
back end with the GPU idle at 3 ms (§1.1) — a conclusion no aggregate frame time
would have produced.

### 7.2 What is worth spending the budget on

- **MSAA via `GL_EXT_multisampled_render_to_texture`**
  (`glFramebufferTexture2DMultisampleEXT`).
  It resolves in tile memory on Adreno, so it costs close to nothing, and
  aliasing is far more objectionable in a headset than on a monitor. Both
  OpenMoHAA and RTCWQuest have it wired up and switched off; it is a measurement
  waiting to be made.
- **Resolution scale** as a cvar, so the picture can be traded against frame time
  on device without a rebuild. OpenMoHAA ran at 0.6 while debugging and went back
  to 1 once the frame budget was known.
- **Scope zoom by narrowing the composition layer's FOV** (RTCWQuest,
  `TBXR_Common.c:1557`) rather than touching the projection matrix, so
  magnification costs the renderer nothing. Applies directly to sniper scopes and
  binoculars. The render-side FOV path and the layer must agree.

### 7.3 Ask for the display's real refresh rate

**Quest 3 hands out 72 Hz by default.** `XR_FB_display_refresh_rate` is one
extension and two entry points, and it works — enumerate with the standard
two-call pattern **from the frame loop**, not at `xrBeginSession`. It is usually
the largest comfort win available for the least work. Do it *after* measuring,
since it halves the frame budget.

### 7.4 Not there, so do not go looking

Neither RTCWQuest nor either of the other two ports has **foveation**, **space
warp**, or **performance-level hints**. Their instance extension list is the same
two everyone enables. Anything in that direction is original work.

---

## 8. The gl4es boundary

Applies to OpenMoHAA, Homeworld and RTCWQuest. Skip if you are not using gl4es —
but read it before you conclude that a renderer on gl4es is broken.

The engine never learns gl4es is there: it reports itself as `2.1 gl4es wrapper
1.1.6`, so the renderer takes its desktop path and gl4es turns `glBegin`,
`glMatrixMode` and the rest into ES underneath. That works.

What is not obvious is that **gl4es keeps its own copy of GL state, and that copy
can disagree with the driver.** Everything below follows from that one fact.

### 8.1 It answers state queries from its own tables

`glGetIntegerv(GL_CULL_FACE_MODE)`, `GL_FRONT_FACE`, `GL_MODELVIEW_MATRIX` and
friends come out of gl4es's `glstate` (`src/gl/getter.c`), not the driver.
`glGetString` is the same — `GL_VERSION`, `GL_VENDOR`, `GL_RENDERER` come from
constants.

So this proves nothing:

```c
qglGetIntegerv( GL_CULL_FACE_MODE, &mode );   /* compare against what the engine set */
```

The engine set that value *through gl4es*. Reading it back through gl4es returns
the engine's own value. **It agrees by construction whatever the hardware is
doing.** Two measurements taken this way were reported as findings and were
worthless — one of them, "GL and the engine agree on culling", was actively
concealing the bug being hunted.

There is a second-order consequence: the engine's only check that a context is
live is `GL_VERSION` coming back non-NULL, so **under gl4es a context that was
created but never bound sails through `R_Init`.** Ask SDL (or EGL) directly
instead.

**To ask the hardware, resolve the entry point out of the driver:**

```c
void *lib = dlopen( "libGLESv3.so", RTLD_NOW | RTLD_LOCAL );
void (*driverGetIntegerv)(GLenum, GLint *) = dlsym( lib, "glGetIntegerv" );
```

A temporary probe that printed gl4es's answer and the driver's side by side found
the cull bug in one run, after many runs that measured nothing. It is four lines.

### 8.2 It drops calls it judges redundant

Each of these returns early when the requested value matches gl4es's copy:

| Call | Where |
|---|---|
| `glCullFace` | `src/gl/face.c:14` |
| `glDepthMask`, `glDepthFunc`, `glDepthRangef` | `src/gl/depth.c` |
| `glBlendFunc` | `src/gl/blend.c` |
| `glColorMask` | `src/gl/gl4es.c` |
| `glViewport`, `glScissor` | `src/gl/raster.c:61` |
| `glEnable`/`glDisable` for `GL_CULL_FACE`, `GL_DEPTH_TEST`, `GL_STENCIL_TEST`, `GL_POLYGON_OFFSET_FILL` | `src/gl/enable.c`, `proxy_glEnable` |

(`GL_SCISSOR_TEST` is *not* filtered — it falls through to the driver every time.)

**Once gl4es's copy drifts from the driver, the engine can never correct it.** It
asks for the right value, gl4es compares against the wrong copy, calls it a no-op,
and the driver never hears. No error is raised, and nothing looks wrong from
inside the engine — its own state tracker and gl4es's tracker agree with each
other; they are just both wrong about the hardware.

Where a value **must** reach the driver, name a throwaway first:

```c
#ifdef USE_GL4ES
    qglCullFace( GL_FRONT_AND_BACK );   /* cannot be mistaken for a no-op */
#endif
    qglCullFace( face );
```

Nothing is drawn between the two calls, so the throwaway is harmless.

### 8.3 Everything the VR layer sets natively must be routed through gl4es

A VR layer that links `libGLESv3` directly makes a bare `glViewport` reach the
driver without gl4es seeing it. gl4es's copy is then stale, and by §8.2 the
engine's next request for that value is silently dropped.

So wrap every state call gl4es caches — viewport, enable/disable, depth mask,
depth func, blend func, colour mask, cull face, scissor, framebuffer binding —
and resolve them out of `libgl4es.so` by name.

**Three things deliberately stay with the driver:**

- **the swapchain texture attachment** (`glFramebufferTexture2D`) — gl4es cannot
  attach a texture the OpenXR runtime created. RTCWQuest documents our exact
  symptom at `TBXR_Common.c:290`: *"the engine then renders into nothing → black
  eye buffer"*.
- **the blit's `GL_READ_FRAMEBUFFER` / `GL_DRAW_FRAMEBUFFER` bindings** — gl4es
  treats `GL_READ_FRAMEBUFFER` as a note to itself and returns without binding
  (`framebuffers.c:238`), which **silently empties the blit** and kills the
  cutscenes. This was done once by accident with an over-broad macro.
- **the VR layer's own pipeline** — its shader program, vertex attributes,
  uniforms and draw calls. gl4es has no business knowing about those.

Note the link order decides the default. RTCWQuest's ndk-build puts gl4es first,
so *their* VR layer's GL calls go through gl4es by default and they bypass it
explicitly where they must; if `libGLESv3` precedes `libgl4es` in `DT_NEEDED`, the
default is the other way round and the routing has to be done by hand.

### 8.4 gl4es must own the `GL_FRAMEBUFFER` binding

gl4es renders from its own table of framebuffers, not the driver's binding:

```c
gles_glBindFramebuffer(GL_FRAMEBUFFER, current_fb->id ? current_fb->id : mainfbo_fbo);
```

A framebuffer created by the driver's `glGenFramebuffers` is not in that table, so
`gl4es_glBindFramebuffer` raises `GL_INVALID_VALUE`, **returns without binding**,
and leaves `current_fb` at zero — which in this process is the 16×16 pbuffer
(§4.2). Measured directly:

```
fbo: gl4es says 0, driver says 10 | driver viewport 756 -71 403 1126
```

The viewport lies entirely outside a 16×16 buffer, so geometry landed in nothing
with no error raised anywhere. **Have gl4es generate, bind and delete these.**

Also: `RE_SetDefaultFramebuffer` in renderergl1 binds **nothing**. It exists only
to flush — gl4es batches geometry and issues it lazily, so work built for one
target would otherwise arrive in whichever is bound when it finally goes out. Call
it *before* your own bind.

### 8.5 Build gl4es exactly the way the reference builds it

**This was the largest single blocker on the OpenMoHAA renderer swap.**

With CMake's own flags, gl4es **rendered no geometry whatsoever** while clearing
the same framebuffer perfectly. Measured, in the same buffer microseconds apart:

```
gl4es draw test: 144 red (clear), 0 green (drawn), 0 other
```

Matching RTCWQuest's `Android.mk` fixed it outright, with **no engine-side
change**:

```
gl4es draw test: 108 red (clear), 36 green (drawn), 0 other
```

(36/144 is exactly the quarter the test quad covers.) Their flags:

```
-DBCMHOST -DNOX11 -DNO_GBM -DDEFAULT_ES=2 -DNO_INIT_CONSTRUCTOR
-O3 -fcommon -fvisibility=hidden -funwind-tables
```

Two things about that list:

- **`ANDROID` is deliberately not among them.** gl4es's own CMakeLists adds
  `-DANDROID` whenever the NDK toolchain is in use, and ndk-build never does — so
  the reference has only ever been exercised *without* it. It is not cosmetic: it
  selects different code in the loader, in glx, and in `hardext`, which is where
  gl4es compiles a probe shader (`testGLSL`) to decide what its fixed-function
  pipeline may emit. It comes from `add_definitions()`, a **directory** property,
  so it must be removed at the directory it was set on — overriding it on the
  target does not work.
- **`BCMHOST` is the Raspberry Pi switch**, which looks wrong on a Quest and is
  carried anyway, because everything it gates is additionally guarded by
  `!defined(ANDROID)`. Matching the reference matters more than tidiness.

This is convergence on a known-good configuration, **not a diagnosis** — which
flag actually mattered has never been bisected. `-DANDROID` is the strong
suspicion.

Two more build details: gl4es sets `CMAKE_LIBRARY_OUTPUT_DIRECTORY` to
`${CMAKE_SOURCE_DIR}/lib`, which under `FetchContent` is *your* source root, and
it forces the suffix `.so.1`. Redirect it into the build tree as `libgl4es.so`
(§1.4). And the target is called `GL`, because that is what gl4es presents itself
as.

### 8.6 The renderer's entry points must come from gl4es, not the driver

A Quake-lineage renderer does not call GL by name — `qgl.h` declares every entry
point as a **function pointer** filled by `GLimp_GetProcAddresses` from
`SDL_GL_GetProcAddress`.

**On EGL 1.5, which the Quest is, `SDL_EGL_GetProcAddress` tries
`eglGetProcAddress` *before* the library it loaded.** The driver answers for every
name desktop GL 1.x and ES have in common — `glEnable`, `glBindTexture`,
`glDrawArrays`, `glTexImage2D`, ~80 more — so only desktop-only names (`glBegin`,
`glMatrixMode`) would ever reach gl4es. Resolve straight from the gl4es handle
instead.

**The extension string has the same problem.** `SDL_GL_ExtensionSupported` reads
the driver's ES list, which names none of the desktop extensions the renderer asks
after, so `GL_ARB_multitexture` reads as absent and the renderer silently drops to
one texture unit.

And the fixed-function branch of `GLimp_SetMode` typically offers exactly one
context: **desktop OpenGL 1.1**. There is no such thing on Android; window
creation fails and `R_Init` gives up with `Couldn't get a visual`. Under gl4es
that branch must ask for **ES 3.0, then ES 2.0** — the renderer is never told,
because it reads its version through gl4es.

### 8.7 The shape to recognise

> Geometry submitted. Vertex data correct. Indices in range. Matrices correct. On
> screen, unclipped, unculled by the frustum. No GL error. **Nothing drawn.**

That is what a dropped state call looks like from inside the engine, every time.
When you see it, **stop measuring the engine and ask the driver.**

The three bugs on this boundary in OpenMoHAA, each of which presented exactly that
way: the framebuffer binding (§8.4), the viewport (§8.3), and the cull face —
gl4es's copy said `GL_FRONT`, the driver was culling `GL_BACK`, the engine asked
for `GL_FRONT` every frame and gl4es dropped it as redundant. Quake's world
polygons are back-facing in GL's terms, which is why `CT_FRONT_SIDED` culls
`GL_FRONT` in the first place, so the hardware was removing precisely the faces
that should have been visible.

**The symptom was the entire world invisible while models rendered perfectly**,
because model shaders are `CT_TWO_SIDED` and never enable culling at all.
Inverting the culled face did not help either — asking for `GL_BACK` matched what
the driver was already doing. Only disabling culling outright brought the world
back, which made it look like a winding fault for a very long time.

---

## 9. Debugging without a console: the method

This section has been more valuable than any individual fix. In a headset there
is no console, no debugger worth the name, and no way to ask the player to catch a
one-frame flash. Every question has to be answered by something written to logcat
or measured in pixels.

### 9.1 Get a cvar channel onto the device before you need one

In OpenMoHAA that is `misc/android/autoexec.cfg`, deployed next to the game data
(not inside the APK):

```sh
adb push misc/android/autoexec.cfg /sdcard/Android/data/<pkg>/files/main/autoexec.cfg
adb shell am force-stop <pkg>
```

It is exec'd by `Com_ExecuteCfg` **after** `omconfig.cfg`, cannot be loaded out of
a pk3, and lands **before** `R_Init` — so the renderer's `Cvar_Get` finds each
cvar already present and keeps this value instead of its own default. That covers
`CVAR_LATCH` and `CVAR_ARCHIVE` cvars.

Two consequences:

- **Archived cvars beat a changed default.** Changing a default in code does
  nothing if the device's stored config already has the cvar. For values that must
  be forced every run, register them unarchived and set them at every start
  (OpenMoHAA's `VR_TuningCvar`).
- **Set every knob explicitly, never by omission.** A value left out is whatever
  the stored config last archived. `cg_shadows` cost a whole round trip that way:
  it was already 0, so the run that was supposed to test disabling shadows tested
  nothing. **Read the stored config before believing a negative result** —
  `adb pull .../files/main/configs/omconfig.cfg`.

### 9.2 Before believing any negative rendering result, prove the renderer can draw at all

**Every engine-side experiment run before §8.5 was worthless, because gl4es could
not draw anything.** Under a broken layer, *any* test that ends "and nothing was
drawn" returns the same answer regardless of what it tests. On the strength of such
results one session wrongly concluded, and acted on, all of:

- that 32-bit indices were the problem
- that the `glDrawElements` client-array path was broken
- that compiled vertex arrays were broken
- that `glDrawBuffer(GL_BACK)` on an FBO was discarding everything
- that immediate mode worked while arrays did not, and later that *neither* worked
  — two contradictory conclusions, both from invalid data

The cheapest possible check, and the one that finally cracked it: **ask the layer
for a `glClear` and then a flat untextured quad into the same buffer, and read the
pixels back.** A clear needs no shaders; a draw needs the whole pipeline. If the
clear lands and the draw does not, stop looking at the engine.

### 9.3 A probe must be shown to reach the thing it measures

Three probes in one session had blind spots exactly where the fault was, and each
produced a confident wrong conclusion:

- a panel readback that counted "brighter than 8" **against a buffer that is never
  cleared**, so *nothing drawn* and *drawn black* were the same number;
- an `r_flatColor` override that did not apply to multitextured surfaces, so it
  only ever covered models — every "the world is not drawing" reading from it was
  about geometry that was never in question;
- a flat-red override that restored its state inside a branch that was never
  taken, corrupting the renderer it was measuring.

And a fourth, dumber version: a probe gated on `primitives == 2` silently stopped
running when a separate change flipped `primitives` to 1, and two rounds of
"nothing drawn" results were actually "the probe never executed".

> **Verify instrumentation still fires after changing anything it depends on.**

### 9.4 Verify the change landed

`r_noDepth` was declared, registered, documented and tested across several device
runs — and **never actually used**, because the edit that was supposed to read it
silently failed to apply and nothing checked. Every result attributed to it was a
no-op, and it sat in the "known good" configuration taking credit for what
disabling culling was doing.

**Grep for the symbol after editing**, or read the warning log. A cvar that reads
nothing looks exactly like a cvar that changes nothing.

### 9.5 Single runs are not evidence

The same configuration produced different results on different runs more than
once, and a bisect was built on top of that before it was noticed.

- **Repeat a result before building on it.**
- **Never change code and configuration in the same step** — doing so destroyed
  the only known-good baseline and cost several round trips to rebuild it.

### 9.6 Look at the frame

**`adb exec-out screencap -p` captures the composited stereo frame** — both eyes,
lens distortion, passthrough, quad layers and the world projection layer, at
4128×2208 on a Quest 3. (An earlier note in the Homeworld tree claimed the
opposite; it was wrong.) That makes it **ground truth for what the player sees**,
and it gives you pixels you can measure — it is how the colour-space defect in
§4.4 was pinned to a number instead of being argued about.

The in-engine version: OpenMoHAA's `vr_captureEye` writes the eye buffer to
`main/vrshotN.tga` every two seconds. One image settled questions that six rounds
of verbal description could not.

**Pixel counts bound the answer; a picture gives it.** And prefer pixel readbacks
logged as numbers over anything a human has to catch by eye.

Pull captures individually — `adb pull .../files/main/` drags the whole pk3 set.

### 9.7 Miscellaneous, each of which produced a wrong conclusion once

- **`grep` silently finds nothing in files with invalid UTF-8.** Several 1990s
  sources carry extended-ASCII bytes, and in a UTF-8 locale GNU grep gives up on
  the file and reports *no matches* rather than an error. It is not "the symbol is
  not there", it is "grep declined". Two confident, wrong conclusions in one
  session. **Always `LC_ALL=C grep -a`.**
- **AddressSanitizer cannot see an engine's own heap.** If the engine has a custom
  allocator, a use-after-free inside it is a bare SEGV with no ASan report. It
  still catches globals and the real heap.
- **A fixed-size engine heap fails silently.** Homeworld's `memAllocFunctionANV`
  returns NULL when no block is big enough, its diagnostic is compiled out of a
  release build, and callers do not check — `etgEffectCreate` dereferences the
  result on the next line. Exhaustion presents as an unexplained SIGSEGV with no
  tombstone and nothing in the log, **deterministic after enough play and absent on
  a freshly loaded save**. That fingerprint is how you recognise it. VR asks more
  of the heap than the original ever did (full mesh detail, 3× draw distance), so
  the cap probably needs raising.
- **`inet_ntoa` returns a static buffer.** Two calls in one `printf` both print the
  second.
- **A pre-existing crash is not your crash.** OpenMoHAA has a recurring `SIGSEGV`
  in the Adreno driver (`gsl_memory_alloc_pure_64`, fault address `0x90`) on a
  secondary thread at every launch. It predates all VR work, was present in the
  flat builds, and the process survives it every time. Establish which faults are
  background noise **before** you start bisecting.

---

## 10. Order of work

```
1  Android build of the dedicated server or a headless client   (proves the toolchain)
2  Full client APK, GLES context, main menu                     (proves the renderer)
3  Side-load retail data, reach gameplay, flat                   (proves the filesystem)
4  Controller input via the existing joystick path, playable     (baseline + perf numbers)
5  OpenXR session, one eye, correct pose                         (proves the XR path)
6  Two eye passes, correct IPD and projection                    (stereo)
7  Flat content on a quad layer                                  (menus usable)
8  Locomotion and weapon handling                                (VR design begins)
9  Comfort pass, camera audit, HUD                               (the long tail)
```

Steps 1–3 are mechanical build engineering. Step 5 onward is where the interesting
decisions live.

**Exit criteria worth holding to:**

| Phase | Done when |
|---|---|
| Android build | APK boots, logs to logcat, reaches the main menu with retail data side-loaded |
| Flat on device | A mission is completable, flat, on the headset, at a stable frame rate |
| Stereo | Correct scale and IPD, head tracking with no mismatch, stable at the display rate, menus readable |
| Interaction | A mission playable end to end, comfortably, **by someone who is not the developer** |

Phase 2 is worth doing even though it looks skippable. OpenMoHAA skipped it and
spent the equivalent time on §4.9.

---

## Appendix A — the trap index

Ordered by how much time each cost, worst first.

1. **gl4es built with the wrong flags renders nothing** while clearing correctly (§8.5).
2. **gl4es's cached state drifts from the driver's** and the engine can never correct it (§8.2, §8.7).
3. **A window-resize `vid_restart` destroys the GL context and OpenXR session mid-run** (§4.9).
4. **`CACHE INTERNAL` implies `FORCE`** — your `-D` never applied (§2.3).
5. **`adb shell mkdir` directories are owned by `shell`**, and the app is `other` there (§3.2).
6. **Linear swapchain format double gamma-encodes** a display-referred renderer (§4.4).
7. **SDL unbinds the EGL context and reports success** when there is no window surface (§4.2).
8. **Off-centre frustum sign is handedness-dependent**; either eye alone looks fine (§4.7).
9. **32-bit indices written through a 16-bit array** after `glIndex_t` changes (§2.6).
10. **`adb install -r` does not kill the running process** — you test the old build (§3.1).
11. **Gradle packages a stale `.so`** and reports success (§3.4).
12. **`am start` strands the headset on the Meta interstitial** while the app runs fine (§3.1).
13. **Only one process may hold an OpenXR session**; the second falls back to flat, silently (§4.3).
14. **`xrSyncActions` succeeds but reports nothing** when not `FOCUSED` (§6.1).
15. **One bad binding path rejects the whole profile suggestion** (§6.1).
16. **Off-hand steering with the wrong sign mirrors rather than offsets** (§6.2).
17. **Frustum culling against the game's symmetric fov** clips the edges of an asymmetric one (§4.7).
18. **`grep` declines on non-UTF-8 sources and reports no matches** (§9.7).
19. **A swapchain rotates 3 images**, so incremental 2D drawing flickers (§4.4).
20. **`-Wno-implicit-function-declaration` hides a missing header** (§2.6).

## Appendix B — numbers, for calibration

Measured on a Quest 3.

| | OpenMoHAA | Homeworld | openblack |
|---|---|---|---|
| Per-eye resolution | 1680×1760 native | — | — |
| Frame rate | **90 fps** | 72 fps | 72 fps |
| Back end, per eye | **4–6 ms** (gl1+gl4es) | — | ~7 ms app GPU |
| Rejected path | rend2: 50–60 ms/eye CPU, 10–15 fps | — | Vulkan (no device injection hook) |
| Composited screencap | — | 4128×2208 | — |
| MSAA | wired, off (`vr_samples 0`) | blocked on per-eye FBOs | — |
| Refresh rate ext | works, 90 Hz | — | — |

---

## Related documents in this tree

- `docs/markdown/04-coding/03-android.md` — the Android build, step by step
- `docs/markdown/04-coding/04-gl4es.md` — the gl4es boundary in full
- `VR_PORT_PLAN.md` — the original survey and plan, including where it was wrong
- `VR_PORT_STATUS.md` — what is built, what was learned, what is next
- `VR_RENDERER_HANDOFF.md` — the renderergl2 → renderergl1 swap in detail

And outside it:

- `/home/berkybear/RTCWQuest` — the finished reference port. **Read it first.**
- `/home/berkybear/Homeworld` — `VR_PORT_HANDOFF.md`, `android/README.md`,
  `documentation/vr-interaction-design.md`
- `/home/berkybear/openblackvr` — `android/README-vr.md`
