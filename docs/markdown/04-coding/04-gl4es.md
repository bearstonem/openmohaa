# The gl4es boundary

On Android the fixed function renderer (`renderergl1`) does not talk to the GPU.
It talks to [gl4es](https://github.com/ptitSeb/gl4es), which translates desktop
OpenGL 1.x into OpenGL ES. The engine never learns this: gl4es reports itself as
`2.1 gl4es wrapper 1.1.6`, so the renderer takes its desktop path and gl4es
turns `glBegin`, `glMatrixMode` and the rest into ES underneath.

That works. What is not obvious, and what has now cost several days of debugging
across separate sessions, is that **gl4es keeps its own copy of GL state, and
that copy can disagree with the driver.** Everything below follows from that one
fact.

Read this before debugging anything that looks like "the geometry is correct and
nothing is drawn".

---

## 1. gl4es answers state queries from its own tables

`glGetIntegerv(GL_CULL_FACE_MODE)`, `GL_FRONT_FACE`, `GL_MODELVIEW_MATRIX` and
friends are answered out of gl4es's `glstate`, not by asking the driver
(`src/gl/getter.c`). `glGetString` is the same - `GL_VERSION`, `GL_VENDOR` and
`GL_RENDERER` come from gl4es's constants without the driver being consulted.

So this proves nothing:

```c
qglGetIntegerv( GL_CULL_FACE_MODE, &mode );
/* compare mode against what the engine set */
```

The engine set that value *through gl4es*. Reading it back through gl4es returns
the engine's own value. It agrees by construction whatever the hardware is doing.

Two measurements taken this way were reported as findings and were worthless -
one of them ("GL and the engine agree on culling") was actively concealing the
bug being hunted.

**To ask the hardware, resolve the entry point out of the driver:**

```c
void *lib = dlopen( "libGLESv3.so", RTLD_NOW | RTLD_LOCAL );
void (*driverGetIntegerv)(GLenum, GLint *) = dlsym( lib, "glGetIntegerv" );
```

A temporary `r_traceSurf` probe did this - printing gl4es's answer and the
driver's side by side - and found the cull bug in one run, after many runs that
measured nothing. It has since been removed; the technique is the point, and it
is four lines to put back.

## 2. gl4es drops calls it judges redundant

Each of these returns early when the requested value matches gl4es's copy:

| Call | Where |
|---|---|
| `glCullFace` | `src/gl/face.c:14` |
| `glDepthMask`, `glDepthFunc`, `glDepthRangef` | `src/gl/depth.c` |
| `glBlendFunc` | `src/gl/blend.c` |
| `glColorMask` | `src/gl/gl4es.c` |
| `glViewport`, `glScissor` | `src/gl/raster.c:61` |
| `glEnable`/`glDisable` for `GL_CULL_FACE`, `GL_DEPTH_TEST`, `GL_STENCIL_TEST`, `GL_POLYGON_OFFSET_FILL` | `src/gl/enable.c`, `proxy_glEnable` |

`GL_SCISSOR_TEST` is *not* filtered - it falls through `proxy_glEnable`'s default
branch to the driver every time.

**Once gl4es's copy drifts from the driver, the engine can never correct it.**
It asks for the right value, gl4es compares against the wrong copy, calls it a
no-op, and the driver never hears. No error is raised. Nothing looks wrong from
inside the engine, because the engine's own state tracker and gl4es's tracker
agree with each other - they are just both wrong about the hardware.

## 3. Anything the VR layer sets natively must go through gl4es

`code/vr/vr_openxr.c` links `libGLESv3` directly, so a bare `glViewport` there
reaches the driver without gl4es seeing it. gl4es's copy is then stale, and by
§2 the engine's next request for that value is silently dropped.

This is inverted from the reference. RTCWQuest's ndk-build puts gl4es first in
its link order, so *their* VR layer's GL calls go through gl4es by default and
they bypass it explicitly only where they must. Here the default is the other
way round, so the routing has to be done by hand.

The VR layer therefore wraps every state call gl4es caches - `VR_Viewport`,
`VR_GLEnable`, `VR_GLDisable`, `VR_DepthMask`, `VR_DepthFunc`, `VR_BlendFunc`,
`VR_ColorMask`, `VR_CullFace`, `VR_Scissor`, `VR_BindFramebuffer` - and resolves
them out of `libgl4es.so` by name.

**Three things deliberately stay with the driver**, and must not be routed:

- the swapchain texture attachment (`glFramebufferTexture2D`) - gl4es cannot
  attach a texture the OpenXR runtime created;
- the blit's `GL_READ_FRAMEBUFFER` / `GL_DRAW_FRAMEBUFFER` bindings - gl4es
  treats `GL_READ_FRAMEBUFFER` as a note to itself and returns without binding
  (`framebuffers.c:238`), which silently empties the blit;
- the VR layer's own pipeline: its shader program, vertex attributes, uniforms
  and draw calls. gl4es has no business knowing about those.

## 4. Setting state that must not be dropped

Where a value has to reach the driver regardless of what gl4es believes, name a
throwaway value first so the real one cannot match a stale cache. `GL_Cull` in
`renderergl1/tr_backend.c`:

```c
#ifdef USE_GL4ES
    qglCullFace( GL_FRONT_AND_BACK );   /* cannot be mistaken for a no-op */
#endif
    qglCullFace( face );
```

Nothing is drawn between the two calls, so the throwaway value is harmless.

---

## 5. What this actually cost

Three bugs on this boundary, each of which presented as "the renderer is
correct and nothing is drawn":

**The framebuffer binding.** gl4es renders from its own table, not the driver's
binding, so a framebuffer created by `glGenFramebuffers` on the driver is not in
that table - `gl4es_glBindFramebuffer` raises `GL_INVALID_VALUE`, returns
without binding, and leaves gl4es drawing into the 16x16 pbuffer. Measured as
`fbo: gl4es says 0, driver says 10`.

**The cull face.** gl4es's copy said `GL_FRONT`; the driver was culling
`GL_BACK`. The engine asked for `GL_FRONT` every frame and gl4es dropped it as
redundant. Quake's world polygons are back facing in GL's terms - which is why
`CT_FRONT_SIDED` culls `GL_FRONT` in the first place - so the hardware was
removing precisely the faces that should have been visible.

The symptom was **the entire world invisible while models rendered perfectly**,
because model shaders are `CT_TWO_SIDED` and never enable culling at all.
Inverting the culled face did not help either: asking for `GL_BACK` matched what
the driver was already doing. Only disabling culling outright brought the world
back, which made it look like a winding fault for a very long time.

**The viewport.** Set natively by the VR layer, cached by gl4es, so the engine's
next request for the same value was dropped.

### The shape to recognise

> Geometry submitted. Vertex data correct. Indices in range. Matrices correct.
> On screen, unclipped, unculled by the frustum. No GL error. Nothing drawn.

That is what a dropped state call looks like from inside the engine, every time.
When you see it, **stop measuring the engine and ask the driver.**

---

## 6. Two rules that are not about gl4es

Learned the hard way alongside the above, and they apply to any device work
here.

**A probe must be shown to reach the thing it measures.** Three separate probes
had blind spots exactly where the fault was and each produced a confident wrong
conclusion: a panel readback that counted "brighter than 8" against a buffer
that is never cleared, so *nothing drawn* and *drawn black* were the same
number; an `r_flatColor` override that did not apply to multitextured surfaces,
so it only ever covered models; and a flat-red override that restored its state
inside a branch that was never taken, corrupting the renderer it was measuring.

**Verify the change landed.** `r_noDepth` was declared, registered, documented
and tested across several device runs - and never actually used, because the
edit that was supposed to read it silently failed to apply and nothing checked.
Every result attributed to it was a no-op, and it sat in the "known good"
configuration taking credit for what disabling culling was doing. Grep for the
symbol after editing, or read the warning log; a cvar that reads nothing looks
exactly like a cvar that changes nothing.

**Look at the frame.** `vr_captureEye` writes the eye buffer to
`main/vrshotN.tga` every two seconds. One image settled questions that six
rounds of verbal description could not. Pixel counts bound the answer; a picture
gives it.
