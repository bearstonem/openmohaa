# Swapping the renderer — where this stands and what to do next

Companion to `VR_PORT_STATUS.md`. That file describes the VR layer; this one is
about the decision to stop using `renderergl2` and what it takes to finish.

Branch `vr-quest3`. Everything described as done is committed and installed on
the headset; **it has not been seen running yet**.

---

## 1. The reference implementation — read this before solving anything

**Team Beef's RTCWQuest is cloned at `/home/berkybear/RTCWQuest`**, clean at
`4c714b9`.

Same Quake 3 engine lineage, same genre, same headset — and unlike this port, it
is finished. **Go to it first for any problem this port is trying to solve.**
That is a standing instruction, not a suggestion: the wrist panel here was built
and iterated over five build cycles before anyone checked how they handle the
HUD, and the answer turned out to be that they do not use a wrist panel at all.
Several other answers were derived slowly and then found sitting in their tree.

### Where things are

| Path (under `Projects/Android/jni/`) | What |
|---|---|
| `RTCWVR/TBXR_Common.c` | their reusable OpenXR layer: session, swapchains, composition layers, frame submit |
| `RTCWVR/VrInputDefault.c` | movement, gestures, weapon handling |
| `RTCWVR/VrInputWeaponAlign.c` | per-weapon aim calibration, positional movement |
| `RTCWVR/VrClientInfo.h` | the whole VR state struct — read this first to see what they bother to track |
| `rtcw/src/` | the game and the original renderer, with their VR changes already in place |
| `SupportLibs/gl4es/` | the vendored translation layer, and the `Android.mk` whose flags this port copies |

### Already extracted — do not re-derive

**Aiming** (`rtcw/src/game/g_weapon.c:1904`). The shot direction is computed in
the *game*, where the trace is fired:

```c
VectorCopy(gVR->weaponangles, viewang);
viewang[YAW] = ent->client->ps.viewangles[YAW]
             + (gVR->weaponangles[YAW] - gVR->hmdorientation[YAW]);
```

Pitch and roll come straight from the controller; yaw is the view yaw plus how
far the controller leads the head. The controller's absolute yaw is meaningless —
the play space's heading is arbitrary — so only the offset from the head carries
information. `weaponangles_knife` and `offhandweaponangles` are separate sets.
The camera stays on the head; only the trace moves to the hand.

**6DoF** (`rtcw/src/client/cl_input.c:847`). Head position delta is fed into the
usercmd as movement, added to the stick. A camera offset on its own lets the
player lean through a wall and never actually travel. Already ported here.

**The HUD.** No wrist panel. Screen space, in the eye buffers, with three
corrections: re-centred on each eye's off-centre frustum, given stereo parallax
so it converges at `cg_hudDepth`, and scaled in from the edges by `cg_hudScale`.

**Hands.** No controller models and no hand assets. `cgs.media.handModel` is
RTCW's existing view-model hand, moved to the controller's real-world offset from
the head (`convertFromVR`).

**Weapon selection.** A 3D wheel anchored to the controller
(`cg_weapons.c:4623`): it snapshots the controller position when opened, then
measures displacement from that point to choose a segment. Items come from a
backpack gesture — reach over the shoulder, detected by distance from the HMD,
height offset, and the hand-forward against head-forward dot product going
negative. `vr.weapon_stabilised` is a two-handed hold; `vr.scopeengaged` is that
weapon brought near the face, which is how they do ADS without a button.

**Ladders.** Nothing at all — the only commit touching their ladder code is the
original source import. RTCW's ladders are contents-based and their view angles
already carry the head, so it works for free. **This one does not transfer**:
MOH:AA is entity-based with a view clamp in `PmoveAdjustViewAngleSettings_OnLadder`.
See `Player::PlayerAngles` for how that is handled here.

**Not there, so do not go looking**: no foveation, no space warp, no
performance-level hints, and `XR_FB_display_refresh_rate` is vestigial — its
entry points are declared, nulled and never resolved, and `TBXR_GetRefreshRate`
returns a hardcoded 90.

---

## 2. Why

The port ran at **10–15 fps against a 90Hz display, every frame late**. The frame
was taken apart phase by phase on the device until there was one number left:

| Phase | Cost |
|---|---|
| `xrWaitFrame` | 0 ms |
| Building the 3D scene (cgame + renderer front end) | 2–3 ms |
| cgame's HUD (`CG_Draw2D`) | 0 ms |
| Font strings (~14 a frame) | ~2 ms |
| Compositor submit | 0 ms |
| **Issuing the scene to GL** | **50–60 ms, per eye** |
| GPU, measured with `glFinish` | **3 ms** |

