# OpenMoHAA on Quest 3 — where the port stands

Companion to `VR_PORT_PLAN.md`, which is the original survey and plan, and to
`VR_RENDERER_HANDOFF.md`, which covers the move to the fixed function renderer.
This records what has actually been built, what was learned that contradicts the
plan, and what to do next.

Branch `vr-quest3`. Everything below is committed.

**On any problem this port is trying to solve, read RTCWQuest first.** It is
cloned at `/home/berkybear/RTCWQuest`, clean at `4c714b9`, and it is finished.
That is a standing instruction, not a suggestion; ignoring it has cost more time
here than any single bug. It is also not always live code — read what is *called*,
not what is present.

The reusable form of everything below is in `QUEST_PORTING_GUIDE.md`, which is
written for the next port rather than this one.

---

## 1. What works

The game runs on a Meta Quest 3 in stereo VR at **90 fps**: 6DoF head tracking,
stick locomotion, snap turning, menus on a panel fixed in the room, the weapon in
the player's hand aimed by the controller, and a hand on each controller. A
mission can be started and played.

| Phase | State |
|---|---|
| **1 — Android build** | Done. APK builds, boots, logs to logcat. |
| **2 — Playable flat** | Skipped deliberately; went straight to VR at the user's direction. |
| **3 — OpenXR stereo** | Done. 90 fps, 4–6 ms per eye in the back end. |
| **4 — VR interaction** | Locomotion, snap turn, menu pointer, controller aim, weapon in hand, hands on both controllers. HUD and per-weapon alignment outstanding. |

Confirmed on the device, in order of when each stopped being a question:

- the world renders — terrain, BSP, models, textures, lightmaps, fog, sky
- 90 fps at 4–6 ms per eye, against rend2's 50–60 (`VR_RENDERER_HANDOFF.md` §2)
- `XR_FB_display_refresh_rate` works; the display runs at 90 Hz
- colour is correct, and therefore darker than it was — see §2
- the start map loads straight into the world without touching a menu
- both controllers track 1:1, with no drift on head rotation and no lag on a
  snap turn
- both hands are anchored on their own bones and sit upright at the right size

### Build

renderergl1 on gl4es is the default on Android; renderergl2 is still reachable
with `-DBUILD_RENDERER_GL1=OFF -DBUILD_RENDERER_GL2=ON -DUSE_GL4ES=OFF`.

```sh
cmake -B build-gl1 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=OFF
cmake --build build-gl1 -j$(nproc)

cd misc/android && gradle assembleDebug -PnativeLibsDir=../../build-gl1/apk-libs
cd ../.. && adb install -r misc/android/app/build/outputs/apk/debug/app-debug.apk
adb push misc/android/autoexec.cfg \
  /sdcard/Android/data/org.openmoh.openmohaa/files/main/autoexec.cfg
adb shell am force-stop org.openmoh.openmohaa
```

SDL2, OpenAL Soft, gl4es and the Khronos OpenXR loader are all fetched and built
from source; `-DSDL2_SOURCE_PATH=` and friends point at local trees instead. Full
instructions and the deployment traps are in
`docs/markdown/04-coding/03-android.md`.

**`adb install -r` does not kill the running process**, so force-stop after
installing or the next launch resumes the old build. **Check the APK is newer
than the `.so`** — gradle will report success while packaging the previous
library. **Never `adb shell am start`**; launch from inside the headset.

### Game data

Lives in `/sdcard/Android/data/org.openmoh.openmohaa/files/main/`. Currently
populated from a GOG War Chest install.

**Never create `main/save` or `main/configs` with `adb`.** A directory made by
`adb shell mkdir` belongs to `shell`, and no chmod makes it app-writable — the
app is `other` there. MOH:AA autosaves on mission start and treats the failure
as fatal, so the symptom is a black screen after New Game with
`FS_CreatePath: failed to create path` in the log. Let the engine create them.

### Device configuration

`misc/android/autoexec.cfg`, deployed next to the game data, not in the APK. It
is the only way to set a cvar on a device with no console: exec'd after
`omconfig.cfg`, before `R_Init`, so it beats both the stored config and the
renderer's own defaults. It cannot reach `VR_TuningCvar` cvars, which force their
value at every start.

The test harness in it ships **off**, so the game starts at its own menu. Filling
in `cheats 1`, `vr_testStart "give all"` and `vr_startMap "m4l1"` drops straight
into The Bocage with every weapon, which is how the hands were tuned; the values
and the reasoning for that map are kept in the comments beside them.

