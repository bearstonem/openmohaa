# OpenMoHAA on Quest 3 — porting plan

Getting Medal of Honor: Allied Assault running natively on a Meta Quest 3 in
VR, by way of an Android port first.

Written after surveying the tree at `a2f34019`, and carrying the lessons from
a completed Quest 3 VR port of Homeworld (1999) — the mistakes noted at the
end are ones already made once and worth not repeating.

---

## 1. What we are starting from

| | |
|---|---|
| Lineage | ioquake3-derived (`renderergl1`/`renderergl2`, `qcommon`, `botlib`, QVM options) |
| Build | CMake ≥ 3.25, modular per-directory `CMakeLists.txt` |
| Platform layer | **SDL2** (`code/sdl/`: glimp, input, sound, gamma, mouse) |
| Renderers | `renderergl1` (fixed function), `renderergl2` (GLSL) |
| Game logic | C++ (`fgame`, `cgame`, `uilib`, `skeletor`, `tiki`) — not Q3-style QVM gameplay |
| Android support | **none whatsoever** |
| Game data | Retail MOH:AA required (GOG recommended); a demo exists |

### The finding that shapes everything

**`renderergl2` already speaks OpenGL ES.** This is inherited from upstream
ioquake3 and is in far better shape than expected:

- `qglesMajorVersion` / `qglesMinorVersion` are real runtime variables
  (`code/renderercommon/qgl.h`), with a `QGLES_VERSION_ATLEAST` macro
- `tr_glsl.c` emits `#version 300 es` when ES 3 is present, `#version 100`
  for ES 2, and injects `precision mediump float` / `precision mediump
  sampler2DShadow`
- `tr_extensions.c` branches on `qglesMajorVersion` when probing extensions
- `sdl_glimp.c` has an **`r_preferOpenGLES` cvar** and will request
  `SDL_GL_CONTEXT_PROFILE_ES` contexts

This is the single biggest difference from the Homeworld port, where the
engine only spoke fixed-function GL 1.x and needed **gl4es** (a GL-over-GLES2
translation layer) underneath everything — which then dictated every later
constraint. Here the plan is to drive `renderergl2` at GLES 3 directly, so:

- no translation layer, no gl4es quirks
- real shaders, so normal mapping and post-processing stay on the table
- FBOs are already first class (`tr_fbo.c`), which matters enormously for
  stereo rendering — Homeworld's port never got this and it blocked MSAA

**`renderergl1` should be considered out of scope.** It is fixed-function and
would need the same emulation layer we are avoiding. Build only `renderergl2`
(`-DBUILD_RENDERER_GL1=OFF -DBUILD_RENDERER_GL2=ON`).

---

## 2. Phase 1 — build for Android

Goal: an APK that launches, logs, and reaches the main menu. No VR yet.

1. **Toolchain.** CMake has first-class NDK support via
   `android.toolchain.cmake`; unlike Homeworld's hand-written meson cross
   file, this is a supported path. Target `arm64-v8a`, `android-29` or later
   (Quest 3 is Android 12).
2. **SDL2 for Android.** Build SDL2 for `arm64-v8a` and use the stock
   `org.libsdl.app.SDLActivity` to load the engine as `libmain.so`. The
   existing `code/sdl/*` should mostly work unchanged — this is the same
   arrangement the Homeworld port used successfully.
3. **Prune the dependency surface.** Decide early, since each is a
   cross-compile:
   - `USE_OPENAL` — Quest wants low-latency audio; OpenAL Soft builds for
     Android, but SDL audio may be simpler for a first pass
   - `USE_CURL` / `USE_HTTP` — off initially, it is only for downloads
   - `USE_CODEC_VORBIS` / `OPUS` / `MAD` — MOH:AA music needs at least one;
     check what the retail data actually ships
   - `BUILD_SERVER` — off for the client APK
