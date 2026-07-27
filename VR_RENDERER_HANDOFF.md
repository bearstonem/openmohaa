# Swapping the renderer — where this stands and what to do next

Companion to `VR_PORT_STATUS.md`. That file describes the VR layer; this one is
about the move from `renderergl2` to `renderergl1` on gl4es.

Branch `vr-quest3`.

---

## 0. Status at a glance

**Working, on the device:** renderergl1 builds and runs on the Quest 3 on gl4es.
OpenXR session up, 90 fps against a 90 Hz display, both eyes and the flat panel
composited, cutscenes render, controllers and pointer beam render, and **the
engine's own geometry renders** — confirmed by a flat-red override showing the
intro logo screen in red.

**The one open problem:** the **main menu draws nothing**. Same frame path, same
framebuffer, same renderer. The logo screen before the cutscene renders; the
menu after it does not.

**Do not trust anything in section 7 that is not marked as measured.** A long
sequence of engine-side experiments in this session were run against a gl4es
that could not draw a single triangle, and every negative result from them is
worthless. See section 6 — that trap is the single most important thing in this
document.

---

## 1. The reference implementation — read this before solving anything

**Team Beef's RTCWQuest is cloned at `/home/berkybear/RTCWQuest`**, clean at
`4c714b9`.

Same Quake 3 engine lineage, same genre, same headset, same gl4es — and unlike
this port, it is finished. **Go to it first for any problem this port is trying
to solve.** That is a standing instruction, not a suggestion. In this session it
was violated repeatedly and cost most of the elapsed time: the answer to the
single biggest blocker (section 5.8) was sitting in their `Android.mk` the whole
time.

| Path (under `Projects/Android/jni/`) | What |
|---|---|
| `RTCWVR/TBXR_Common.c` | their OpenXR layer: session, swapchains, composition layers, frame submit |
| `RTCWVR/VrInputDefault.c` | movement, gestures, weapon handling |
| `RTCWVR/VrInputWeaponAlign.c` | per-weapon aim calibration |
| `RTCWVR/VrClientInfo.h` | their whole VR state struct |
| `rtcw/src/renderer/` | the fixed function renderer with their GLES changes (`#ifdef HAVE_GLES`) |
| `SupportLibs/gl4es/Android.mk` | **the gl4es build flags. These matter. See 5.8.** |

### Already extracted — do not re-derive

**Aiming** (`rtcw/src/game/g_weapon.c:1904`). Shot direction is computed in the
*game*, where the trace is fired:

```c
VectorCopy(gVR->weaponangles, viewang);
viewang[YAW] = ent->client->ps.viewangles[YAW]
             + (gVR->weaponangles[YAW] - gVR->hmdorientation[YAW]);
```

Pitch and roll come straight from the controller; yaw is the view yaw plus how
far the controller leads the head. The controller's absolute yaw is meaningless.
The camera stays on the head; only the trace moves to the hand.

**6DoF** (`rtcw/src/client/cl_input.c:847`). Head position delta is fed into the
usercmd as movement. Already ported here.

**The HUD.** No wrist panel. Screen space, in the eye buffers, re-centred on
each eye's off-centre frustum, given stereo parallax, scaled in from the edges.

**Hands.** No controller models. Their existing view-model hand, moved to the
controller's real-world offset from the head.

**Ladders.** Nothing at all — RTCW's are contents-based so it works for free.
**Does not transfer**: MOH:AA is entity-based with a view clamp in
`PmoveAdjustViewAngleSettings_OnLadder`.

**Not there, so do not go looking**: no foveation, no space warp, no performance
level hints. `XR_FB_display_refresh_rate` is vestigial in their tree.

---

## 2. Why the renderer was swapped

renderergl2 ran at **10–15 fps against a 90 Hz display**. Taken apart on the
device:

| Phase | Cost |
|---|---|
| `xrWaitFrame` | 0 ms |
| Building the 3D scene | 2–3 ms |
| **Issuing the scene to GL** | **50–60 ms, per eye** |
| GPU, measured with `glFinish` | **3 ms** |

The GPU finishes in 3 ms and waits. Rendering at 36% of the pixels bought only
23% back, so it is not fill rate. It is CPU in the back end issuing draw calls:
rend2 puts every surface through a GLSL program with dozens of uniforms, 139
`GLSL_BindProgram`/`GLSL_SetUniform` call sites in the surface path, once per
surface per eye.

