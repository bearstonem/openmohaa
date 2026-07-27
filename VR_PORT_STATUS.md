# OpenMoHAA on Quest 3 — where the port stands

Companion to `VR_PORT_PLAN.md`, which is the original survey and plan. This
records what has actually been built, what was learned that contradicts the
plan, and what to do next.

Branch `vr-quest3`. Everything below is committed.

---

## 1. What works

The game runs on a Meta Quest 3 in stereo VR: native 1680×1760 per eye, 6DoF
head tracking, stick locomotion, snap turning, and menus on a panel fixed in
the room. A mission can be started and played.

| Phase | State |
|---|---|
| **1 — Android build** | Done. APK builds, boots, logs to logcat. |
| **2 — Playable flat** | Skipped deliberately; went straight to VR at the user's direction. |
| **3 — OpenXR stereo** | Done bar a frame-rate measurement. |
| **4 — VR interaction** | Started: locomotion, snap turn, head aim, menu pointer. Weapon handling not begun. |

### Build

```sh
cmake -B build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=OFF
cmake --build build-android-arm64 -j$(nproc)

cd misc/android && gradle assembleDebug
adb install -r misc/android/app/build/outputs/apk/debug/app-debug.apk
```

SDL2, OpenAL Soft and the Khronos OpenXR loader are all fetched and built from
source; `-DSDL2_SOURCE_PATH=` / `-DOPENAL_SOURCE_PATH=` / `-DOPENXR_SOURCE_PATH=`
point at local trees instead. Full instructions and the deployment traps are in
`docs/markdown/04-coding/03-android.md`.

**Do not launch with `adb shell am start`.** Launch from inside the headset.

### Game data

Lives in `/sdcard/Android/data/org.openmoh.openmohaa/files/main/`. Currently
populated from a GOG War Chest install.

**Never create `main/save` or `main/configs` with `adb`.** A directory made by
`adb shell mkdir` belongs to `shell`, and no chmod makes it app-writable — the
app is `other` there. MOH:AA autosaves on mission start and treats the failure
as fatal, so the symptom is a black screen after New Game with
`FS_CreatePath: failed to create path` in the log. Let the engine create them.

---

## 2. Where the plan was wrong

Worth reading before trusting `VR_PORT_PLAN.md` on these points.

**"`renderergl2` already speaks OpenGL ES" — nearly, but it could not start.**
Two defects, neither Android-specific; both would break any GLES target
including the Emscripten build already in tree. `QGL_1_1_PROCS` carried five
entry points that exist in no version of GLES (`glCallList`, `glDrawPixels`,
`glFogi`, `glLineStipple`, `glPixelZoom`), and that list is shared with the ES
path, so `GLimp_GetProcAddresses` failed on every ES context. Separately the ES
3.0 context was only ever requested under `#ifdef __EMSCRIPTEN__`, so EGL
handed back a real ES 2.0 context everywhere else. Both fixed in
`f3ca6230`.

**"FBOs are already first class" — they were switched off on GLES.**
`tr_extensions.c` takes a separate branch for ES that `goto done`s before any
framebuffer setup, leaving `glRefConfig.framebufferObject` false and the whole
`QGL_ARB_framebuffer_object_PROCS` table NULL — despite framebuffer objects
being *core* in ES 2.0. The entry points are now loaded, but the renderer's
*internal* use of them is deliberately left off on Android
(`r_ext_framebuffer_object 0`): in VR each eye is already a swapchain image, so
routing through `tr.renderFbo` first costs a second full-resolution pass and
resolve per eye for nothing.

**"SDL audio may be simpler for a first pass" — there is no SDL audio path.**
`snd_local_new.h` hardcodes `#define SOUND_DRIVER OPENAL`, and `USE_OPENAL=OFF`
does not even compile because `qal.h` is included regardless. OpenAL Soft is
built from source and uses its OpenSL ES backend.

**"Game modules: static or dlopen?" — static is not an option.** `cgame` and
`fgame` both compile `q_shared`, `q_math` and the script system, so linking
them into one binary collides. They ship inside the APK as `libgame.so` /
`libcgame.so` and load off the linker's search path (`DLL_PREFIX` in
`q_platform.h`).

**Quest 3 is Android 14 / API 34**, not 12.

**Do not set the render resolution through cvars.** `r_mode` is `CVAR_LATCH`
*and* registered by the renderer itself, so nothing the client sets beforehand
can win — an ordinary set is deferred, a forced set lands on a cvar that does
not exist yet. The renderer asks the client instead, via
`refimport_t::GetVRRenderResolution`.

---

## 3. How the VR layer is put together

`code/vr/vr_openxr.c`, about 1500 lines, plus `vr_common.h` as the engine-facing
API.

**The session binds to SDL's existing EGL context**, not a private one. This is
the main divergence from RTCWQuest, which uses a NativeActivity and no SDL at
all. One GL context in the process, no thread or context juggling, and the flat
path stays intact.

**Eyes are rendered by substituting the renderer's idea of the screen.**
`re.SetDefaultFramebuffer` points `FBO_Bind(NULL)` at the eye's swapchain image,
so the entire render path lands in the headset without knowing. Exactly two eye
passes per frame; deliberately no third mono pass (plan §3.4).