4. **Game modules.** `BUILD_GAME_LIBRARIES` vs `BUILD_GAME_QVMS`. Native
   `.so` modules via `dlopen` work on Android, but Android's linker
   namespaces are fussy about paths — consider **static linking the game
   modules into the main binary** for the first port and revisiting later.
5. **Filesystem.** Point the base path at the app's external files directory
   (`SDL_AndroidGetExternalStoragePath()`), and `chdir()` there early —
   exactly what the Homeworld port does. The user side-loads `main/pak*.pk3`
   into it.
6. **Force the ES path.** Set `r_preferOpenGLES 1` and confirm
   `qglesMajorVersion` comes back as 3. This is the moment of truth for the
   whole approach and should be validated before anything else is built on
   top.

**Exit criteria:** APK boots, logs to `logcat`, reaches the main menu with
retail data side-loaded, renders through `renderergl2` on a GLES 3 context.

---

## 3. Phase 2 — playable flat on the headset

Goal: the game is genuinely playable as a 2D panel app before VR is attempted.
This is deliberately a separate milestone; debugging engine problems is far
easier without a stereo compositor in the way.

- **Input:** Quest Touch controllers appear to SDL as a game controller.
  Map to the existing `sdl_input.c` joystick path first; a bespoke mapping
  comes later with VR.
- **Performance baseline:** measure frame time with the flat renderer before
  stereo triples the cost. Establish what the Quest 3 GPU actually delivers on
  a large MOH:AA map — this number decides how ambitious VR can be.
- **Config and saves:** confirm they write to the external files dir, and that
  the directory is app-owned (see the ownership trap in §6).
- **Audio:** verify no dropouts; Android audio callbacks are less forgiving
  than desktop.

**Exit criteria:** a mission is completable, flat, on the headset, at a stable
frame rate.

---

## 4. Phase 3 — stereo via OpenXR

Goal: the world renders in tracked stereo at 72 fps.

1. **Session and swapchains.** Link the Khronos `openxr_loader` for Android,
   create an instance with `XR_KHR_opengl_es_enable`, and drive
   `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` from the engine's frame loop.
2. **Two eye passes into FBOs.** `renderergl2` already renders into
   framebuffer objects (`tr_fbo.c`), so each eye can target an OpenXR
   swapchain image directly. **This is the piece the Homeworld port never
   built**, and its absence there blocked MSAA and forced awkward blits — do
   it properly from the start here.
3. **View and projection from OpenXR.** Replace the engine's projection setup
   with the per-eye FOV and pose from `xrLocateViews`. In id Tech the camera
   comes from `refdef` — feed eye offsets in there rather than hacking the
   projection matrix downstream.
4. **Do not render the world three times.** The Homeworld port ended up with a
   mono pass plus two eye passes because the 2D UI and camera capture were
   entangled with the main view. Design the frame as *exactly two* eye passes
   from day one; anything needing a flat image should read from an eye buffer
   or render separately.
5. **The 2D UI.** MOH:AA's HUD and menus draw in screen space. Two options,
   probably both eventually: render them to a texture and present as an
   OpenXR quad layer (crisp, comfortable, good for menus), or project them
   into the world (better for the in-game HUD).

**Exit criteria:** stereo world, correct scale and IPD, head tracking with no
mismatch, stable 72 fps, menus readable.

---

## 5. Phase 4 — VR interaction design

This is the largest and least mechanical phase, and it is where an FPS differs
completely from the RTS port. A wrong choice here causes motion sickness
rather than a bug report.

**Locomotion — the central question.**
- Smooth locomotion with comfort options (vignette, snap turn) is the
  expected default for a shooter, but MOH:AA's level design assumes a fast,
  free-running player
- Teleport is comfortable but destroys the pacing of firefights
- Likely: smooth movement, snap *and* smooth turn as options, adjustable
  vignette. Make all of it a cvar so it can be tuned on device.

**Weapon handling.**
- Aim from the controller, not the head — this is non-negotiable for an FPS
- Two-handed hold for rifles is a large fidelity win and a large amount of
  work; consider one-handed first