None of what that buys is visible here — this art has no normal or specular
maps. RTCWQuest's renderer contains no GLSL at all: the original Quake 3 forward
renderer on gl4es. That is the stack being copied.

---

## 3. Measurement traps in the *old* renderer investigation

Kept because they are still true of the code.

**`backEnd.pc.msec` is assigned, not accumulated** (`tr_backend.c`). Later empty
flushes overwrite the scene's real number with 0.

**`Set2DWindow` begins with `R_IssuePendingRenderCommands()`**. The 3D scene is
*built* during the world pass but only *executed* when something forces a flush,
and the first thing that does is the HUD. The whole frame's rendering therefore
appears to any timer to be HUD cost.

**`tr.frontEndMsec` only covers `RE_RenderScene`**, not the cgame work around it.

---

## 4. Build

```sh
cmake -B build-gl1 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=OFF
cmake --build build-gl1 -j$(nproc)

cd misc/android && gradle assembleDebug -PnativeLibsDir=../../build-gl1/apk-libs
cd ../.. && adb install -r misc/android/app/build/outputs/apk/debug/app-debug.apk
```

renderergl1 + gl4es is the default on Android. renderergl2 is still reachable
with `-DBUILD_RENDERER_GL1=OFF -DBUILD_RENDERER_GL2=ON -DUSE_GL4ES=OFF` and
still builds.

---

## 5. What was wrong, and what fixing it required

All committed. Each of these was a hard blocker in its own right.

### 5.1 renderergl1 had never compiled

`cmake/platforms/android.cmake` held `set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL "")`.
**`CACHE INTERNAL` implies `FORCE`**, so the `-D` on the command line was
overwritten on every configure. The cache read `BUILD_RENDERER_GL1:INTERNAL=OFF`
and `build.ninja` never mentioned renderergl1. A previous handoff claimed this
renderer "compiles clean"; it had never been compiled at all. **Check the cache,
not the command line.**

### 5.2 gl4es was building into the source tree, under a name Android cannot load

gl4es's own CMakeLists sets `CMAKE_LIBRARY_OUTPUT_DIRECTORY` to
`${CMAKE_SOURCE_DIR}/lib`, which under FetchContent is *this* project's source
root. It also forces the suffix `.so.1`. Android has no versioned sonames and
the package manager extracts only `lib*.so`. Redirected into the build tree as
`libgl4es.so` and staged with the other APK libraries.

### 5.3 The renderer's GL entry points must come from gl4es

renderergl1 does not call GL by name. `qgl_linked.h` is dead code that nothing
includes; `renderercommon/qgl.h` declares every entry point as a **function
pointer** filled by `GLimp_GetProcAddresses` from `SDL_GL_GetProcAddress`.

On **EGL 1.5, which the Quest is**, `SDL_EGL_GetProcAddress` tries
`eglGetProcAddress` *before* the library it loaded. The driver answers for every
name OpenGL 1.x and ES have in common — `glEnable`, `glBindTexture`,
`glDrawArrays`, `glTexImage2D`, ~80 more — so only desktop-only names
(`glBegin`, `glMatrixMode`) would reach gl4es. The renderer now resolves through
`GLimp_GetProcAddress`, straight to the gl4es handle.

The **extension string** has the same problem: `SDL_GL_ExtensionSupported` reads
the driver's ES list, which names none of the desktop extensions the renderer
asks after, so `GL_ARB_multitexture` read as absent and the renderer would have
dropped to one texture unit.

### 5.4 The fixed function path asked for a context that does not exist

`GLimp_SetMode`'s `fixedFunction` branch offered exactly one context: **desktop
OpenGL 1.1**. There is no such thing on Android; window creation failed and
R_Init gave up with `Couldn't get a visual`. renderergl2 never met this because
it takes the other branch.

Under `USE_GL4ES` that branch now asks for **ES 3.0 then ES 2.0**. The renderer
is never told: it reads its version through gl4es, which reports desktop GL
(`2.1 gl4es wrapper 1.1.6`), and takes the desktop fixed function path on the
strength of it. ES underneath, desktop GL above — the whole point of the layer.

### 5.5 gl4es answers `glGetString` from constants

`GL_VERSION`, `GL_VENDOR` and `GL_RENDERER` come out of gl4es's own globals
without the driver being asked. The engine's only check that a context is live is
`GL_VERSION` coming back non-NULL, so under gl4es that check passes
unconditionally and a context created but never bound sails through `R_Init`.
`GLimp_SetMode` now asks SDL directly instead.