Nothing the game needs to be correct is only in this file. It is a tuning and
testing knob — the defaults compiled into the build are the values measured on
device, so an APK installed without it still behaves.

---

## 2. Where the plan was wrong

Worth reading before trusting `VR_PORT_PLAN.md` on these points.

**"Drive `renderergl2` at GLES 3 directly; `renderergl1` is out of scope" — the
opposite happened, and it was the single largest win.** rend2 ran at 10–15 fps
against a 90 Hz display: 50–60 ms of *CPU* per eye in the back end binding GLSL
programs, while the GPU finished in 3 ms. None of what that buys is visible on
art with no normal or specular maps. The port now runs renderergl1 — the plain
Quake 3 forward renderer — on gl4es, at 4–6 ms per eye. The whole of
`VR_RENDERER_HANDOFF.md` is about that swap and the gl4es boundary it dragged in.

**"`renderergl2` already speaks OpenGL ES" — nearly, but it could not start.**
`QGL_1_1_PROCS` carried five entry points that exist in no version of GLES, and
the ES 3.0 context was only requested under `#ifdef __EMSCRIPTEN__`. Both fixed
in `f3ca6230`; both would have broken any GLES target.

**"FBOs are already first class" — they were switched off on GLES**, and are
deliberately left off on Android (`r_ext_framebuffer_object 0`): in VR each eye is
already a swapchain image, so routing through `tr.renderFbo` first costs a second
full-resolution pass and resolve per eye for nothing.

**"SDL audio may be simpler for a first pass" — there is no SDL audio path.**
`snd_local_new.h` hardcodes `#define SOUND_DRIVER OPENAL`, and `USE_OPENAL=OFF`
does not compile. OpenAL Soft is built from source on its OpenSL ES backend.

**"Game modules: static or dlopen?" — static is not an option.** `cgame` and
`fgame` both compile `q_shared`, `q_math` and the script system, so linking them
into one binary collides. They ship as `libgame.so` / `libcgame.so`.

**Quest 3 is Android 14 / API 34**, not 12.

**Do not set the render resolution through cvars.** `r_mode` is `CVAR_LATCH` *and*
registered by the renderer itself, so nothing the client sets beforehand can win.
The renderer asks the client instead, via `refimport_t::GetVRRenderResolution`.

**The world is 52.5 units to the metre, not 32.** MOHAA's art is authored at 16
units per foot — its tik files say so where they convert from centimetres
(`models/weapons/bar.tik`: *"16/30.5 since world is in 16 units per foot"*), and
`MAXS_Z 94` / `DEFAULT_VIEWHEIGHT 82` agree at 1.79 m standing. `vr_worldscale`
was 32 for months, which is the id Tech number for a game built in inches and
simply the wrong engine's. Everything measured in metres was 1.64× too small, and
it presented as *the hand models being too large* rather than as a scale error,
because a correctly sized model drawn too close is the only clue and the world
gives you nothing to compare against.

---

## 3. How the VR layer is put together

`code/vr/vr_openxr.c`, plus `vr_common.h` as the engine-facing API.

**The session binds to SDL's existing EGL context**, made current against a 16×16
pbuffer because SDL unbinds it and reports success when there is no window
surface (`VR_RENDERER_HANDOFF.md` §5.6). One GL context in the process.

**Eyes are rendered by substituting the renderer's idea of the screen.**
`re.SetDefaultFramebuffer` points the renderer at the eye's swapchain image, so
the entire render path lands in the headset without knowing. Exactly two eye
passes per frame; deliberately no third mono pass.

**The session is torn down and rebuilt around renderer restarts**, and
window-resize restarts are declined outright under VR — one of those was
destroying the context five seconds into every run and was the black menu.

**Flat content goes on a quad layer** fixed in the room, drawn into one
persistent buffer and blitted into the swapchain image, because a swapchain
rotates through three images and the 2D path expects last frame's content to
still be there.

**Colour: the swapchains are `GL_SRGB8_ALPHA8` *and* sRGB write conversion is
disabled** via `GL_EXT_sRGB_write_control`. The format alone is half the fix and
looks like the whole of it — ES 3.0 converts on write into an sRGB attachment and
core ES has no switch to stop it, so the engine's already display-referred colour
was encoded a second time. It read as flat lighting. The corrected picture is
considerably darker, and that is correct.