The GPU finishes in 3 ms and then waits. Rendering at 60% resolution — 36% of
the pixels — bought only a 23% frame time reduction, so it is not fill rate
either. The cost is **CPU, in the back end, issuing draw calls**.

`renderergl2` is the rend2 rewrite. Every surface goes through a GLSL program
with dozens of uniforms set on it — 139 `GLSL_BindProgram`/`GLSL_SetUniform`
call sites in the surface path — once per surface, per frame, per eye. That is
where the 55 ms goes.

None of what it buys is visible here. This game's art was authored for a fixed
function renderer: there are no normal maps or specular maps for rend2's
lighting to read, so it pays in full and returns a picture *further* from the
original than the plain path gives. The game looks worse than RTCWQuest and runs
at a fraction of the speed.

**RTCWQuest's renderer contains no GLSL at all.** It is the original Quake 3
forward renderer, running on **gl4es**, a GL1.x→OpenGL ES translation layer they
vendor. That is the stack to copy, and openmohaa already ships `renderergl1`.

---

## 3. Measurement traps, so nobody repeats them

Three things actively lied during the investigation. All three cost a run on the
device.

**`backEnd.pc.msec` is assigned, not accumulated** (`tr_backend.c`). It is set at
the end of each `RB_ExecuteRenderCommands`. The scene's big execution sets it to
~55, then later empty flushes overwrite it with 0, and `RE_EndFrame` reads the
last one. It reported 0 ms while the back end was eating the entire frame.

**`Set2DWindow` begins with `R_IssuePendingRenderCommands()`** (`tr_draw.c:502`).
The 3D scene is *built* during the world pass but only *executed* when something
forces a flush — and the first thing that does is the HUD's `set2D()`. So the
whole frame's rendering appears, to any timer, to be HUD cost. It is not.

**`tr.frontEndMsec` only covers `RE_RenderScene`**, not the cgame work around it.
A small front end number does not mean cgame is cheap; here it happened to be,
but that had to be measured separately.

Two hypotheses died on contact with data and are recorded so they are not
revisited: the masked alpha clear in `VR_FinishEye` (refuted — cost varies with
scene, a fixed stall would not), and the per-string command buffer flush in
`R_DrawString_sgl` (refuted — ~100–300 calls a second, ~2 ms a frame).

---

## 4. What is done

- **`cmake/libraries/gl4es.cmake`** — fetches gl4es v1.1.6 from
  `github.com/ptitSeb/gl4es` via FetchContent, matching the pattern used for
  SDL2/OpenAL/OpenXR, with `GL4ES_SOURCE_PATH` to build against a local tree.
  Uses RTCWQuest's exact flags: `NOX11`, `NO_GBM`, `DEFAULT_ES=2`,
  `NO_INIT_CONSTRUCTOR`.
- **`USE_GL4ES`** option in `CMakeLists.txt`, defaulting on for Android, included
  from `cmake/libraries/all.cmake`.
- **`renderergl1` builds for arm64 against gl4es's `<GL/gl.h>`, and is the
  default renderer on Android.** The whole of the VR work is now in it: eye
  redirect, head pose, asymmetric frustum, HUD convergence, viewer-facing
  sprites.
- **The APK ships `libgl4es.so`** and the renderer's GL entry points resolve to
  it.

```sh
cmake -B build-gl1 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=OFF
cmake --build build-gl1 -j$(nproc)

cd misc/android && gradle assembleDebug -PnativeLibsDir=../../build-gl1/apk-libs
```

renderergl2 is still reachable with
`-DBUILD_RENDERER_GL1=OFF -DBUILD_RENDERER_GL2=ON -DUSE_GL4ES=OFF`, and still
builds.

### What section 5 said, and what was actually wrong

Recorded because each of these hid the next, and the build succeeding said
nothing about any of them.

**renderergl1 had never compiled.** The claim above that it did was false.
`cmake/platforms/android.cmake` held `set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL
"")`, and **`CACHE INTERNAL` implies `FORCE`** — so the `-D` on the command line
was overwritten on every configure. The cache read `BUILD_RENDERER_GL1:INTERNAL=OFF`,
`build.ninja` never mentioned renderergl1, and the build that "succeeded" was a
renderergl2 build with gl4es sitting unused beside it. Check the cache, not the
command line.

**gl4es was building all along**, into `lib/` *in the engine's source tree* —
gl4es's own CMakeLists sets `CMAKE_LIBRARY_OUTPUT_DIRECTORY` to
`${CMAKE_SOURCE_DIR}/lib`, and under FetchContent `CMAKE_SOURCE_DIR` is the
top-level project. It also forces the suffix `.so.1` after the fashion of a
system OpenGL, which Android will neither package nor load. `RENDERER_LIBRARIES`
did reach the client (`cmake/client.cmake:136`); that part was never broken.