### 5.6 SDL unbinds the context and reports success

`SDL_EGL_MakeCurrent`, handed no surface and without `gl_allow_no_surface`:

```c
if (!egl_context || (!egl_surface && !_this->gl_allow_no_surface)) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
return 0;   /* success, either way */
```

In a headset there is no window being presented, so that surface never arrives.
SDL then records the context as current, and SDL and EGL disagree permanently.
Asking SDL to bind it again does nothing — `SDL_GL_MakeCurrent` sees its own
bookkeeping agree and returns early with `/* We're already current. */`.

RTCWQuest never meets this because it never depends on a window surface: it makes
its context current against a **16×16 pbuffer** (`TBXR_Common.c`,
`egl->TinySurface`). Same here, with the context SDL already made. The pbuffer
outlives the session deliberately and is released in `VR_Shutdown`.

### 5.7 gl4es must own the `GL_FRAMEBUFFER` binding — and only that

gl4es keeps its own table of framebuffers it created and **renders from that,
not from the driver's binding**:

```c
gles_glBindFramebuffer(GL_FRAMEBUFFER, current_fb->id ? current_fb->id : mainfbo_fbo);
```

A framebuffer from the driver's `glGenFramebuffers` is not in that table, so
`gl4es_glBindFramebuffer` raises `GL_INVALID_VALUE`, **returns without binding**,
and leaves `current_fb` at zero. Measured directly:

```
fbo: gl4es says 0, driver says 10 | driver viewport 756 -71 403 1126
```

Zero in this process is the 16×16 pbuffer, and the viewport lies entirely outside
it — so geometry landed in nothing, with no error raised anywhere.

The VR layer now has gl4es generate, bind and delete these. **Three things must
stay with the driver:**

- the **swapchain texture attachment** — gl4es cannot attach a texture the
  OpenXR runtime created. RTCWQuest hits this and documents it at
  `TBXR_Common.c:290`, describing our exact symptom: *"the engine then renders
  into nothing -> black eye buffer"*. (They must bypass gl4es explicitly because
  ndk-build puts gl4es first in their link order; ours is already native because
  `libGLESv3` precedes `libgl4es` in `DT_NEEDED`.)
- the **blit's `GL_READ_FRAMEBUFFER` / `GL_DRAW_FRAMEBUFFER` bindings** — gl4es
  takes `GL_READ_FRAMEBUFFER` as a note to itself and `return`s without binding
  (`framebuffers.c:238`). Routing the blit through it silently empties the blit
  and **kills the cutscenes**. This was done once by accident with an
  over-broad macro; do not repeat it.
- `glFramebufferTexture2D`, per the first point.

`RE_SetDefaultFramebuffer` in renderergl1 binds **nothing**. It exists only to
flush: gl4es batches geometry and issues it lazily, so work built for one target
would otherwise arrive in whichever is bound when it finally goes out. The VR
layer calls it *before* its own bind for that reason.

### 5.8 gl4es must be built the way the reference builds it

**This was the largest single blocker and it is the thing to remember.**

With CMake's own flags, gl4es **rendered no geometry whatsoever** while clearing
the same framebuffer perfectly. Measured, in the same buffer microseconds apart:

```
gl4es draw test: 144 red (clear), 0 green (drawn), 0 other
```

Matching RTCWQuest's `Android.mk` fixed it outright, with no engine-side change:

```
gl4es draw test: 108 red (clear), 36 green (drawn), 0 other
```

(36/144 is exactly the quarter the test quad covers.)

Their flags, now in `cmake/libraries/gl4es.cmake`:

```
-DBCMHOST -DNOX11 -DNO_GBM -DDEFAULT_ES=2 -DNO_INIT_CONSTRUCTOR
-O3 -fcommon -fvisibility=hidden -funwind-tables
```

**`ANDROID` is deliberately not among them.** gl4es's own CMakeLists adds
`-DANDROID` whenever the NDK toolchain is in use, and ndk-build never does — so
the reference has only ever been exercised without it. It is not cosmetic: it
selects different code in the loader, in glx, and in `hardext`, which is where
gl4es compiles a probe shader (`testGLSL`) to decide what its fixed function
pipeline may emit. It comes from `add_definitions()`, a **directory** property,
so it must be removed at the directory it was set on — overriding it on the
target does not work.