**The session is torn down and rebuilt around renderer restarts.** It belongs to
the GL context it was created with, and the engine restarts the renderer during
startup. `CL_ShutdownRef` / `CL_StartHunkUsers` bracket it.

**Flat content goes on a quad layer** fixed in the room, placed once in front of
wherever the player is facing, using only their heading so it stays upright.
Menus, loading screens and cinematics all take this path
(`clc.state != CA_ACTIVE`).

**The panel is drawn into one persistent buffer and blitted into the swapchain
image.** A swapchain rotates through 3 images; the engine's 2D path expects the
screen to still hold what it drew last frame. This was the flicker.

**Orientation travels as axes, never Euler angles.** Converting the headset
quaternion to pitch/yaw/roll and composing by *adding* angles only works while
pitch and roll are near zero, and turns every head movement into a twist once
they are not.

**Position is measured from where the head was when tracking started**, and the
starting heading is subtracted too. Otherwise the player's real standing height
stacks on the character's eye height, and the play space's arbitrary forward is
added to the character's heading — which is why the player spawned facing
backwards.

**Head pose is applied in `RE_RenderScene`**, so it follows every camera the
game has — the player's, cutscenes, ladders, death animations — without each
needing to know about VR. That directly serves plan §5's camera audit.

### Debugging

```
vr_traceTracking 1     raw headset pose, tracking origin, world scale
r_vrTrace 1            the composed camera the renderer actually uses
adb logcat -s openmohaa
```

The crash handler (`code/sys/new/sys_android_new.c`) reports the faulting
address and the `pc`/`lr` via `sigaction` + `SA_SIGINFO` on a private
`sigaltstack`. This matters: `_Unwind_Backtrace` cannot walk out of the kernel's
signal trampoline, so before this every crash reported four frames of the
handler and nothing else. Several bugs here were found in one round trip because
of it.

---

## 4. Known problems

**Aim is on the head, not the controller.** Plan §5 is right that this is wrong
for a shooter. `vrInput_t` already carries `weaponYaw`/`weaponPitch` from the
dominant hand, unused. This is the next real piece of work and it means touching
how `cgame` positions the weapon and where shots originate.

**No controller models and no pointer ray.** The menu pointer works — the aim
ray is intersected with the quad and drives the engine's own cursor — but there
is nothing to see. On a quad-only frame there is no 3D scene to draw a beam
into, so it needs a second thin quad layer or a minimal 3D pass.

**Some foliage still rotates with head movement.** `AutospriteDeform` and the
`SPRITE_PARALLEL` / `SPRITE_PARALLEL_UPRIGHT` sprite paths now face the viewer's
position rather than the view plane, which fixed the near foliage and the
imposters behind it. At least one more path does the same thing.
`Autosprite2Deform` was left alone on purpose — it pivots on a fixed axis for
beams and rails and is far less sensitive. Deferred until movement makes it
easier to tell which effects misbehave.

**A redundant renderer restart at startup** tears down and rebuilds the XR
session and swapchains mid-run. Harmless now that both inits agree on
resolution, but wasteful and worth removing.

**A recurring SIGSEGV in the Adreno driver** (`gsl_memory_alloc_pure_64`, fault
address 0x90) on a secondary thread at every launch. It **predates all VR work**
— it was in the flat builds too, on a `Sys_InitPIDFile` → `exit()` →
`__cxa_finalize` path. The process survives it every time. Still unexplained.

**Frame rate has never been measured.** Plan §7's open question 2 is still open,
and now more pressing: sprite facing became per-sprite work, and there are two
full-resolution eye passes at 1680×1760.

---

## 5. What to do next

1. **Controller weapon aim.** §5's headline item. RTCWQuest keeps weapon angles
   entirely separate from the view (`vr.weaponangles`) and drives the engine's
   aim from those while the camera stays on the head. Their
   `VrInputWeaponAlign.c` also does per-weapon calibration, and there is a
   stabilised two-handed mode worth having later.
2. **Measure the frame budget** on a real map. Everything about how ambitious
   phase 4 can be depends on this number.
3. **The HUD.** Still screen-space during gameplay, so it has the same
   non-convergence the menus had before the quad layer. §3.5 and §5 suggest
   wrist- or weapon-mounted readouts.
4. Remaining foliage paths, the redundant renderer restart, the Adreno fault.

### Reference

Team Beef's RTCWQuest is cloned at `/home/berkybear/RTCWQuest`. Same Quake 3
engine lineage, same genre, same headset — the closest possible analogue.
`Projects/Android/jni/RTCWVR/`: `TBXR_Common.c` is their reusable OpenXR layer,
`VrInputDefault.c` the movement and weapon handling, `VrInputWeaponAlign.c` the
per-weapon aim calibration.

The single most valuable thing taken from it so far: **locomotion follows the
off-hand controller, not the gaze** (`controllerYawHeading = offhandYaw −
hmdYaw`). Tying travel to where the player looks means they cannot glance
sideways without veering, which is most of why naive VR movement feels like
being dragged around.