**Orientation travels as axes, never Euler angles**, and position is measured
from where the head was when tracking started with the starting heading
subtracted.

**Head pose is applied in `RE_RenderScene`**, so it follows every camera the game
has — the player's, cutscenes, ladders, death animations.

### Placing things in the world, which is not obvious

`cg.refdef.vieworg` **is not the camera.** `R_VRComposeView` adds the head's play
space offset to it, rotated into the body's frame, after cgame is finished:

```c
bodyAngles = (0, viewYaw - baseYaw, 0);
fd->vieworg += vrView.origin[i] * bodyAxis[i];
```

Anything anchored on `vieworg` alone is displaced by exactly that offset, which
drifts as the head turns and does not survive a snap turn. So both hands are
reported measured from the **tracking origin** — the frame `VR_PoseToView` puts
the eyes in — and composed the same way the renderer composes the camera.

RTCWQuest reaches the same place by the other road: a head-relative offset,
rotated by `viewangles[YAW] - hmdorientation[YAW]` in `convertFromVR`, with its
height replaced by the `-64 + hmdposition[1] * worldScale` rebase. Either works.
**Half of each does not**: a head-relative offset composed as play space leaves
the hand at `vieworg + R*(hand - head)` while the camera is at `vieworg + R*head`,
so the head's motion is counted twice and every hand movement is amplified. That
is written up in `vr_common.h` above both accessors.

### Weapon and hands

**Aim comes from the controller, the camera stays on the head.**
`Weapon::GetMuzzlePosition` takes the controller's pitch and the yaw the hand
leads the head by, added to the view yaw the game already has — the controller's
absolute yaw is meaningless, since the player can snap turn and the play space
has an arbitrary forward. The angles reach `fgame` through `gameImport_t`, which
a dedicated server answers false for.

**The hands are separate surfaces on the first person model** — `triggerhand`,
`lefthand`, `garandhand`, `viewsleeves` on `USarmyplyr.skd` — so a hand per
controller needs no mesh work. The weapon hand's copy drops the left hand surface
and a second copy carries only that. `viewsleeves` is one mesh across both
forearms and cannot be split without re-authoring, so it stays with the weapon
hand.

**Each copy is anchored on a tag, not on the model origin**, by inverting the
composition `Entity::GetTagPositionAndOrientation` uses. The off hand anchors on
`Bip01 L Hand` and the weapon hand on `tag_weapon_right`, since what is held
there is the gun and the controller is its grip. Every candidate is measured and
logged rather than trusted — `tag_weapon_left` was assumed to be an unused slot
at the origin and is a real anchor 23.5 units out.

What is left after the frame and the anchor are right is the wrist bones' own
rotation convention, which is a constant: both hands need a roll of 180 in
`vr_weaponAdjust` / `vr_offHandAdjust`. That is the compiled default for both,
not something the config has to supply — the two hands are mirrored and were
still measured to want the same number, which is why they stay two cvars.

**Two handed hold**: off hand grip with the hands close together points the
weapon along the line between them, roll halved. **Reload** is a tap of the
weapon hand's grip. **Weapon switching** is the dominant thumbstick up and down,
gated on the sideways component and edge triggered.

### Debugging

```
vr_traceTracking 1     raw headset pose, tracking origin, world scale
r_vrTrace 1            the composed camera the renderer actually uses
vr_traceFrame 1        the per frame timing split (on by default; noisy)
vr_captureEye 1        writes the eye buffer to main/vrshotN.tga
```

**Start the logcat capture before handing the headset over.** The buffer is
shared with a shell that prints every five seconds, so a once-only startup line
is gone within a minute or two — that has cost two round trips:

```sh
adb logcat -c && adb logcat -s openmohaa:V > run.log &
```

The crash handler (`code/sys/new/sys_android_new.c`) reports the faulting address
and the `pc`/`lr` via `sigaction` + `SA_SIGINFO` on a private `sigaltstack`.
`_Unwind_Backtrace` cannot walk out of the kernel's signal trampoline, so before
that every crash reported four frames of the handler and nothing else.

---

## 4. Known problems

**It does not hold 90 Hz while moving.** `vr_resolutionScale` is now 1, so the
eyes render at the full 1680×1760, and the frame budget is spent:

| | fps | eye pass |
|---|---|---|
| standing at spawn | 90.0 | 8–9 ms |
| moving in the Bocage | 77–85 | 11–12 ms |