**Still to do: bisect which flag actually mattered.** `-DANDROID` is the strong
suspicion. This is currently convergence on a known-good configuration, not a
diagnosis.

---

## 6. The trap that cost this session, in bold

**Every engine-side experiment run before 5.8 was worthless, because gl4es could
not draw anything at all.**

Under that broken gl4es, *any* test that ends "and nothing was drawn" returns the
same answer regardless of what it is testing. On the strength of such results
this session wrongly concluded, and acted on:

- that 32-bit indices were the problem (changed to 16-bit — see 8.1)
- that the `glDrawElements` client-array path was broken (switched to
  `qglArrayElement`; no change)
- that compiled vertex arrays were broken (disabled them — see 8.2)
- that `glDrawBuffer(GL_BACK)` on an FBO was discarding everything (stubbed it —
  see 8.3)
- that immediate mode worked while arrays did not, and later that *neither*
  worked — two contradictory conclusions, both from invalid data

**Before believing any negative rendering result, prove the renderer can draw at
all.** The cheapest possible check, and the one that finally cracked this:
ask gl4es for a `glClear` and then a flat untextured quad into the same buffer,
and read the pixels back. A clear needs no shaders; a draw needs the whole
pipeline. If the clear lands and the draw does not, stop looking at the engine.

A second, dumber version of the same mistake: a probe gated on
`primitives == 2` silently stopped running when a separate change flipped
`primitives` to 1, and two rounds of "nothing drawn" results were actually
"the probe never executed". **Verify instrumentation still fires after changing
anything it depends on.**

---

## 7. What is now positively proven working

Measured on the device, after 5.8. Do not re-investigate these.

- The OpenXR session, both eye swapchains and the flat quad layer. 90 fps.
- The panel framebuffer is real, complete (`GL_FRAMEBUFFER_COMPLETE`), writable
  and composited — a clear to blue showed as blue in the headset.
- gl4es reaches that framebuffer: a clear to red showed as red.
- gl4es renders geometry: a flat quad rendered, verified by pixel readback.
- **The engine's own geometry renders** — with a flat-red override, the intro
  logo screen before the cutscene appeared red in the headset.