**The link was never the question.** `renderergl1` does not call GL by name.
`qgl_linked.h` is dead code that nothing includes; `renderercommon/qgl.h`
declares every entry point as a *function pointer*, filled in by
`GLimp_GetProcAddresses` from `SDL_GL_GetProcAddress`. So `DT_NEEDED` order was
irrelevant and gl4es would have been linked, loaded, and never called.

Worse than never called: on **EGL 1.5, which the Quest is**,
`SDL_EGL_GetProcAddress` tries `eglGetProcAddress` *before* the library it
loaded. Adreno answers for every name OpenGL 1.x and ES have in common —
`glEnable`, `glBindTexture`, `glDrawArrays`, `glTexImage2D`, ~80 more — so only
the desktop-only names (`glBegin`, `glMatrixMode`) would have reached gl4es.
gl4es would have been batching geometry and tracking a matrix stack against
calls the driver was never told about. The renderer now resolves through
`GLimp_GetProcAddress`, which goes straight to the gl4es handle.

The extension *string* had the same shape of problem:
`SDL_GL_ExtensionSupported` reads the driver's ES list, which names none of the
desktop extensions the renderer asks after, so `GL_ARB_multitexture` read as
absent and the renderer would have quietly dropped to one texture unit.

---

## 5. What remains, in order

### 5.1 Does it run

Everything below section 4 is built and installed and has **not yet been seen
on the device**. That is the only open question that matters; the rest of this
section is what to look at when it answers.

Two things are worth knowing before reading a failure:

- gl4es reports `GL_VERSION` as a *desktop* string (`"2.1 gl4es wrapper
  1.1.6"`), so `GLimp_GetProcAddresses` takes the desktop fixed-function branch
  rather than the ES one. That is intended. If the log shows the ES path, gl4es
  is not answering `glGetString` and nothing else will work either.
- `initialize_gl4es()` is called on the first proc-address request, which is
  just after `SDL_GL_CreateContext`. RTCWQuest calls it earlier still, before
  any context exists, so a context being current is not a requirement.

### 5.2 gl4es alongside the VR layer's own GLES calls

`code/vr/vr_openxr.c` calls GLES directly - swapchain framebuffers, the pointer
beam, the panel resolve - while the renderer goes through gl4es on the same
context. **RTCWQuest does exactly this** (`TBXR_Common.c` binds framebuffers,
clears and blits directly while the engine renders through gl4es), so the
arrangement is proven; do not redesign it on suspicion.

Three things about that boundary were learned the hard way and should not be
re-derived.

**gl4es cannot bind a framebuffer it did not create.** `gl4es_glBindFramebuffer`
looks the name up in gl4es's own table (`find_framebuffer`); for a name from the
driver's `glGenFramebuffers` - which is every framebuffer the VR layer owns - the
lookup misses, it raises `GL_INVALID_VALUE`, **returns without binding**, and
goes on believing framebuffer 0 is current. Routing the eye redirect through
gl4es therefore does nothing at all, and the symptom is not an error: it is a
black headset, working audio, and the frame loop reporting a contented 90 fps
while the whole game draws into the window. The renderer must bind nothing; the
VR layer's own direct bind is what selects the eye. That is what RTCWQuest does,
because Quake 3's fixed function renderer has no framebuffer calls in it at all.

**But the renderer still has to be told, for the flush.** gl4es batches geometry
and issues it lazily, so work built for one target arrives in whichever is bound
when it finally goes out. `RE_SetDefaultFramebuffer` exists now only to flush -
it binds nothing - and the VR layer calls it *before* its own bind so the flush
lands while the old target is still current.

**gl4es answers `glGetString` from constants.** `GL_VERSION`, `GL_VENDOR` and
`GL_RENDERER` come out of its own globals without the driver being asked, so the
engine's long-standing proof that a context is live - `GL_VERSION` coming back
non-NULL - passes unconditionally under gl4es. A context created but never bound
sails through `R_Init` and is only noticed somewhere else entirely. `GLimp_SetMode`
now asks SDL directly instead.

Not established: whether gl4es's cached state survives the VR layer's direct
`glDisable(GL_SCISSOR_TEST)` / `glColorMask` / `glViewport`. Quake 3's backend
re-issues those every frame and RTCWQuest gets away with it, so this is a suspect
to remember rather than a bug to fix in advance.

### 5.2.1 The EGL context