- Iron sights become genuinely physical: the sight has to line up with the eye
- Reloading: gesture vs button. Button first.

**The player body and camera.**
- MOH:AA moves the view for scripted sequences, cutscenes, ladder climbs and
  death animations. **Any engine-driven camera motion is a nausea risk in
  VR** and every instance needs auditing. This is likely the single largest
  source of unpleasant surprises.
- Player height: real standing height vs the engine's fixed viewheight
- Crouch: physical vs button, ideally both

**HUD.**
- Screen-locked HUD elements are uncomfortable in VR
- Ammo and health on the weapon or wrist, as the Homeworld port did with
  wrist cards — that worked well and is worth reusing
- Objectives and subtitles on a comfortable head-relative panel

**Exit criteria:** a mission playable end to end, comfortably, by someone who
is not the developer.

---

## 6. Traps carried from the Homeworld Quest port

Hard-won, all of them cost hours:

- **Never `adb shell am start` to launch after deploying.** It intermittently
  strands the headset in Meta's loading environment. Install, then launch from
  inside the headset.
- **`adb push` cannot create directories inside an app's data folder**, and
  `chmod` must come *after* pushing or adb fails with `remote fchown failed`.
- **A directory created by `adb shell mkdir` is mode 2770 owned by `shell`**,
  which the app cannot even traverse into — it fails *silently*. `chmod 775`
  afterwards. Files pushed by adb are 644 and world-readable, which is why
  data files work while directories do not.
- **Verify your logging works before relying on it.** A crash handler that has
  been silently displaced reports nothing, and is indistinguishable from a
  crash that never happened. Add a startup self-test that exercises the same
  path.
- **`tombstoned` may not write backtraces to logcat.** When it does not,
  `dumpsys activity exit-info` still reports the signal, and an in-process
  handler using `_Unwind_Backtrace` + `dladdr` can report the stack itself —
  but it needs a real `sigaltstack`, or a stack overflow silences it.
- **Only one process may hold an OpenXR session.** If the previous run has not
  fully exited, the new one falls back to flat mono rendering with no error.
- **The headset must be awake** or the session never reaches
  `XR_SESSION_STATE_FOCUSED` and the launch appears to hang.
- **Wireless adb ports change** every time debugging is toggled, and mDNS can
  advertise a dead one.

---

## 7. Open questions to settle early

1. **Does `renderergl2` actually initialise on a Quest GLES 3 context?**
   Everything above rests on this. Validate in Phase 1 before building more.
2. **What frame budget does a real MOH:AA map leave?** id Tech 3 maps are
   light by modern standards, but MOH:AA's are large and the Quest 3 must
   render everything twice at 72 fps.
3. **Game modules: static or `dlopen`?** Decide before the build is shaped
   around either.
4. **How much engine-driven camera movement is there?** Grep for view
   overrides early; it drives the Phase 4 comfort work and may be the deciding
   factor in how pleasant this is to play.
5. **Which MOH:AA data edition to target first?** GOG is recommended by the
   project and is the easiest for testers to obtain legally.
6. **Licensing.** OpenMoHAA is GPL; no game assets may be redistributed. Same
   posture as the Homeworld port: the user supplies their own `pak` files.

---

## 8. Suggested order of work

```
1  Android build of the dedicated server or a headless client   (proves the toolchain)
2  Full client APK, GLES 3 context, main menu                   (proves the renderer)
3  Side-load retail data, reach gameplay, flat                   (proves the filesystem)
4  Controller input, playable flat                               (baseline + perf numbers)
5  OpenXR session, one eye, correct pose                         (proves the XR path)
6  Two eye FBOs, correct IPD and projection                      (stereo)
7  UI as quad layers                                             (menus usable)
8  Locomotion and weapon handling                                (VR design begins)
9  Comfort pass, camera audit, HUD                               (the long tail)
```

Steps 1–3 are mechanical and mostly build engineering. Step 5 onward is where
the interesting decisions live.