- Cutscenes render (immediate mode, `RE_StretchRaw`).
- Controllers and the pointer beam render (VR layer's own GLES).
- Both layers agree on the framebuffer: `gl4es says 10, driver says 10`.
- The 2D setup is correct: viewport `0 0 1008 1056`, ortho `0 1008 1056 0`,
  full scissor, `vrView.active` false during menus (so the HUD convergence code
  in `R_VRAdjust2DOrtho` is *not* involved in the menu at all).
- The geometry submitted for menu widgets is correct: 4 verts, 6 strip indices,
  sensible coordinates, identity modelview, a correct ortho matching the
  widget's own viewport, `glGetError() == 0` on every draw.

---

## 8. The open problem

**The logo screen renders. The menu does not.** Both are flat 2D on the quad
layer, `clc.state != CA_ACTIVE`, same framebuffer, same renderer, same frame
path (`VR_UseScreenLayer()` → `VR_PrepareScreenLayer` → `UpdateStereoSide` →
`re.EndFrame` → `VR_FinishScreenLayer`).

A non-destructive 16×16 grid readback of the panel during the menu reports
`0/256 samples lit` — nothing is written at all, not even black-on-black.

### Ruled out this session

- **The scissor.** `RE_Scissor` enables `GL_SCISSOR_TEST` and nothing ever
  disables it, which looked like an excellent candidate for clipping the whole
  UI away. Disabling it entirely changed nothing. (The test build for this is
  still in the tree — see 9.)

### Where to look next

The menu is the only thing that draws **per widget**, each setting its own
viewport and ortho through `re.Set2DWindow` from `cl_ui.cpp:1722` and
`uiwidget.cpp`. A logged example:

```
viewport 756 -71 403 1126   ortho 0 403 1126 0
```

Note `y = -71` and a height of 1126 in a 1056-tall buffer. Those come from
`uid.vidWidth`/`uid.vidHeight`, which is the UI's idea of the screen. **Check
what those are set to against the 1008×1056 eye buffer** — if the UI is laying
out for a different surface size, widget rectangles can land wholly outside the
panel. That is the most promising untested lead.

Also worth doing, cheaply:

- Put the flat-red override back and walk from the logo screen into the menu,
  watching where red stops appearing. The transition is the useful signal, and
  the logo screen proves the machinery works either side of it.
- Log every distinct viewport/ortho the menu sets over one second (deduplicated,
  not sampled once a second — sampling has already misled here twice).
- Check whether `Draw_StretchPic` (used by the logo) and `Draw_TilePic` /
  `Draw_TilePicOffset` (used by widgets) differ in a way that matters; note the
  latter divide by `uploadWidth`/`uploadHeight` for texcoords, which is
  meaningless if the image failed to upload.

---

## 9. Diagnostic scaffolding currently in the working tree

**None of this is committed. It must be removed or decided before shipping.**

| File | What | Verdict |
|---|---|---|
| `renderergl1/tr_shade.c` | flat-red override in `R_DrawElements` (kills texture, blend, colour array) | **remove** — diagnostic only |
| `renderergl1/tr_draw.c` | `RE_Scissor` disabled + logging | **remove** — scissor was ruled out, restore the real function |
| `code/vr/vr_openxr.c` | panel grid-scan readback in `VR_FinishScreenLayer` | **remove** — diagnostic only |

Three uncommitted changes are **not** scaffolding and need a decision:

**9.1 `renderergl1/tr_local.h` — 16-bit indices.** `GL_INDEX_TYPE` changed to
`GL_UNSIGNED_SHORT` under `USE_GL4ES`. Justified independently: ES 2.0 has no
32-bit element indices without `GL_OES_element_index_uint`, and RTCWQuest does
exactly this under `HAVE_GLES`. Costs nothing — indices only address within one
tess batch and `SHADER_MAX_VERTEXES` is 2048 (theirs is 6000 with shorts).
**Keep**, but note it was adopted on invalid evidence and has never been shown to
be necessary.

**9.2 `code/sdl/sdl_glimp.c` — compiled vertex arrays disabled under gl4es.**
Adopted on invalid evidence (section 6). `qglLockArraysEXT` is currently never
resolved, which also forces `r_primitives` to route 1 (`qglArrayElement`) instead
of route 2 (`glDrawElements`). **Re-test both ways now that gl4es works.** The
reference leaves CVA enabled.

**9.3 `code/sdl/sdl_glimp.c` — `qglDrawBuffer`/`qglPolygonMode` stubbed under
gl4es.** Independently defensible (the engine already stubs both on its ES path,
and `GL_BACK` is not something an FBO has), but adopted on invalid evidence and
never shown to be necessary. **Keep, low risk**, but do not credit it with
anything.

Also uncommitted: the `VR_BindFramebuffer` / `VR_GenFramebuffers` helpers in
`vr_openxr.c` implementing 5.7. **These are real and should be committed.**

---

## 10. Working notes

**Device.** `adb connect 192.168.1.92:43105`. It sleeps and the connection dies;
`adb disconnect` then `adb connect`. **Launch from inside the headset** — never
`adb shell am start`.

A logcat wrapper that survives the sleep/wake cycle is worth keeping around; the
connection drops constantly and a plain `adb logcat` dies with it.

**Archived cvars beat a changed default.** The config is exec'd before `VR_Init`,
so `Cvar_Get` finds the cvar already there and keeps the old value. `VR_TuningCvar`
in `vr_openxr.c` exists for this: it registers unarchived and forces the default
every start. There is no console in a headset to correct a stale value with.

**There is no console in a headset.** Every question has to be answered by
something written to logcat, or by something visible enough to describe. Prefer
pixel readbacks logged as numbers over anything that has to be seen — asking
someone to catch a one-frame flash is a bad test, and was one here.

**Settled, so not re-investigated:**

- `XR_FB_display_refresh_rate` works; the display runs at 90 Hz. Ask for it from
  the frame loop with the two-call enumeration, not at `xrBeginSession`.
- MSAA via `GL_EXT_multisampled_render_to_texture` works and is off
  (`vr_samples 0`).
- `vr_resolutionScale` is at **0.6** and should go back to 1 once the picture is
  up — fill rate was never the problem.
- A recurring `Fatal signal 11` at address `0x90` on a secondary thread at every
  launch **predates all VR work** and the process survives it every time.

**Once the menu draws**, the number this whole swap was for is still unmeasured:
renderergl1's back-end cost per eye against rend2's 50–60 ms. Section 2 is the
baseline to beat. The instrumentation for splitting a VR frame into
wait/scene/issue/gpu is described in `VR_PORT_STATUS.md`.