SDL binds the GL context against the *window's* EGL surface, and in a headset
there is no window being presented - so that surface never arrives. What SDL does
then is the trap: `SDL_EGL_MakeCurrent`, handed no surface and without
`gl_allow_no_surface`, calls `eglMakeCurrent(EGL_NO_SURFACE, EGL_NO_CONTEXT)` to
unbind everything **and returns success**. SDL then records the context as
current, so SDL and EGL disagree permanently, and asking SDL to bind it again
does nothing because `SDL_GL_MakeCurrent` sees its own bookkeeping agree and
returns early.

RTCWQuest never meets this because it never depends on a window surface: it makes
its context current against a 16x16 pbuffer (`TBXR_Common.c`, `egl->TinySurface`).
`VR_CreateSession` now does the same with the context SDL already made. The
pbuffer outlives the session on purpose and is released in `VR_Shutdown`.

### 5.3 Then the things the renderer swap was for

- `vr_resolutionScale` back to 1 - fill rate was never the problem.
- Measure the frame again. The whole point was the 50-60 ms of CPU in rend2's
  back end; the number to beat is that, not the GPU's 3 ms.
- Section 7's instrumentation comes out once the number is in. It is still
  wanted for the run that produces it - especially the `glFinish` in
  `VR_FinishEye`, which is what separates work from queueing.

## 6. Built but never deployed

Both are in the tree, both are independent of the renderer choice, both should
come along whichever renderer wins. They were built when the headset went to
sleep and never reached the device.

**The HUD.** `R_VRAdjust2DOrtho` centres flat content on the eye's own axis,
gives it stereo parallax so it converges at `vr_hudDepth` (2 m), and scales it in
from the edges by `vr_hudScale` (0.55). It is called from **both** `RB_SetGL2D`
*and* `Set2DWindow` — the second one matters, because everything the UI draws
goes through `Set2DWindow`, which builds its own matrix and never touches
`RB_SetGL2D`. Correcting only one corrects nothing visible. The HUD was invisible
before this because `RB_SetGL2D`'s early out keys on the bound framebuffer, which
is always NULL when the renderer's own FBO support is off — so it ran once, with
no frustum yet, and left a NaN in the projection.

**Tree imposters.** `Autosprite2Deform` took its facing from the view plane once
per batch; it now takes it per sprite from the viewer's position. On a monitor
those are the same thing. In a headset the imposter swings with the head while
the tree beside it holds still — the second, worse tree leaning out of every
real one.

---

## 7. Instrumentation to remove

All temporary, all in the way, none of it wanted once the renderer question is
settled:

- `VR_TraceFrameTiming` and the whole `phase*` / `trace*` block in
  `code/vr/vr_openxr.c`, plus `VR_TraceRenderTimes` / `VR_TraceSceneTimes` /
  `VR_TraceViewTimes` / `VR_TraceHudTimes` / `VR_TraceHudParts` /
  `VR_TraceEvent` / `VR_TraceState` and their declarations in `vr_common.h`.
- The `glFinish()` in `VR_FinishEye`. **Expensive by design** — it exists to tell
  work from queueing and must not ship.
- `R_FontTraceBegin` / `R_FontTraceEnd` in `renderergl2/tr_font.cpp`.
- The timing calls in `cl_scrn.cpp`, `cl_uiview3d.cpp`, `cl_ui.cpp`.
- `vr_traceFrame` defaults to 1 because there is no console in a headset; it
  should default to 0 once this is over.

Keep the shape of it somewhere, though. Being able to split a VR frame into
wait / scene / issue / gpu on the device is what turned this from guesswork into
one number, after several rounds of confident wrong answers.

---

## 8. Working notes

**Device.** `adb connect 192.168.1.92:43105`. It sleeps and the connection dies;
reconnect with `adb disconnect` then `adb connect`. **Launch from inside the
headset** — never `adb shell am start`.

**Archived cvars will beat a changed default.** The config is exec'd before
`VR_Init` runs, so `Cvar_Get` finds the cvar already there and keeps the old
value. This cost a wasted device test when multisampling was reported off for two
builds while still on. `VR_TuningCvar` in `vr_openxr.c` exists for this: it
registers unarchived and forces the default every start. Use it for anything
still being tuned — there is no console in the headset to correct a stale value
with.

**Settled along the way**, so it is not re-investigated:

- `XR_FB_display_refresh_rate` works; the display runs at 90Hz. It must be asked
  for from the frame loop with the two call enumeration, not at `xrBeginSession`,
  where the Oculus runtime reports no rates at all.
- MSAA via `GL_EXT_multisampled_render_to_texture` works and is off
  (`vr_samples 0`). It was never the black patches on the ground — those are
  still unexplained, and are the one open rendering defect that predates all of
  this.
- `vr_resolutionScale` is at 0.6 and should go back to 1 once the renderer is
  settled, since fill rate was not the problem.