The display period is 11.1 ms, so while moving most frames overshoot it and the
compositor reprojects them. **Kept deliberately** — it plays well, the picture is
much sharper, and the reprojection is not felt. But it is a known trade rather
than headroom, and `vr_resolutionScale` is the first thing to give back if
something later needs the time.

**There is no budget for MSAA.** It is wired up and switched off (`vr_samples 0`)
and it resolves in tile memory on Adreno so it is nearly free on the GPU — but
the GPU is not the constraint, and there is nothing spare on the CPU side. This
stays off until the frame split below is trustworthy.

**The frame split cannot be taken at face value.** It reports `world 1ms` against
`hud 5ms`, which is not credible for a full world render. `Set2DWindow` begins
with `R_IssuePendingRenderCommands()`, so the world's deferred draw calls are
attributed to the first 2D call that forces them out — the trap in
`QUEST_PORTING_GUIDE.md` §7.1. The one trustworthy figure is `gpuwait`, still
**3 ms**: the GPU finishes early and waits, exactly as it did under rend2, so
whatever the 11 ms is, it is CPU issuing draw calls and not fill rate.

**Fixing that attribution is the prerequisite to spending any budget**, because
right now nobody can say where half the frame goes.

**The HUD is still screen space during gameplay**, with the same non-convergence
the menus had before the quad layer.

**Per-weapon alignment is one global setting.** `vr_weaponAdjust` is shared
across every weapon, but a Garand, a Colt and a bazooka will not want the same
grip offset — the reference keeps `vr_weapon_adjustment_<id>` per weapon. The
Bocage hands over a Colt and a Garand at spawn, plus `give all`, which is the
place to find out how far apart they need to be.

**Not confirmed on the device**, and each is only known to compile:

- the two handed hold and the reload gesture
- off hand steering — it was mirrored, and the corrected sign has not been tried
- whether the pistol's `lefthand` surface looks right; only the Garand's
  `garandhand` has been seen

**Some foliage still rotates with head movement.** `AutospriteDeform` and the
`SPRITE_PARALLEL` paths now face the viewer's position rather than the view
plane, which fixed the near foliage and the imposters behind it. At least one
more path does the same thing. `Autosprite2Deform` was left alone on purpose.

**A recurring SIGSEGV in the Adreno driver** (`gsl_memory_alloc_pure_64`, fault
address `0x90`) on a secondary thread at every launch. It **predates all VR
work** — it was in the flat builds too — and the process survives it every time.
Establish which faults are background noise before bisecting anything.

---

## 5. What to do next

1. **Fix the frame split**, so `world` and `hud` mean what they say. Half the
   frame is currently attributed to a flush rather than to the work that caused
   it, and every performance decision after this one depends on knowing where
   the 11 ms actually goes. The GPU idling at 3 ms says there is something real
   to find.
2. **Then decide about MSAA**, and about whether the CPU cost can come down far
   enough to hold 90 Hz at full resolution rather than reprojecting to it.
3. **Confirm the unconfirmed** — the two handed hold, the reload tap, off hand
   steering. All three are one session on the device with the harness already in
   place.
4. **Per-weapon alignment.** Build the per-weapon table once the numbers for two
   or three weapons are known.
5. **The HUD.** Wrist or weapon mounted readouts; `VR_WristPanelEnabled` already
   exists and is off.
6. Remaining foliage paths, and the Adreno fault.

### Reference

Team Beef's RTCWQuest, `/home/berkybear/RTCWQuest`, clean at `4c714b9`.
`Projects/Android/jni/RTCWVR/`: `TBXR_Common.c` is their OpenXR layer,
`VrInputDefault.c` the movement and weapon handling, `VrInputWeaponAlign.c` the
per-weapon calibration. `rtcw/src/cgame/cg_weapons.c` has
`CG_CalculateVRWeaponPosition` and the offhand equivalents;
`rtcw/src/renderer/tr_scene.c` has `convertFromVR`, which is the frame conversion
this port arrives at from the other direction.

Ideas there worth having and not yet taken: **a projection layer and a quad layer
in the same frame** (`TBXR_Common.c:1576`), which is how a pointer ray and
controller models get drawn on a menu-only frame; and **scope zoom by narrowing
the composition layer's FOV** (`TBXR_Common.c:1557`) rather than touching the
projection matrix, which applies directly to the sniper scopes and binoculars.

Not there, so do not go looking: no foveation, no space warp, no performance
level hints. `XR_FB_display_refresh_rate` is vestigial in their tree — the
working implementation here is original.
