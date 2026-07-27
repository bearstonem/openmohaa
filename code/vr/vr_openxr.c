/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*
OpenXR stereo rendering.

The engine keeps its SDL window and the EGL context SDL created; the session
is bound to that same context rather than a private one, so there is a single
GL context in the process and the renderer needs no thread or context juggling.
What changes is where the renderer's "screen" is: each eye is drawn into an
OpenXR swapchain image instead of the window, and the compositor presents them.

The session and every swapchain are tied to the GL context they were created
with. The engine destroys and recreates that context on vid_restart, which
would leave the session referring to a dead context and make
xrAcquireSwapchainImage fail, so GLimp tears the session down and rebuilds it
around any renderer restart.
*/

#include "vr_common.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"
#include "../sys/sys_android.h"

#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
// GL_EXT_multisampled_render_to_texture is an ES extension and lives in the ES2
// extension header even for an ES3 context.
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

// From code/sdl/sdl_glimp.c, which owns the window and the GL context. Declared
// here rather than by including the renderer's headers, which would pull the
// whole of tr_local into a file that has no business seeing it.
qboolean GLimp_MakeCurrent(void);



/*
gl4es has to agree with the driver about which framebuffer is bound.

It keeps its own table of the framebuffers it created and renders from that,
not from the driver's binding. A framebuffer the driver made is not in the
table, so gl4es leaves its idea of the target at zero and draws there - and
zero, in this process, is the 16x16 pbuffer the GL context is current against.
The engine then submits correct geometry through correct matrices into a
viewport that lies entirely outside a sixteen pixel square, and nothing is
drawn anywhere, with no error raised.

So gl4es generates, binds and deletes these. Two things deliberately stay with
the driver:

  - the attachment of the swapchain texture, because gl4es cannot attach a
    texture the OpenXR runtime created (RTCWQuest hits this too and says so at
    TBXR_Common.c:290);
  - the read and draw bindings used by the blit, because gl4es takes
    GL_READ_FRAMEBUFFER as a note to itself and returns without binding
    anything - which silently empties the blit and takes the cutscenes with it.

Only GL_FRAMEBUFFER goes through gl4es. The names are the driver's either way,
since gl4es registers what the driver hands back.

The same argument covers the viewport, for a different reason. gl4es caches it
and skips a call that matches what it last sent (raster.c, gl4es_glViewport):

	if (glstate->raster.viewport.x!=x || ... ) { gles_glViewport(...); ... }

So a glViewport issued straight at the driver here does not just go unnoticed -
it poisons the next one the engine asks for. Set the driver to A behind gl4es's
back while gl4es still believes A is current, and the engine's own request for A
is dropped as redundant; set it to B, and the engine draws into A believing it
asked for B. Routing ours through gl4es keeps the cache honest, and gl4es
forwards to the same driver call regardless, so nothing else changes.

glScissor is cached the same way but the VR layer never sets one. GL_SCISSOR_TEST
is safe: gl4es filters GL_CULL_FACE, GL_DEPTH_TEST and GL_BLEND against its own
state but lets GL_SCISSOR_TEST fall through to the driver every time
(enable.c, proxy_glEnable's default branch).
*/
#ifdef USE_GL4ES
static void (*gl4esGenFramebuffers)(GLsizei n, GLuint *framebuffers);
static void (*gl4esBindFramebuffer)(GLenum target, GLuint framebuffer);
static void (*gl4esDeleteFramebuffers)(GLsizei n, const GLuint *framebuffers);
static void (*gl4esViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
static void (*gl4esEnable)(GLenum cap);
static void (*gl4esDisable)(GLenum cap);
static void (*gl4esDepthMask)(GLboolean flag);
static void (*gl4esDepthFunc)(GLenum func);
static void (*gl4esBlendFunc)(GLenum sfactor, GLenum dfactor);
static void (*gl4esColorMask)(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
static void (*gl4esCullFace)(GLenum mode);
static void (*gl4esScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
static void (*gl4esStencilMask)(GLuint mask);

static qboolean VR_GL4ESFramebuffers(void)
{
	static qboolean tried;

	if (!tried) {
		void *gl4es = dlopen("libgl4es.so", RTLD_NOW | RTLD_LOCAL);

		tried = qtrue;

		if (gl4es) {
			gl4esGenFramebuffers    = dlsym(gl4es, "glGenFramebuffers");
			gl4esBindFramebuffer    = dlsym(gl4es, "glBindFramebuffer");
			gl4esDeleteFramebuffers = dlsym(gl4es, "glDeleteFramebuffers");
			gl4esViewport           = dlsym(gl4es, "glViewport");
			gl4esEnable             = dlsym(gl4es, "glEnable");
			gl4esDisable            = dlsym(gl4es, "glDisable");
			gl4esDepthMask          = dlsym(gl4es, "glDepthMask");
			gl4esDepthFunc          = dlsym(gl4es, "glDepthFunc");
			gl4esBlendFunc          = dlsym(gl4es, "glBlendFunc");
			gl4esColorMask          = dlsym(gl4es, "glColorMask");
			gl4esCullFace           = dlsym(gl4es, "glCullFace");
			gl4esScissor            = dlsym(gl4es, "glScissor");
			gl4esStencilMask        = dlsym(gl4es, "glStencilMask");
		}

		if (!gl4esGenFramebuffers || !gl4esBindFramebuffer || !gl4esDeleteFramebuffers) {
			Com_Printf("VR: no gl4es framebuffer entry points; the renderer and the "
				"driver will disagree about the target and nothing will be drawn\n");
			gl4esBindFramebuffer = NULL;
		}

		if (!gl4esViewport) {
			Com_Printf("VR: no gl4es glViewport; gl4es will keep a stale viewport "
				"and drop the renderer's next request for it as redundant\n");
		}
	}

	return gl4esBindFramebuffer != NULL;
}
#endif

/*
Every GL state setter below goes through gl4es for the reason given above, and
the list is not arbitrary - it is exactly what gl4es keeps its own copy of and
skips when the new value matches:

	glEnable/glDisable   GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST,
	                     GL_POLYGON_OFFSET_FILL, GL_PROGRAM_POINT_SIZE
	                     (enable.c, proxy_glEnable)
	glDepthMask          depth.c, "if (glstate->depth.mask == flag) return"
	glDepthFunc          depth.c
	glBlendFunc          blend.c, "already set"
	glColorMask          gl4es.c
	glCullFace, glScissor, glViewport

GL_SCISSOR_TEST is not among the cached caps - it falls through proxy_glEnable's
default branch to the driver every time - but glScissor itself is cached, so the
pair is routed together rather than split on a distinction nobody will remember.

This is the third time this exact bug has been found here, and the first two
were each fixed as one-offs: the framebuffer binding (5.7) and the viewport.
The general statement is that this process has two independent caches over one
driver - the renderer's glState and gl4es's glstate - and a third party writing
underneath both. Anything the VR layer sets natively that gl4es also tracks will
be silently dropped the next time the engine asks for it.

What deliberately stays native is the VR layer's own pipeline: its shader
program, vertex attributes, uniforms, texture uploads and draw calls. gl4es does
not need to know about those, and routing them through it would mean handing its
fixed function emulation a program it did not build.
*/
static void VR_Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esViewport) {
		gl4esViewport(x, y, width, height);
		return;
	}
#endif
	glViewport(x, y, width, height);
}

static void VR_GLEnable(GLenum cap)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esEnable) {
		gl4esEnable(cap);
		return;
	}
#endif
	glEnable(cap);
}

static void VR_GLDisable(GLenum cap)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esDisable) {
		gl4esDisable(cap);
		return;
	}
#endif
	glDisable(cap);
}

static void VR_DepthMask(GLboolean flag)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esDepthMask) {
		gl4esDepthMask(flag);
		return;
	}
#endif
	glDepthMask(flag);
}

static void VR_DepthFunc(GLenum func)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esDepthFunc) {
		gl4esDepthFunc(func);
		return;
	}
#endif
	glDepthFunc(func);
}

static void VR_BlendFunc(GLenum sfactor, GLenum dfactor)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esBlendFunc) {
		gl4esBlendFunc(sfactor, dfactor);
		return;
	}
#endif
	glBlendFunc(sfactor, dfactor);
}

static void VR_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esColorMask) {
		gl4esColorMask(r, g, b, a);
		return;
	}
#endif
	glColorMask(r, g, b, a);
}

static void VR_CullFace(GLenum mode)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esCullFace) {
		gl4esCullFace(mode);
		return;
	}
#endif
	glCullFace(mode);
}

static void VR_Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esScissor) {
		gl4esScissor(x, y, width, height);
		return;
	}
#endif
	glScissor(x, y, width, height);
}

static void VR_StencilMask(GLuint mask)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers() && gl4esStencilMask) {
		gl4esStencilMask(mask);
		return;
	}
#endif
	glStencilMask(mask);
}


static void VR_GenFramebuffers(GLsizei n, GLuint *framebuffers)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers()) {
		gl4esGenFramebuffers(n, framebuffers);
		return;
	}
#endif
	glGenFramebuffers(n, framebuffers);
}

static void VR_DeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers()) {
		gl4esDeleteFramebuffers(n, framebuffers);
		return;
	}
#endif
	glDeleteFramebuffers(n, framebuffers);
}

// GL_FRAMEBUFFER only; see above. The blit's read and draw bindings must not
// come through here.
static void VR_BindFramebuffer(GLuint framebuffer)
{
#ifdef USE_GL4ES
	if (VR_GL4ESFramebuffers()) {
		gl4esBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		return;
	}
#endif
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

typedef struct {
	XrSwapchain     handle;
	uint32_t        width;
	uint32_t        height;
	uint32_t        imageCount;
	XrSwapchainImageOpenGLESKHR *images;
	GLuint         *depthBuffers;
	GLuint         *frameBuffers;
	uint32_t        acquiredIndex;
	qboolean        acquired;
} vrSwapchain_t;

static struct {
	qboolean        enabled;
	qboolean        sessionRunning;
	qboolean        frameStarted;

	XrInstance      instance;
	XrSystemId      systemId;
	XrSession       session;
	XrSpace         stageSpace;
	XrSpace         viewSpace;
	XrSessionState  sessionState;

	// Something for the engine's GL context to be current against when the
	// window has no surface, which in a headset is the ordinary case. See
	// VR_CreateSession.
	EGLSurface      eglTinySurface;

	uint32_t        eyeWidth;
	uint32_t        eyeHeight;

	vrSwapchain_t   swapchains[VR_MAX_EYES];
	XrView          views[VR_MAX_EYES];
	XrCompositionLayerProjectionView projectionViews[VR_MAX_EYES];

	// Flat content - menus, cinematics - goes on a quad in front of the
	// viewer instead of into the eye buffers. The quad is placed once, in
	// world space, so the head can then move freely around it.
	vrSwapchain_t   uiSwapchain;
	// The panel is drawn into this and then copied into whichever swapchain
	// image the runtime hands out. A swapchain rotates through several images,
	// so anything the 2D path expects to still be there from the previous frame
	// would be missing two frames out of three; one buffer it keeps returning
	// to behaves like the single screen the engine thinks it has.
	GLuint          uiTexture;
	GLuint          uiFramebuffer;
	qboolean        screenLayerActive;
	qboolean        layerReady;
	XrPosef         screenAnchor;
	qboolean        screenAnchorValid;

	// Where the head was when tracking started. Poses are reported relative to
	// this, so the game keeps supplying the eye height and the headset only
	// contributes what the player does on top of it - leaning, ducking,
	// stepping. Using the absolute stage position instead would add the
	// player's real standing height to the character's.
	XrVector3f      trackingOrigin;
	// Which way the player was facing when tracking started. Subtracted from
	// every later reading so that facing forwards in the room means facing
	// forwards in the game. Without it the play space's own arbitrary heading -
	// whichever way the guardian happened to be set up - is added to the
	// character's, and the player spawns looking somewhere else entirely.
	float           yawOffset;
	qboolean        trackingOriginValid;
	qboolean        viewsValid;

	XrFrameState    frameState;
	vrEyeView_t     eyeViews[VR_MAX_EYES];

	// Controllers. Only what the menus need for now: where each hand points,
	// and whether the trigger is down.
	XrActionSet     actionSet;
	XrAction        aimAction;
	XrAction        selectAction;
	XrAction        moveAction;
	XrAction        turnAction;
	XrAction        objectivesAction;
	qboolean        objectivesWasDown;
	XrAction        jumpAction;
	qboolean        jumpWasDown;
	XrAction        duckAction;
	qboolean        duckWasDown;
	XrAction        useAction;
	qboolean        useWasDown;
	XrPath          handPaths[2];
	XrSpace         aimSpaces[2];
	qboolean        actionsReady;
	qboolean        selectWasDown;
	int             pointerHand;

	// XR_FB_display_refresh_rate, when the runtime offers it. Quest hands out
	// 72Hz unless asked otherwise, and asking is most of a comfort upgrade for
	// almost none of the work.
	PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateRefreshRates;
	PFN_xrRequestDisplayRefreshRateFB    pfnRequestRefreshRate;
	qboolean        refreshRateApplied;

	// GL_EXT_multisampled_render_to_texture. On a tiler the resolve happens in
	// tile memory on the way out, so multisampling costs a fraction of what the
	// same thing costs on a desktop part - and aliasing is far more obvious
	// through a headset than on a monitor.
	int             samples;
	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC  glRenderbufferStorageMultisampleEXT;
	PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT;

	// A beam drawn from each hand, so the menu pointer has something to see.
	// Its own tiny GL program: on a screen layer frame there is no 3D scene for
	// the renderer to put it in, and this is a handful of triangles.
	GLuint          beamProgram;
	GLint           beamMvpLocation;
	GLint           beamColorLocation;
	GLuint          beamVertexArray;
	GLuint          beamVertexBuffer;
	qboolean        beamReady;

	// Turns what the HUD drew into what the compositor can blend. See
	// VR_ResolveWristPanel.
	GLuint          panelProgram;
	GLint           panelTextureLocation;
	GLint           panelCropLocation;
	GLuint          panelVertexArray;
	qboolean        panelReady;
	XrPosef         handPoses[2];
	qboolean        handPoseValid[2];
	float           pointerDistance;
	qboolean        pointerLayerReady;

	// See VR_SetBaseYaw.
	float           baseYaw;

	// Where the head was last frame, for turning walking into movement.
	vec3_t          lastHeadOrigin;
	qboolean        stepValid;

	// Frame timing, split by phase. See VR_TraceFrameTiming.
	int             eyeStart;
	int             phaseWait;
	int             phaseEyes;
	int             phaseSubmit;
	int             phaseFrontEnd;
	int             phaseBackEnd;
	int             phaseScene;
	int             phaseIssue;
	int             phaseWorld;
	int             phaseHud;
	int             phaseCgameHud;
	int             phaseGpu;
	int             traceEvents[VRTRACE_COUNT];
	int             traceDrawMode;
	int             traceHudPass;
	int             traceNoMenus;
	int             hudSetup;
	int             hudFades;
	int             hudPrints;
	int             hudOverlays;
	int             hudTail;

	// The wrist panel. Shares the screen layer's buffers: the two are never up
	// at once, since one is what the player sees instead of the world and the
	// other is what they see over it.
	qboolean        wristVisible;
	XrPosef         wristPose;
	qboolean        wristLayerReady;

	// Built once a frame and drawn into whichever targets want it - both eyes
	// during play, both eye images again on a menu frame.
	float           beamVerts[2 * 2 * 36 * 3];
	int             beamVertexCount;

	cvar_t         *vr_wristPanel;
	cvar_t         *vr_wristSize;
	cvar_t         *vr_wristDistance;
	cvar_t         *vr_wristBack;
	cvar_t         *vr_wristCropX;
	cvar_t         *vr_wristCropY;
	cvar_t         *vr_wristCropW;
	cvar_t         *vr_wristCropH;

	cvar_t         *vr_worldscale;
	cvar_t         *vr_screenDistance;
	cvar_t         *vr_screenSize;
	cvar_t         *vr_refreshRate;
	cvar_t         *vr_msaa;
	cvar_t         *vr_pointerBeam;
} vr;

static cvar_t *vr_traceTracking;
static cvar_t *vr_traceFrame;
static cvar_t *vr_debugPanel;
static cvar_t *vr_captureEye;

/*
==================
VR_CheckResult
==================
*/
static qboolean VR_CheckResult(XrResult result, const char *what)
{
	char message[XR_MAX_RESULT_STRING_SIZE + 128];
	char resultName[XR_MAX_RESULT_STRING_SIZE];

	if (XR_SUCCEEDED(result)) {
		return qtrue;
	}

	if (vr.instance != XR_NULL_HANDLE
		&& XR_SUCCEEDED(xrResultToString(vr.instance, result, resultName))) {
		Com_sprintf(message, sizeof(message), "OpenXR: %s failed: %s", what, resultName);
	} else {
		Com_sprintf(message, sizeof(message), "OpenXR: %s failed: %d", what, (int)result);
	}

	Com_Printf("%s\n", message);
	return qfalse;
}

#define XR_CHECK(call) VR_CheckResult((call), #call)

static void VR_CreateActions(void);
static void VR_DestroySwapchain(vrSwapchain_t *swapchain);
static void VR_CreateBeam(void);
static void VR_DestroyBeam(void);
static void VR_RenderPointerLayer(void);
static void VR_ResolveWristPanel(GLuint target);
static int  VR_BuildBeamGeometry(float *verts, int maxVerts);
static void VR_DrawBeam(int eye, const float *verts, int vertexCount);

/*
==================
VR_Enabled
==================
*/
qboolean VR_Enabled(void)
{
	return vr.enabled;
}

/*
==================
VR_GetRenderResolution
==================
*/
void VR_GetRenderResolution(int *width, int *height)
{
	if (width) {
		*width = (int)vr.eyeWidth;
	}
	if (height) {
		*height = (int)vr.eyeHeight;
	}
}

/*
==================
VR_InitLoader

Android requires the loader be handed the JVM and the activity before any
other OpenXR call is made.
==================
*/
static qboolean VR_InitLoader(void)
{
	PFN_xrInitializeLoaderKHR   xrInitializeLoaderKHR = NULL;
	XrLoaderInitInfoAndroidKHR  loaderInfo;

	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		(PFN_xrVoidFunction *)&xrInitializeLoaderKHR);

	if (!xrInitializeLoaderKHR) {
		Com_Printf("OpenXR: no xrInitializeLoaderKHR; the loader is too old for Android\n");
		return qfalse;
	}

	memset(&loaderInfo, 0, sizeof(loaderInfo));
	loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;

	// SDL owns the JavaVM and the activity; borrow them rather than keeping a
	// second reference to either.
	{
		JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
		JavaVM *vm = NULL;

		if (!env) {
			Com_Printf("OpenXR: no JNI environment\n");
			return qfalse;
		}

		(*env)->GetJavaVM(env, &vm);

		loaderInfo.applicationVM = vm;
		loaderInfo.applicationContext = SDL_AndroidGetActivity();

		if (!vm || !loaderInfo.applicationContext) {
			Com_Printf("OpenXR: no JavaVM or activity\n");
			return qfalse;
		}
	}

	return XR_CHECK(xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo));
}

/*
==================
VR_TuningCvar

A cvar whose default is still being argued with.

Registered unarchived and forced to the default every start, because a value
written out by an earlier build otherwise sits in the config and quietly wins:
the config is exec'd before any of this runs, so Cvar_Get finds the cvar already
there and keeps whatever it says, and a changed default does nothing at all.
That cost a round trip on the device with vr_msaa, where multisampling was
reported off for two builds while it was still on.

There is no console in the headset to correct one with either, so a stale value
is not something the player can work around. These become ordinary archived
cvars once the numbers settle.
==================
*/
static cvar_t *VR_TuningCvar(const char *name, const char *value)
{
	cvar_t *cvar = Cvar_Get(name, value, 0);

	Cvar_Set2(name, value, qtrue);
	return cvar;
}

/*
==================
VR_ExtensionSupported

Whether the runtime offers an instance extension. Everything beyond the two
required ones is asked for rather than assumed: a runtime that does not have it
must not be handed the name, or xrCreateInstance fails outright and the game
loses VR entirely over an optional feature.
==================
*/
static qboolean VR_ExtensionSupported(const char *name)
{
	XrExtensionProperties *available;
	uint32_t               count = 0;
	uint32_t               i;
	qboolean               found = qfalse;

	if (!XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL)) || !count) {
		Com_Printf("OpenXR: could not enumerate instance extensions\n");
		return qfalse;
	}

	available = Z_Malloc(count * sizeof(*available));
	for (i = 0; i < count; i++) {
		memset(&available[i], 0, sizeof(available[i]));
		available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}

	if (XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, count, &count, available))) {
		for (i = 0; i < count && !found; i++) {
			if (!strcmp(available[i].extensionName, name)) {
				found = qtrue;
			}
		}
	}

	Com_Printf("OpenXR: %s %s among %u instance extensions\n",
		name, found ? "found" : "NOT found", count);

	// The list itself when the answer is no, because the alternative is
	// guessing at whether the name is wrong, the runtime is old, or the
	// enumeration returned nothing useful at all.
	if (!found) {
		for (i = 0; i < count; i++) {
			Com_Printf("OpenXR:   %s\n", available[i].extensionName);
		}
	}

	Z_Free(available);
	return found;
}

/*
==================
VR_ApplyRefreshRate

Quest 3 runs at 72Hz until something asks for more. Picks the fastest mode the
headset offers that is no faster than vr_refreshRate, so lowering the cvar is a
way to buy frame budget rather than a way to be ignored.
==================
*/
static void VR_ApplyRefreshRate(void)
{
	float    rates[32];
	uint32_t count = 0;
	uint32_t i;
	float    want, best = 0.0f;

	// Every way out of here says so. Silence was indistinguishable from the
	// call never having happened, which cost a round trip on the device to find
	// out which.
	if (!vr.pfnEnumerateRefreshRates || !vr.pfnRequestRefreshRate) {
		Com_Printf("OpenXR: refresh rate entry points missing\n");
		return;
	}

	want = vr.vr_refreshRate ? vr.vr_refreshRate->value : 0.0f;

	// 0 leaves whatever the runtime chose, for telling "the request failed"
	// apart from "the request was never made" when a frame rate looks wrong.
	if (want <= 0.0f) {
		Com_Printf("OpenXR: vr_refreshRate %g, leaving the runtime's choice alone\n", want);
		return;
	}

	// Two calls, the way the rest of OpenXR is enumerated: ask how many, then
	// ask for them. Handing over a buffer and hoping got a count of zero out of
	// the Oculus runtime.
	if (!XR_CHECK(vr.pfnEnumerateRefreshRates(vr.session, 0, &count, NULL))) {
		return;
	}

	if (!count) {
		// Not an error and not final: the runtime does not necessarily have
		// these ready the moment the session begins, so this is retried until it
		// does. Silent, because otherwise it would say so every frame.
		return;
	}

	if (count > ARRAY_LEN(rates)) {
		count = ARRAY_LEN(rates);
	}

	if (!XR_CHECK(vr.pfnEnumerateRefreshRates(vr.session, count, &count, rates)) || !count) {
		return;
	}

	for (i = 0; i < count; i++) {
		Com_Printf("OpenXR: display refresh rate available: %.0fHz\n", rates[i]);

		if (rates[i] <= want + 0.5f && rates[i] > best) {
			best = rates[i];
		}
	}

	if (best <= 0.0f) {
		Com_Printf("OpenXR: no display refresh rate at or below %.0fHz\n", want);
		return;
	}

	if (XR_CHECK(vr.pfnRequestRefreshRate(vr.session, best))) {
		Com_Printf("OpenXR: display refresh rate %.0fHz\n", best);
	}

	// Either way, stop asking.
	vr.refreshRateApplied = qtrue;
}

/*
==================
VR_Init
==================
*/
qboolean VR_Init(void)
{
	const char *extensions[4];
	uint32_t    extensionCount = 0;
	qboolean    wantRefreshRate;

	XrInstanceCreateInfoAndroidKHR androidInfo;
	XrInstanceCreateInfo           instanceInfo;
	XrSystemGetInfo                systemInfo;
	XrInstanceProperties           instanceProperties;
	XrViewConfigurationView        viewConfigs[VR_MAX_EYES];
	uint32_t                       viewCount = 0;
	int                            i;

	PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetRequirements = NULL;
	XrGraphicsRequirementsOpenGLESKHR        requirements;

	memset(&vr, 0, sizeof(vr));

	vr.vr_worldscale = Cvar_Get("vr_worldscale", "32", CVAR_ARCHIVE);
	vr.vr_screenDistance = Cvar_Get("vr_screenDistance", "2.5", CVAR_ARCHIVE);
	vr.vr_screenSize = Cvar_Get("vr_screenSize", "3.0", CVAR_ARCHIVE);
	vr.vr_refreshRate = Cvar_Get("vr_refreshRate", "90", CVAR_ARCHIVE);
	// Off until there is a measured frame budget to spend on it. The mechanism
	// works - the driver has the extension and the attachments come up complete
	// - but multisampled render to texture keeps the samples in tile memory,
	// and anything that unbinds and rebinds the framebuffer mid frame loses
	// them, so it wants checking against the renderer's passes before it goes
	// on by default.
	//
	// Off until the black patches on the ground are understood - it is the
	// obvious suspect, since multisampled render to texture keeps its samples in
	// tile memory and anything that unbinds the framebuffer mid frame loses
	// them.
	vr.vr_msaa = VR_TuningCvar("vr_samples", "0");
	vr.vr_pointerBeam = Cvar_Get("vr_pointerBeam", "1", CVAR_ARCHIVE);
	vr.vr_wristPanel = VR_TuningCvar("vr_wristPanel", "0");
	// Big for something worn on the arm, because the panel carries the whole
	// 1680x1760 screen and the HUD only occupies a small part of it - a compass
	// a hundred and fifty pixels across is a couple of degrees on a panel this
	// close, which is legible only in the sense that you can tell it is there.
	// Scaling the panel scales everything on it and crops nothing.
	vr.vr_wristSize = VR_TuningCvar("vr_wristSize", "0.26");
	// Which part of the screen the panel shows, as fractions. The HUD is laid
	// out for a whole screen and occupies a small share of it, so at full extent
	// a compass is a couple of degrees across and legible only in the sense that
	// you can tell it is there. Growing the quad does not help - that is a
	// bigger panel with the same small compass on it. Showing less of the buffer
	// is what makes the contents bigger, and the compositor does the crop for
	// free.
	//
	// The top band, full width: the health readout sits at one end of it and the
	// compass at the other, with the rest of the screen empty. Keeping the whole
	// width keeps both, and throwing away the empty two thirds below is what
	// makes them large enough to read - the panel shows less, so what is left is
	// bigger, without the quad growing in the room.
	//
	vr.vr_wristCropX = VR_TuningCvar("vr_wristCropX", "0");
	vr.vr_wristCropY = VR_TuningCvar("vr_wristCropY", "0");
	vr.vr_wristCropW = VR_TuningCvar("vr_wristCropW", "1");
	vr.vr_wristCropH = VR_TuningCvar("vr_wristCropH", "0.34");
	// Out of the back of the hand, and back along the forearm: the controller's
	// pose sits out at the fingers, so a panel placed at it would be worn on the
	// knuckles rather than the wrist. Stood further off now that it is large
	// enough to otherwise swallow the player's arm.
	vr.vr_wristDistance = VR_TuningCvar("vr_wristDistance", "0.16");
	vr.vr_wristBack = VR_TuningCvar("vr_wristBack", "0.07");
	// On while the menu is still black. Clears the flat panel to magenta before
	// the engine draws into it and sorts a grid of samples afterwards, so that
	// "the UI never got here" and "the UI got here and drew black" stop being
	// the same measurement - which is what the readback that reported 0/256 was
	// actually doing. Turn it off once the picture is up: it costs a clear and
	// 256 glReadPixels a second, and a magenta panel is not a shipping menu.
	vr_debugPanel = VR_TuningCvar("vr_debugPanel", "1");
	// Dumps the left eye to main/vrshotN.tga every two seconds, so what the
	// renderer produced can be looked at directly instead of described.
	vr_captureEye = VR_TuningCvar("vr_captureEye", "1");
	vr_traceTracking = Cvar_Get("vr_traceTracking", "0", 0);
	// On by default while the frame budget is still an open question; there is
	// no console in the headset to turn it on with when it is wanted.
	vr_traceFrame = Cvar_Get("vr_traceFrame", "1", 0);
	// Read by the game, which otherwise clamps where the player may look while
	// they are climbing - see Player::PlayerAngles.
	Cvar_Set2("vr_freeLook", "1", qtrue);

	//
	// Strip the renderer back to what a 2002 game actually needs.
	//
	// renderergl2 is the rend2 rewrite: every surface goes through a GLSL
	// program with dozens of uniforms set on it, once per surface per eye. That
	// is the cost the frame timing found - tens of milliseconds of CPU issuing
	// draws, while the GPU sat idle finishing in three. None of what it buys is
	// visible here: this content was authored for a fixed function renderer, so
	// the normal maps, specular, tone mapping and the rest are being computed
	// over textures that have nothing for them to read.
	//
	// Forced rather than defaulted, because these are archived and a value from
	// an earlier run would otherwise win - and there is no console in a headset
	// to correct one with.
	//
	{
		static const char *strip[] = {
			"r_normalMapping",  "0",
			"r_specularMapping","0",
			"r_deluxeMapping",  "0",
			"r_cubeMapping",    "0",
			"r_hdr",            "0",
			"r_toneMap",        "0",
			"r_ssao",           "0",
			"r_pbr",            "0",
			"r_sunlightMode",   "0",
			"r_depthPrepass",   "0",
			"r_shadows",        "0",
			"r_dlightMode",     "0",
		};
		int i;

		for (i = 0; i < (int)ARRAY_LEN(strip); i += 2) {
			Cvar_Set2(strip[i], strip[i + 1], qtrue);
		}
	}

	if (!VR_InitLoader()) {
		return qfalse;
	}

	extensions[extensionCount++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	extensions[extensionCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;

	wantRefreshRate = VR_ExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (wantRefreshRate) {
		extensions[extensionCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	}

	memset(&androidInfo, 0, sizeof(androidInfo));
	androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	{
		JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
		JavaVM *vm = NULL;

		(*env)->GetJavaVM(env, &vm);
		androidInfo.applicationVM = vm;
		androidInfo.applicationActivity = SDL_AndroidGetActivity();
	}

	memset(&instanceInfo, 0, sizeof(instanceInfo));
	instanceInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.next = (const void *)&androidInfo;
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	Q_strncpyz(instanceInfo.applicationInfo.applicationName, "OpenMoHAA",
		sizeof(instanceInfo.applicationInfo.applicationName));
	Q_strncpyz(instanceInfo.applicationInfo.engineName, "OpenMoHAA",
		sizeof(instanceInfo.applicationInfo.engineName));
	instanceInfo.enabledExtensionCount = extensionCount;
	instanceInfo.enabledExtensionNames = extensions;

	// A missing runtime is not an error: the game should still run flat.
	if (!XR_CHECK(xrCreateInstance(&instanceInfo, &vr.instance))) {
		Com_Printf("OpenXR: no runtime available, running without VR\n");
		vr.instance = XR_NULL_HANDLE;
		return qfalse;
	}

	if (wantRefreshRate) {
		XR_CHECK(xrGetInstanceProcAddr(vr.instance, "xrEnumerateDisplayRefreshRatesFB",
			(PFN_xrVoidFunction *)&vr.pfnEnumerateRefreshRates));
		XR_CHECK(xrGetInstanceProcAddr(vr.instance, "xrRequestDisplayRefreshRateFB",
			(PFN_xrVoidFunction *)&vr.pfnRequestRefreshRate));
	}

	memset(&instanceProperties, 0, sizeof(instanceProperties));
	instanceProperties.type = XR_TYPE_INSTANCE_PROPERTIES;
	if (XR_CHECK(xrGetInstanceProperties(vr.instance, &instanceProperties))) {
		Com_Printf("OpenXR runtime: %s %d.%d.%d\n",
			instanceProperties.runtimeName,
			XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
			XR_VERSION_MINOR(instanceProperties.runtimeVersion),
			XR_VERSION_PATCH(instanceProperties.runtimeVersion));
	}

	memset(&systemInfo, 0, sizeof(systemInfo));
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	if (!XR_CHECK(xrGetSystem(vr.instance, &systemInfo, &vr.systemId))) {
		Com_Printf("OpenXR: no headset attached, running without VR\n");
		xrDestroyInstance(vr.instance);
		vr.instance = XR_NULL_HANDLE;
		return qfalse;
	}

	// Must be called before the session is created, even though the result is
	// only advisory for us - the runtime treats skipping it as an error.
	if (XR_CHECK(xrGetInstanceProcAddr(vr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
			(PFN_xrVoidFunction *)&pfnGetRequirements)) && pfnGetRequirements) {
		memset(&requirements, 0, sizeof(requirements));
		requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
		XR_CHECK(pfnGetRequirements(vr.instance, vr.systemId, &requirements));
	}

	for (i = 0; i < VR_MAX_EYES; i++) {
		memset(&viewConfigs[i], 0, sizeof(viewConfigs[i]));
		viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}

	if (!XR_CHECK(xrEnumerateViewConfigurationViews(vr.instance, vr.systemId,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, VR_MAX_EYES, &viewCount, viewConfigs))
		|| viewCount != VR_MAX_EYES) {
		Com_Printf("OpenXR: expected a stereo view configuration, running without VR\n");
		xrDestroyInstance(vr.instance);
		vr.instance = XR_NULL_HANDLE;
		return qfalse;
	}

	vr.eyeWidth  = viewConfigs[0].recommendedImageRectWidth;
	vr.eyeHeight = viewConfigs[0].recommendedImageRectHeight;

	// Rendering below the runtime's recommendation, which is the one lever that
	// moves fill rate directly. It is also the measurement that settles whether
	// fill rate is the problem at all: if the frame time falls roughly with the
	// pixel count, the GPU is the bottleneck and this is the fix; if it barely
	// moves, the cost is per draw and lies somewhere else entirely.
	{
		cvar_t     *scaleCvar = VR_TuningCvar("vr_resolutionScale", "0.6");
		const float scale = scaleCvar->value;

		if (scale > 0.1f && scale < 1.0f) {
			vr.eyeWidth = (uint32_t)(vr.eyeWidth * scale);
			vr.eyeHeight = (uint32_t)(vr.eyeHeight * scale);

			// Even dimensions: an odd render target is a needless awkwardness
			// for the compositor's own scaling.
			vr.eyeWidth &= ~1u;
			vr.eyeHeight &= ~1u;
		}
	}

	Com_Printf("OpenXR: %ux%u per eye\n", vr.eyeWidth, vr.eyeHeight);

	// The engine has to render at exactly the eye texture size. Its viewport,
	// its 2D scaling and its projection all come from the video mode, so if
	// that is smaller than the swapchain image the whole frame lands in one
	// corner of the eye and the rest is never written.
	// Forced, because these are latched: an ordinary set would be held back
	// until the next renderer restart, so the first run would come up at the
	// default size and draw the whole frame into one corner of the eye texture.
	Cvar_Set2("r_customwidth", va("%u", vr.eyeWidth), qtrue);
	Cvar_Set2("r_customheight", va("%u", vr.eyeHeight), qtrue);
	Cvar_Set2("r_mode", "-1", qtrue);

	for (i = 0; i < VR_MAX_EYES; i++) {
		memset(&vr.views[i], 0, sizeof(vr.views[i]));
		vr.views[i].type = XR_TYPE_VIEW;
	}

	vr.enabled = qtrue;
	return qtrue;
}

/*
==================
VR_DestroySwapchain
==================
*/
static void VR_DestroySwapchain(vrSwapchain_t *swapchain)
{
	if (swapchain->frameBuffers) {
		VR_DeleteFramebuffers(swapchain->imageCount, swapchain->frameBuffers);
		Z_Free(swapchain->frameBuffers);
		swapchain->frameBuffers = NULL;
	}

	if (swapchain->depthBuffers) {
		glDeleteRenderbuffers(swapchain->imageCount, swapchain->depthBuffers);
		Z_Free(swapchain->depthBuffers);
		swapchain->depthBuffers = NULL;
	}

	if (swapchain->images) {
		Z_Free(swapchain->images);
		swapchain->images = NULL;
	}

	if (swapchain->handle != XR_NULL_HANDLE) {
		xrDestroySwapchain(swapchain->handle);
		swapchain->handle = XR_NULL_HANDLE;
	}

	swapchain->acquired = qfalse;
	swapchain->imageCount = 0;
}

/*
==================
VR_InitMultisampling

GL_EXT_multisampled_render_to_texture keeps the multisampled image in tile
memory and resolves it as the tile is written out, so the extra samples never
reach main memory and the bandwidth cost - the part that actually hurts on this
hardware - is close to nothing. The swapchain image itself stays single
sampled; it is the attachment that carries the sample count.
==================
*/
static void VR_InitMultisampling(void)
{
	const char *extensionList = (const char *)glGetString(GL_EXTENSIONS);
	int         want = vr.vr_msaa ? vr.vr_msaa->integer : 0;
	GLint       maxSamples = 0;

	vr.samples = 1;
	vr.glRenderbufferStorageMultisampleEXT = NULL;
	vr.glFramebufferTexture2DMultisampleEXT = NULL;

	if (want <= 1) {
		return;
	}

	if (!extensionList || !strstr(extensionList, "GL_EXT_multisampled_render_to_texture")) {
		Com_Printf("OpenXR: no GL_EXT_multisampled_render_to_texture, vr_samples ignored\n");
		return;
	}

	vr.glRenderbufferStorageMultisampleEXT = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
		eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	vr.glFramebufferTexture2DMultisampleEXT = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)
		eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");

	if (!vr.glRenderbufferStorageMultisampleEXT || !vr.glFramebufferTexture2DMultisampleEXT) {
		Com_Printf("OpenXR: multisampled render to texture advertised but not resolvable\n");
		vr.glRenderbufferStorageMultisampleEXT = NULL;
		vr.glFramebufferTexture2DMultisampleEXT = NULL;
		return;
	}

	glGetIntegerv(GL_MAX_SAMPLES_EXT, &maxSamples);
	if (maxSamples < 2) {
		return;
	}

	vr.samples = want > maxSamples ? maxSamples : want;
	Com_Printf("OpenXR: %dx MSAA, resolved in tile memory\n", vr.samples);
}

/*
==================
VR_CreateSwapchain

One swapchain per eye, each image wrapped in a framebuffer with its own depth
buffer so the engine can render straight into it.
==================
*/
static qboolean VR_CreateSwapchain(vrSwapchain_t *swapchain, uint32_t width, uint32_t height,
	int samples)
{
	XrSwapchainCreateInfo createInfo;
	uint32_t i;

	memset(swapchain, 0, sizeof(*swapchain));

	memset(&createInfo, 0, sizeof(createInfo));
	createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	createInfo.format = GL_SRGB8_ALPHA8;
	createInfo.sampleCount = 1;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;

	if (!XR_CHECK(xrCreateSwapchain(vr.session, &createInfo, &swapchain->handle))) {
		return qfalse;
	}

	swapchain->width = width;
	swapchain->height = height;

	if (!XR_CHECK(xrEnumerateSwapchainImages(swapchain->handle, 0, &swapchain->imageCount, NULL))) {
		return qfalse;
	}

	swapchain->images = Z_Malloc(swapchain->imageCount * sizeof(*swapchain->images));
	for (i = 0; i < swapchain->imageCount; i++) {
		memset(&swapchain->images[i], 0, sizeof(swapchain->images[i]));
		swapchain->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
	}

	if (!XR_CHECK(xrEnumerateSwapchainImages(swapchain->handle, swapchain->imageCount,
			&swapchain->imageCount, (XrSwapchainImageBaseHeader *)swapchain->images))) {
		return qfalse;
	}

	swapchain->depthBuffers = Z_Malloc(swapchain->imageCount * sizeof(GLuint));
	swapchain->frameBuffers = Z_Malloc(swapchain->imageCount * sizeof(GLuint));

	glGenRenderbuffers(swapchain->imageCount, swapchain->depthBuffers);
	VR_GenFramebuffers(swapchain->imageCount, swapchain->frameBuffers);

	// Both attachments have to carry the same sample count or the framebuffer
	// is incomplete, so the depth renderbuffer is multisampled alongside the
	// colour attachment even though nothing ever reads it back.
	if (samples > 1 && (!vr.glRenderbufferStorageMultisampleEXT || !vr.glFramebufferTexture2DMultisampleEXT)) {
		samples = 1;
	}

	for (i = 0; i < swapchain->imageCount; i++) {
		glBindRenderbuffer(GL_RENDERBUFFER, swapchain->depthBuffers[i]);
		if (samples > 1) {
			vr.glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
				swapchain->width, swapchain->height);
		} else {
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, swapchain->width, swapchain->height);
		}
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		VR_BindFramebuffer(swapchain->frameBuffers[i]);
		if (samples > 1) {
			vr.glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				swapchain->images[i].image, 0, samples);
		} else {
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				swapchain->images[i].image, 0);
		}
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
			swapchain->depthBuffers[i]);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			Com_Printf("OpenXR: incomplete eye framebuffer %u\n", i);
			VR_BindFramebuffer(0);
			return qfalse;
		}
	}

	VR_BindFramebuffer(0);
	return qtrue;
}

/*
==================
VR_CreateSession
==================
*/
void VR_CreateSession(void)
{
	XrGraphicsBindingOpenGLESAndroidKHR binding;
	XrSessionCreateInfo                 createInfo;
	XrReferenceSpaceCreateInfo          spaceInfo;
	EGLDisplay                          display;
	EGLContext                          context;
	EGLint                              configId = 0;
	EGLConfig                           config = NULL;
	EGLint                              numConfigs = 0;
	int                                 i;

	if (!vr.enabled || vr.session != XR_NULL_HANDLE) {
		return;
	}

	display = eglGetCurrentDisplay();
	context = eglGetCurrentContext();

	// Nothing is current, and on this device that is the normal case rather
	// than an error.
	//
	// SDL binds the GL context against the window's EGL surface. In a headset
	// there is no window being presented - every pixel goes to the compositor
	// through the swapchains - so that surface may never arrive. What SDL does
	// then is the trap: SDL_EGL_MakeCurrent, handed no surface, calls
	// eglMakeCurrent(EGL_NO_SURFACE, EGL_NO_CONTEXT) to unbind everything and
	// *returns success*, and SDL records the context as current. So SDL is
	// certain a context is bound while EGL reports none, and asking SDL to bind
	// it again does nothing at all - SDL_GL_MakeCurrent sees its own bookkeeping
	// agree and returns early.
	//
	// RTCWQuest never has this problem because it never depends on a window
	// surface: it makes its context current against a 16x16 pbuffer
	// (TBXR_Common.c, egl->TinySurface). Same thing here, except the context is
	// the one SDL already made, since the engine renders in it.
	if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		context = (EGLContext)SDL_GL_GetCurrentContext();

		if (display == EGL_NO_DISPLAY || !context) {
			Com_Printf("OpenXR: no EGL context to bind (display %p, context %p); "
				"cannot create a session\n", (void *)display, (void *)context);
			return;
		}
	}

	// The runtime wants the EGLConfig the context was made with, which EGL will
	// only hand back by id. Needed for the binding below, and to make a surface
	// the context will accept.
	{
		const EGLint attribs[] = { EGL_CONFIG_ID, 0, EGL_NONE };
		EGLint       queryAttribs[3];

		memcpy(queryAttribs, attribs, sizeof(attribs));
		eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
		queryAttribs[1] = configId;

		if (!eglChooseConfig(display, queryAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
			Com_Printf("OpenXR: could not recover the EGLConfig (id %d)\n", configId);
			config = NULL;
		}
	}

	if (eglGetCurrentContext() != context) {
		static const EGLint pbufferAttribs[] = {
			EGL_WIDTH, 16,
			EGL_HEIGHT, 16,
			EGL_NONE
		};

		if (vr.eglTinySurface == EGL_NO_SURFACE && config) {
			vr.eglTinySurface = eglCreatePbufferSurface(display, config, pbufferAttribs);
		}

		if (vr.eglTinySurface == EGL_NO_SURFACE) {
			Com_Printf("OpenXR: could not create the pbuffer to bind the context against "
				"(egl error 0x%x); cannot create a session\n", eglGetError());
			return;
		}

		if (!eglMakeCurrent(display, vr.eglTinySurface, vr.eglTinySurface, context)) {
			Com_Printf("OpenXR: eglMakeCurrent on the pbuffer failed (egl error 0x%x); "
				"cannot create a session\n", eglGetError());
			return;
		}

		Com_Printf("OpenXR: bound the engine's GL context against a 16x16 pbuffer; "
			"the window had no surface\n");
	}

	memset(&binding, 0, sizeof(binding));
	binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
	binding.display = display;
	binding.config = config;
	binding.context = context;

	memset(&createInfo, 0, sizeof(createInfo));
	createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	createInfo.next = &binding;
	createInfo.systemId = vr.systemId;

	if (!XR_CHECK(xrCreateSession(vr.instance, &createInfo, &vr.session))) {
		vr.session = XR_NULL_HANDLE;
		return;
	}

	// STAGE is the room-scale space: its origin is on the floor, which is what
	// makes standing and ducking translate into real movement.
	memset(&spaceInfo, 0, sizeof(spaceInfo));
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

	if (!XR_CHECK(xrCreateReferenceSpace(vr.session, &spaceInfo, &vr.stageSpace))) {
		// Not every runtime offers a stage; local is seated but always present.
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		XR_CHECK(xrCreateReferenceSpace(vr.session, &spaceInfo, &vr.stageSpace));
	}

	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(vr.session, &spaceInfo, &vr.viewSpace));

	VR_InitMultisampling();

	// Two attempts: a driver that advertises the extension can still refuse the
	// combination of formats, and losing VR over an image quality setting would
	// be a poor trade. Both eyes are rebuilt together so they cannot end up with
	// different sample counts.
	for (;;) {
		qboolean ok = qtrue;

		for (i = 0; i < VR_MAX_EYES; i++) {
			if (!VR_CreateSwapchain(&vr.swapchains[i], vr.eyeWidth, vr.eyeHeight, vr.samples)) {
				ok = qfalse;
				break;
			}
		}

		if (ok) {
			break;
		}

		for (i = 0; i < VR_MAX_EYES; i++) {
			VR_DestroySwapchain(&vr.swapchains[i]);
		}

		if (vr.samples <= 1) {
			Com_Printf("OpenXR: failed to create the eye swapchains\n");
			VR_DestroySession();
			return;
		}

		Com_Printf("OpenXR: %dx MSAA eye framebuffer incomplete, falling back to 1x\n", vr.samples);
		vr.samples = 1;
	}

	// Single sampled deliberately: the panel is a blit target for a buffer the
	// 2D path already drew, so there are no edges here for extra samples to
	// find.
	if (!VR_CreateSwapchain(&vr.uiSwapchain, vr.eyeWidth, vr.eyeHeight, 1)) {
		Com_Printf("OpenXR: failed to create the screen layer swapchain\n");
		VR_DestroySession();
		return;
	}

	glGenTextures(1, &vr.uiTexture);
	glBindTexture(GL_TEXTURE_2D, vr.uiTexture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, vr.eyeWidth, vr.eyeHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	VR_GenFramebuffers(1, &vr.uiFramebuffer);
	VR_BindFramebuffer(vr.uiFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr.uiTexture, 0);
	VR_GLDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	VR_BindFramebuffer(0);

	VR_CreateActions();
	VR_CreateBeam();

	Com_Printf("OpenXR: session created (%u images per eye)\n", vr.swapchains[0].imageCount);
}

/*
==================
VR_CreateActions

One action set with an aim pose and a select button per hand, which is all the
menus need. Bindings are suggested for the Touch controllers and for the
generic profile, so a runtime that reports something else still gets a pointer.
==================
*/
static void VR_CreateActions(void)
{
	XrActionSetCreateInfo         setInfo;
	XrActionCreateInfo            actionInfo;
	XrActionSpaceCreateInfo       spaceInfo;
	XrSessionActionSetsAttachInfo attachInfo;
	XrActionSuggestedBinding      bindings[16];
	XrInteractionProfileSuggestedBinding suggested;
	XrPath                        profile;
	int                           i;

	static const char *profiles[] = {
		"/interaction_profiles/oculus/touch_controller",
		"/interaction_profiles/khr/simple_controller",
	};
	static const char *selectBindings[] = {
		"/user/hand/%s/input/trigger/value",
		"/user/hand/%s/input/select/click",
	};

	Com_Printf("OpenXR: creating controller actions\n");

	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	Q_strncpyz(setInfo.actionSetName, "gameplay", sizeof(setInfo.actionSetName));
	Q_strncpyz(setInfo.localizedActionSetName, "Gameplay", sizeof(setInfo.localizedActionSetName));

	if (!XR_CHECK(xrCreateActionSet(vr.instance, &setInfo, &vr.actionSet))) {
		return;
	}

	xrStringToPath(vr.instance, "/user/hand/left", &vr.handPaths[0]);
	xrStringToPath(vr.instance, "/user/hand/right", &vr.handPaths[1]);

	memset(&actionInfo, 0, sizeof(actionInfo));
	actionInfo.type = XR_TYPE_ACTION_CREATE_INFO;
	actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
	actionInfo.countSubactionPaths = 2;
	actionInfo.subactionPaths = vr.handPaths;
	Q_strncpyz(actionInfo.actionName, "aim", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Aim", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.aimAction))) {
		return;
	}

	actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	Q_strncpyz(actionInfo.actionName, "select", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Select", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.selectAction))) {
		return;
	}

	actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
	actionInfo.countSubactionPaths = 0;
	actionInfo.subactionPaths = NULL;
	Q_strncpyz(actionInfo.actionName, "move", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Move", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.moveAction))) {
		return;
	}

	Q_strncpyz(actionInfo.actionName, "turn", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Turn", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.turnAction))) {
		return;
	}

	actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	Q_strncpyz(actionInfo.actionName, "objectives", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Objectives", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.objectivesAction))) {
		return;
	}

	Q_strncpyz(actionInfo.actionName, "jump", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Jump", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.jumpAction))) {
		return;
	}

	Q_strncpyz(actionInfo.actionName, "duck", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Duck", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.duckAction))) {
		return;
	}

	Q_strncpyz(actionInfo.actionName, "use", sizeof(actionInfo.actionName));
	Q_strncpyz(actionInfo.localizedActionName, "Use", sizeof(actionInfo.localizedActionName));

	if (!XR_CHECK(xrCreateAction(vr.actionSet, &actionInfo, &vr.useAction))) {
		return;
	}

	for (i = 0; i < (int)ARRAY_LEN(profiles); i++) {
		const char *hands[2] = { "left", "right" };
		uint32_t    count = 0;
		int         hand;

		for (hand = 0; hand < 2; hand++) {
			char path[XR_MAX_PATH_LENGTH];

			Com_sprintf(path, sizeof(path), "/user/hand/%s/input/aim/pose", hands[hand]);
			bindings[count].action = vr.aimAction;
			if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
				count++;
			}

			Com_sprintf(path, sizeof(path), selectBindings[i], hands[hand]);
			bindings[count].action = vr.selectAction;
			if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
				count++;
			}

			// Thumbsticks and face buttons only exist on the Touch profile; the
			// simple controller has neither, and a binding it does not know
			// would have the runtime reject the whole set.
			if (i == 0) {
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/thumbstick", hands[hand]);
				bindings[count].action = (hand == 0) ? vr.moveAction : vr.turnAction;
				if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
					count++;
				}

				// The upper face button on either hand: Y on the left, B on the
				// right. Both, because which hand is free depends on which one
				// the player is wearing the panel on.
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/%s/click",
					hands[hand], (hand == 0) ? "y" : "b");
				bindings[count].action = vr.objectivesAction;
				if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
					count++;
				}

				// And the lower one for jump: X on the left, A on the right.
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/%s/click",
					hands[hand], (hand == 0) ? "x" : "a");
				bindings[count].action = vr.jumpAction;
				if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
					count++;
				}

				// Duck goes on the left stick, since the four face buttons are
				// spoken for and pressing the stick down to crouch is at least a
				// gesture in the right direction. Use goes on the right one:
				// opening doors belongs with the hand that holds the weapon.
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/thumbstick/click", hands[hand]);
				bindings[count].action = (hand == 0) ? vr.duckAction : vr.useAction;
				if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
					count++;
				}

				// And on the grips as well, both of them, because reaching for a
				// door handle with the grip is the thing people try first.
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/squeeze/value", hands[hand]);
				bindings[count].action = vr.useAction;
				if (XR_SUCCEEDED(xrStringToPath(vr.instance, path, &bindings[count].binding))) {
					count++;
				}
			}
		}

		if (!XR_SUCCEEDED(xrStringToPath(vr.instance, profiles[i], &profile))) {
			continue;
		}

		memset(&suggested, 0, sizeof(suggested));
		suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
		suggested.interactionProfile = profile;
		suggested.countSuggestedBindings = count;
		suggested.suggestedBindings = bindings;

		// A runtime rejecting a profile it does not know is expected, not an
		// error; the others still apply.
		xrSuggestInteractionProfileBindings(vr.instance, &suggested);
	}

	for (i = 0; i < 2; i++) {
		memset(&spaceInfo, 0, sizeof(spaceInfo));
		spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
		spaceInfo.action = vr.aimAction;
		spaceInfo.subactionPath = vr.handPaths[i];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;

		XR_CHECK(xrCreateActionSpace(vr.session, &spaceInfo, &vr.aimSpaces[i]));
	}

	memset(&attachInfo, 0, sizeof(attachInfo));
	attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &vr.actionSet;

	if (!XR_CHECK(xrAttachSessionActionSets(vr.session, &attachInfo))) {
		return;
	}

	vr.actionsReady = qtrue;
	vr.pointerHand = 1;

	Com_Printf("OpenXR: controllers ready\n");
}

/*
==================
VR_RotateVector

Turns a vector by a quaternion.
==================
*/
static void VR_RotateVector(const XrQuaternionf *q, const XrVector3f *in, XrVector3f *out)
{
	const float x = q->x, y = q->y, z = q->z, w = q->w;
	XrVector3f   t;

	// t = 2 * cross(q.xyz, v)
	t.x = 2.0f * (y * in->z - z * in->y);
	t.y = 2.0f * (z * in->x - x * in->z);
	t.z = 2.0f * (x * in->y - y * in->x);

	// out = v + w * t + cross(q.xyz, t)
	out->x = in->x + w * t.x + (y * t.z - z * t.y);
	out->y = in->y + w * t.y + (z * t.x - x * t.z);
	out->z = in->z + w * t.z + (x * t.y - y * t.x);
}

/*
==================
VR_PointAtPanel

Casts the hand's aim ray at a panel and, if it lands on it, moves the engine's
cursor to the same spot. The engine then draws its own cursor there, so the
menus behave as they always did.

Takes the panel rather than assuming one, because there are two: the big one
fixed in the room while the world is not being drawn, and the small one on the
player's wrist while it is.
==================
*/
static qboolean VR_PointAtPanel(const XrPosef *aim, const XrPosef *panel, float size,
	float cropX, float cropY, float cropWidth, float cropHeight)
{
	XrVector3f  forward  = { 0.0f, 0.0f, -1.0f };
	XrVector3f  rightAxis = { 1.0f, 0.0f, 0.0f };
	XrVector3f  upAxis   = { 0.0f, 1.0f, 0.0f };
	XrVector3f  normalAxis = { 0.0f, 0.0f, 1.0f };
	XrVector3f  dir, right, up, normal, toPlane, hit;
	float       denom, distance, localX, localY;
	float       halfWidth, halfHeight;

	halfWidth = size * 0.5f;
	halfHeight = halfWidth
		* (cropHeight * (float)vr.uiSwapchain.height)
		/ (cropWidth * (float)vr.uiSwapchain.width);

	VR_RotateVector(&aim->orientation, &forward, &dir);
	VR_RotateVector(&panel->orientation, &rightAxis, &right);
	VR_RotateVector(&panel->orientation, &upAxis, &up);
	VR_RotateVector(&panel->orientation, &normalAxis, &normal);

	denom = dir.x * normal.x + dir.y * normal.y + dir.z * normal.z;

	// Parallel to the panel, or aiming at its back.
	if (fabsf(denom) < 0.0001f) {
		return qfalse;
	}

	toPlane.x = panel->position.x - aim->position.x;
	toPlane.y = panel->position.y - aim->position.y;
	toPlane.z = panel->position.z - aim->position.z;

	distance = (toPlane.x * normal.x + toPlane.y * normal.y + toPlane.z * normal.z) / denom;

	if (distance <= 0.0f) {
		return qfalse;
	}

	hit.x = aim->position.x + dir.x * distance - panel->position.x;
	hit.y = aim->position.y + dir.y * distance - panel->position.y;
	hit.z = aim->position.z + dir.z * distance - panel->position.z;

	localX = hit.x * right.x + hit.y * right.y + hit.z * right.z;
	localY = hit.x * up.x + hit.y * up.y + hit.z * up.z;

	if (fabsf(localX) > halfWidth || fabsf(localY) > halfHeight) {
		return qfalse;
	}

	// Panel space is centred and Y up; the engine's screen is corner based and
	// Y down. The crop sits between the two: what the player sees is a window
	// onto the screen, so a hit halfway across the panel is halfway across the
	// window, not halfway across the screen.
	CL_SetMousePos(
		(int)((cropX + (localX / halfWidth * 0.5f + 0.5f) * cropWidth) * cls.glconfig.vidWidth),
		(int)((cropY + (0.5f - localY / halfHeight * 0.5f) * cropHeight) * cls.glconfig.vidHeight));

	// How far the beam has to travel to reach what it is pointing at.
	vr.pointerDistance = distance;

	return qtrue;
}

/*
==================
VR_WristCrop

The part of the screen the wrist quad shows, as fractions with the origin at the
top left - the same way round as the engine's own screen coordinates, so the
numbers mean what they look like they mean.

Kept in one place because three things have to agree about it: the shader that
samples it, the quad's aspect ratio, and the ray that puts the cursor on it.
==================
*/
static void VR_WristCrop(float *x, float *y, float *width, float *height)
{
	float cx = vr.vr_wristCropX->value;
	float cy = vr.vr_wristCropY->value;
	float cw = vr.vr_wristCropW->value;
	float ch = vr.vr_wristCropH->value;

	if (cw <= 0.0f || cw > 1.0f) {
		cw = 1.0f;
	}
	if (ch <= 0.0f || ch > 1.0f) {
		ch = 1.0f;
	}

	// Clamped so a crop cannot be asked to run off the edge of the buffer, which
	// the runtime treats as a hard error rather than something to tidy up.
	if (cx < 0.0f) {
		cx = 0.0f;
	} else if (cx > 1.0f - cw) {
		cx = 1.0f - cw;
	}
	if (cy < 0.0f) {
		cy = 0.0f;
	} else if (cy > 1.0f - ch) {
		cy = 1.0f - ch;
	}

	*x = cx;
	*y = cy;
	*width = cw;
	*height = ch;
}

/*
==================
VR_QuatFromAxes

A quaternion from three orthonormal axes, given as the columns of the rotation.
==================
*/
static void VR_QuatFromAxes(const XrVector3f *x, const XrVector3f *y, const XrVector3f *z,
	XrQuaternionf *q)
{
	const float trace = x->x + y->y + z->z;

	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;

		q->w = 0.25f * s;
		q->x = (y->z - z->y) / s;
		q->y = (z->x - x->z) / s;
		q->z = (x->y - y->x) / s;
	} else if (x->x > y->y && x->x > z->z) {
		const float s = sqrtf(1.0f + x->x - y->y - z->z) * 2.0f;

		q->w = (y->z - z->y) / s;
		q->x = 0.25f * s;
		q->y = (y->x + x->y) / s;
		q->z = (z->x + x->z) / s;
	} else if (y->y > z->z) {
		const float s = sqrtf(1.0f + y->y - x->x - z->z) * 2.0f;

		q->w = (z->x - x->z) / s;
		q->x = (y->x + x->y) / s;
		q->y = 0.25f * s;
		q->z = (z->y + y->z) / s;
	} else {
		const float s = sqrtf(1.0f + z->z - x->x - y->y) * 2.0f;

		q->w = (x->y - y->x) / s;
		q->x = (z->x + x->z) / s;
		q->y = (z->y + y->z) / s;
		q->z = 0.25f * s;
	}
}

/*
==================
VR_UpdateWristPanel

Raise the off hand and turn it towards your face and the panel appears on it,
the way a watch is read. No button spent on it, and it is the gesture people try
first.

The thresholds are deliberately different going in and coming out. A single
threshold sits exactly where the hand rests while being read, so the panel
strobes on and off at the very moment it is being looked at.
==================
*/
static void VR_UpdateWristPanel(void)
{
	static const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	static const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	XrVector3f              head, toHead, handUp, handForward;
	XrVector3f              axisX, axisY, axisZ;
	float                   distance, facing, length;
	float                   offset, back;

	if (!vr.handPoseValid[0] || !vr.viewsValid
		|| (vr.vr_wristPanel && !vr.vr_wristPanel->integer)) {
		vr.wristVisible = qfalse;
		return;
	}

	head.x = (vr.views[0].pose.position.x + vr.views[1].pose.position.x) * 0.5f;
	head.y = (vr.views[0].pose.position.y + vr.views[1].pose.position.y) * 0.5f;
	head.z = (vr.views[0].pose.position.z + vr.views[1].pose.position.z) * 0.5f;

	toHead.x = head.x - vr.handPoses[0].position.x;
	toHead.y = head.y - vr.handPoses[0].position.y;
	toHead.z = head.z - vr.handPoses[0].position.z;

	distance = sqrtf(toHead.x * toHead.x + toHead.y * toHead.y + toHead.z * toHead.z);

	if (distance < 0.0001f) {
		vr.wristVisible = qfalse;
		return;
	}

	toHead.x /= distance;
	toHead.y /= distance;
	toHead.z /= distance;

	// Which way the back of the hand is pointing. Turning the wrist over to
	// read it swings this towards the face.
	VR_RotateVector(&vr.handPoses[0].orientation, &localUp, &handUp);

	facing = handUp.x * toHead.x + handUp.y * toHead.y + handUp.z * toHead.z;

	if (vr.wristVisible) {
		vr.wristVisible = (distance < 0.85f && facing > 0.20f) ? qtrue : qfalse;
	} else {
		vr.wristVisible = (distance < 0.65f && facing > 0.50f) ? qtrue : qfalse;
	}

	if (!vr.wristVisible) {
		return;
	}

	// Worn, not floating: the panel is strapped to the back of the hand and goes
	// wherever the hand goes, so turning the wrist turns it. Facing the eye
	// instead would read as a thing hovering near the arm rather than a thing on
	// it, and it would slide about whenever the head moved.
	//
	// Out of the back of the hand is the way the panel faces; along the fingers
	// is its up. Both come straight off the controller pose.
	VR_RotateVector(&vr.handPoses[0].orientation, &localForward, &handForward);

	axisZ = handUp;
	axisY = handForward;

	// X from the other two, then Y rebuilt from X and Z, so the three are
	// square even if the pose's own axes are not quite.
	axisX.x = axisY.y * axisZ.z - axisY.z * axisZ.y;
	axisX.y = axisY.z * axisZ.x - axisY.x * axisZ.z;
	axisX.z = axisY.x * axisZ.y - axisY.y * axisZ.x;

	length = sqrtf(axisX.x * axisX.x + axisX.y * axisX.y + axisX.z * axisX.z);

	if (length < 0.0001f) {
		vr.wristVisible = qfalse;
		return;
	}

	axisX.x /= length;
	axisX.y /= length;
	axisX.z /= length;

	axisY.x = axisZ.y * axisX.z - axisZ.z * axisX.y;
	axisY.y = axisZ.z * axisX.x - axisZ.x * axisX.z;
	axisY.z = axisZ.x * axisX.y - axisZ.y * axisX.x;

	// Stood off the back of the hand, and back along the forearm towards where a
	// watch would actually sit - the controller pose is out at the fingers.
	offset = vr.vr_wristDistance->value;
	back = vr.vr_wristBack->value;

	vr.wristPose.position.x = vr.handPoses[0].position.x + handUp.x * offset - handForward.x * back;
	vr.wristPose.position.y = vr.handPoses[0].position.y + handUp.y * offset - handForward.y * back;
	vr.wristPose.position.z = vr.handPoses[0].position.z + handUp.z * offset - handForward.z * back;

	VR_QuatFromAxes(&axisX, &axisY, &axisZ, &vr.wristPose.orientation);
}

/*
==================
VR_WristPanelVisible
==================
*/
qboolean VR_WristPanelVisible(void)
{
	return vr.wristVisible;
}

/*
==================
VR_WristPanelEnabled

Whether the HUD goes on the wrist at all. Off by default: the HUD is drawn in
the eye buffers instead, converged and pulled in from the edges - see
RB_SetGL2D. That keeps it always readable without a gesture, at the size it was
designed to be, rather than a slice of a screen shown on a small quad.
==================
*/
qboolean VR_WristPanelEnabled(void)
{
	return (vr.vr_wristPanel && vr.vr_wristPanel->integer) ? qtrue : qfalse;
}

/*
==================
VR_UpdateHeldButton

Turns a button into a held console command, sending both edges. The commands
these drive are all of the "while this is down" kind, so a toggle would leave
the game holding a key the player has let go of.
==================
*/
static void VR_UpdateHeldButton(XrAction action, qboolean *wasDown,
	const char *press, const char *release)
{
	XrActionStateGetInfo getInfo;
	XrActionStateBoolean state;
	qboolean             down = qfalse;

	memset(&getInfo, 0, sizeof(getInfo));
	getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
	getInfo.action = action;

	memset(&state, 0, sizeof(state));
	state.type = XR_TYPE_ACTION_STATE_BOOLEAN;

	if (XR_SUCCEEDED(xrGetActionStateBoolean(vr.session, &getInfo, &state))
		&& state.isActive && state.currentState) {
		down = qtrue;
	}

	if (down != *wasDown) {
		Cbuf_AddText(down ? press : release);
		*wasDown = down;
	}
}

/*
==================
VR_UpdateInput
==================
*/
void VR_UpdateInput(void)
{
	XrActiveActionSet        activeSet;
	XrActionsSyncInfo        syncInfo;
	XrActionStateGetInfo     getInfo;
	XrActionStateBoolean     selectState;
	XrSpaceLocation          location;
	qboolean                 selectDown = qfalse;
	qboolean                 onScreen = qfalse;
	int                      hand;

	if (!vr.actionsReady || !vr.sessionRunning || !vr.frameStarted) {
		return;
	}

	memset(&activeSet, 0, sizeof(activeSet));
	activeSet.actionSet = vr.actionSet;
	activeSet.subactionPath = XR_NULL_PATH;

	memset(&syncInfo, 0, sizeof(syncInfo));
	syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;

	if (!XR_CHECK(xrSyncActions(vr.session, &syncInfo))) {
		return;
	}

	// Where each hand is, kept for the beam as well as the cursor. Both are
	// located every frame rather than stopping at the first hit, so the hand
	// that is not pointing at the panel still has something to draw.
	vr.handPoseValid[0] = vr.handPoseValid[1] = qfalse;
	vr.pointerDistance = 0.0f;

	for (hand = 0; hand < 2; hand++) {
		memset(&location, 0, sizeof(location));
		location.type = XR_TYPE_SPACE_LOCATION;

		if (!XR_SUCCEEDED(xrLocateSpace(vr.aimSpaces[hand], vr.stageSpace,
				vr.frameState.predictedDisplayTime, &location))
			|| !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			|| !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			continue;
		}

		vr.handPoses[hand] = location.pose;
		vr.handPoseValid[hand] = qtrue;
	}

	// Whether the player is reading their wrist, which decides both what the
	// pointer aims at and whether there is a beam to see.
	VR_UpdateWristPanel();

	// Whichever hand is pointing at the panel drives the cursor, so it does not
	// matter which one the player picks up. The hand that had it keeps it while
	// it is still on target, which stops the cursor jumping when both point.
	//
	// Which panel depends on what is up: the big one in the room when the world
	// is not being drawn, the one on the wrist when it is. Never both.
	for (hand = 0; hand < 2 && !onScreen; hand++) {
		int which = (vr.pointerHand + hand) & 1;

		if (!vr.handPoseValid[which]) {
			continue;
		}

		if (VR_UseScreenLayer()) {
			const float size = vr.vr_screenSize->value > 0.0f ? vr.vr_screenSize->value : 3.0f;

			// The room sized panel shows the whole screen, uncropped.
			if (vr.screenAnchorValid
				&& VR_PointAtPanel(&vr.handPoses[which], &vr.screenAnchor, size,
					0.0f, 0.0f, 1.0f, 1.0f)) {
				vr.pointerHand = which;
				onScreen = qtrue;
			}
		} else if (vr.wristVisible) {
			const float size = vr.vr_wristSize->value > 0.0f ? vr.vr_wristSize->value : 0.26f;
			float       cropX, cropY, cropWidth, cropHeight;

			VR_WristCrop(&cropX, &cropY, &cropWidth, &cropHeight);

			// Not the hand wearing it: pointing a controller at itself is not a
			// gesture anyone can make.
			if (which != 0
				&& VR_PointAtPanel(&vr.handPoses[which], &vr.wristPose, size,
					cropX, cropY, cropWidth, cropHeight)) {
				vr.pointerHand = which;
				onScreen = qtrue;
			}
		}
	}

	memset(&getInfo, 0, sizeof(getInfo));
	getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
	getInfo.action = vr.selectAction;

	for (hand = 0; hand < 2; hand++) {
		getInfo.subactionPath = vr.handPaths[hand];

		memset(&selectState, 0, sizeof(selectState));
		selectState.type = XR_TYPE_ACTION_STATE_BOOLEAN;

		if (XR_SUCCEEDED(xrGetActionStateBoolean(vr.session, &getInfo, &selectState))
			&& selectState.isActive && selectState.currentState) {
			selectDown = qtrue;
		}
	}

	// Edges only, so holding the trigger does not repeat.
	if (selectDown != vr.selectWasDown) {
		Com_QueueEvent(0, SE_KEY, K_MOUSE1, selectDown, 0, NULL);
		vr.selectWasDown = selectDown;
	}

	// Objectives and jump. Both are held commands - the objectives list fades in
	// while the key is down, and a jump is a held +moveup the same as it is on a
	// keyboard - so each button sends both edges rather than toggling.
	VR_UpdateHeldButton(vr.objectivesAction, &vr.objectivesWasDown, "+scores\n", "-scores\n");
	VR_UpdateHeldButton(vr.jumpAction, &vr.jumpWasDown, "+moveup\n", "-moveup\n");
	VR_UpdateHeldButton(vr.duckAction, &vr.duckWasDown, "+movedown\n", "-movedown\n");
	VR_UpdateHeldButton(vr.useAction, &vr.useWasDown, "+use\n", "-use\n");
}

/*
==================
VR_AimForward

A controller's forward direction, in the same engine frame and with the same
recentring as the head, so hand and head headings can be compared directly.
==================
*/
static void VR_AimForward(const XrQuaternionf *q, vec3_t forward)
{
	XrVector3f  back = { 0.0f, 0.0f, -1.0f };
	XrVector3f  dir;

	VR_RotateVector(q, &back, &dir);

	forward[0] = -dir.z;
	forward[1] = -dir.x;
	forward[2] =  dir.y;

	if (vr.yawOffset != 0.0f) {
		const float radians = -vr.yawOffset * (float)M_PI / 180.0f;
		const float c = cosf(radians);
		const float sn = sinf(radians);
		const float x = forward[0];
		const float y = forward[1];

		forward[0] = x * c - y * sn;
		forward[1] = x * sn + y * c;
	}

	VectorNormalize(forward);
}

/*
==================
VR_GetInput

The sticks, and where the head is pointing relative to where the player
started. Heading is measured from the same reference the view uses, so the
game's idea of which way the player faces matches what they see.
==================
*/
qboolean VR_GetInput(vrInput_t *input)
{
	XrActionStateGetInfo  getInfo;
	XrActionStateVector2f stick;
	vec3_t                angles;

	memset(input, 0, sizeof(*input));

	if (!vr.actionsReady || !vr.sessionRunning || !vr.viewsValid) {
		return qfalse;
	}

	memset(&getInfo, 0, sizeof(getInfo));
	getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;

	memset(&stick, 0, sizeof(stick));
	stick.type = XR_TYPE_ACTION_STATE_VECTOR2F;
	getInfo.action = vr.moveAction;

	if (XR_SUCCEEDED(xrGetActionStateVector2f(vr.session, &getInfo, &stick)) && stick.isActive) {
		input->moveRight = stick.currentState.x;
		input->moveForward = stick.currentState.y;
	}

	memset(&stick, 0, sizeof(stick));
	stick.type = XR_TYPE_ACTION_STATE_VECTOR2F;
	getInfo.action = vr.turnAction;

	if (XR_SUCCEEDED(xrGetActionStateVector2f(vr.session, &getInfo, &stick)) && stick.isActive) {
		// Sideways only. Pushing this stick up or down is how the player reaches
		// for whatever else ends up on it, and no thumb makes that movement
		// without carrying some sideways component along with it - easily enough
		// to snap the view a step round if it is taken at face value.
		if (fabsf(stick.currentState.x) > fabsf(stick.currentState.y)) {
			input->turn = stick.currentState.x;
		}
	}

	// Room scale. The headset's own movement is handed to the game as movement
	// input, not just left as a camera offset: an offset moves the view and
	// leaves the character standing where it was, so leaning through a wall
	// works and walking anywhere does not. What the player does with their feet
	// has to reach the same pmove that the stick does.
	{
		vec3_t delta;

		if (vr.stepValid) {
			VectorSubtract(vr.eyeViews[0].origin, vr.lastHeadOrigin, delta);
		} else {
			VectorClear(delta);
			vr.stepValid = qtrue;
		}

		VectorCopy(vr.eyeViews[0].origin, vr.lastHeadOrigin);

		input->stepForward = delta[0];
		// The engine's second axis points left, the usercmd's rightmove right.
		input->stepRight = -delta[1];
	}

	// The eye views already carry the recentred orientation, so read the
	// heading back out of them rather than recomputing it from the raw pose.
	vectoangles(vr.eyeViews[0].axis[0], angles);
	input->headYaw = angles[YAW];
	input->headPitch = angles[PITCH];

	// Both hands, in the same frame as the head so the two can be compared.
	{
		XrSpaceLocation location;
		int             hand;

		for (hand = 0; hand < 2; hand++) {
			vec3_t forward, handAngles;

			memset(&location, 0, sizeof(location));
			location.type = XR_TYPE_SPACE_LOCATION;

			if (!XR_SUCCEEDED(xrLocateSpace(vr.aimSpaces[hand], vr.stageSpace,
					vr.frameState.predictedDisplayTime, &location))
				|| !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
				continue;
			}

			VR_AimForward(&location.pose.orientation, forward);
			vectoangles(forward, handAngles);

			if (hand == 0) {
				input->offhandYaw = handAngles[YAW];
				input->offhandTracked = qtrue;
			} else {
				input->weaponYaw = handAngles[YAW];
				input->weaponPitch = handAngles[PITCH];
				input->weaponTracked = qtrue;
			}
		}
	}

	input->valid = qtrue;
	return qtrue;
}

/*
==================
VR_DestroySession
==================
*/
void VR_DestroySession(void)
{
	int i;

	if (vr.session == XR_NULL_HANDLE) {
		return;
	}

	for (i = 0; i < VR_MAX_EYES; i++) {
		VR_DestroySwapchain(&vr.swapchains[i]);
	}

	VR_DestroySwapchain(&vr.uiSwapchain);
	VR_DestroyBeam();

	if (vr.uiFramebuffer) {
		VR_DeleteFramebuffers(1, &vr.uiFramebuffer);
		vr.uiFramebuffer = 0;
	}

	if (vr.uiTexture) {
		glDeleteTextures(1, &vr.uiTexture);
		vr.uiTexture = 0;
	}

	for (i = 0; i < 2; i++) {
		if (vr.aimSpaces[i] != XR_NULL_HANDLE) {
			xrDestroySpace(vr.aimSpaces[i]);
			vr.aimSpaces[i] = XR_NULL_HANDLE;
		}
	}

	if (vr.actionSet != XR_NULL_HANDLE) {
		xrDestroyActionSet(vr.actionSet);
		vr.actionSet = XR_NULL_HANDLE;
	}

	vr.actionsReady = qfalse;
	vr.selectWasDown = qfalse;

	if (vr.viewSpace != XR_NULL_HANDLE) {
		xrDestroySpace(vr.viewSpace);
		vr.viewSpace = XR_NULL_HANDLE;
	}

	if (vr.stageSpace != XR_NULL_HANDLE) {
		xrDestroySpace(vr.stageSpace);
		vr.stageSpace = XR_NULL_HANDLE;
	}

	xrDestroySession(vr.session);
	vr.session = XR_NULL_HANDLE;
	vr.sessionRunning = qfalse;
	vr.frameStarted = qfalse;
	vr.sessionState = XR_SESSION_STATE_UNKNOWN;

	// The pbuffer the context may be current against outlives the session on
	// purpose - the session is torn down and rebuilt around renderer restarts,
	// and unbinding the context in the middle of that would be the very thing
	// this exists to prevent. It goes at shutdown, in VR_Shutdown.
}

/*
==================
VR_Shutdown
==================
*/
void VR_Shutdown(void)
{
	VR_DestroySession();

	if (vr.instance != XR_NULL_HANDLE) {
		xrDestroyInstance(vr.instance);
		vr.instance = XR_NULL_HANDLE;
	}

	if (vr.eglTinySurface != EGL_NO_SURFACE) {
		EGLDisplay display = eglGetCurrentDisplay();

		if (display == EGL_NO_DISPLAY) {
			display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		}

		if (display != EGL_NO_DISPLAY) {
			eglDestroySurface(display, vr.eglTinySurface);
		}

		vr.eglTinySurface = EGL_NO_SURFACE;
	}

	vr.enabled = qfalse;
}

/*
==================
VR_HandleSessionStateChange
==================
*/
static void VR_HandleSessionStateChange(XrSessionState state)
{
	XrSessionBeginInfo beginInfo;

	vr.sessionState = state;

	switch (state) {
	case XR_SESSION_STATE_READY:
		memset(&beginInfo, 0, sizeof(beginInfo));
		beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
		beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

		if (XR_CHECK(xrBeginSession(vr.session, &beginInfo))) {
			vr.sessionRunning = qtrue;
			vr.refreshRateApplied = qfalse;
			Com_Printf("OpenXR: session running\n");
		}
		break;

	case XR_SESSION_STATE_STOPPING:
		vr.sessionRunning = qfalse;
		XR_CHECK(xrEndSession(vr.session));
		Com_Printf("OpenXR: session stopped\n");
		break;

	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		vr.sessionRunning = qfalse;
		Com_Printf("OpenXR: session lost\n");
		break;

	default:
		break;
	}
}

/*
==================
VR_PollEvents
==================
*/
static void VR_PollEvents(void)
{
	XrEventDataBuffer event;

	for (;;) {
		memset(&event, 0, sizeof(event));
		event.type = XR_TYPE_EVENT_DATA_BUFFER;

		if (xrPollEvent(vr.instance, &event) != XR_SUCCESS) {
			break;
		}

		switch (event.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			VR_HandleSessionStateChange(
				((XrEventDataSessionStateChanged *)&event)->state);
			break;

		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			Com_Printf("OpenXR: instance loss pending\n");
			vr.sessionRunning = qfalse;
			break;

		default:
			break;
		}
	}
}

/*
==================
VR_PoseToView

OpenXR is right handed, Y up, metres, looking down -Z. The engine is Quake's:
X forward, Y left, Z up, in units. vr_worldscale is how many units make a
metre.
==================
*/
static void VR_PoseToView(const XrPosef *pose, vrEyeView_t *view)
{
	const XrQuaternionf *q = &pose->orientation;
	float scale = vr.vr_worldscale->value;
	vec3_t forward, right, up;
	float  m[3][3];

	if (scale <= 0.0f) {
		scale = 32.0f;
	}

	view->origin[0] = -(pose->position.z - vr.trackingOrigin.z) * scale;
	view->origin[1] = -(pose->position.x - vr.trackingOrigin.x) * scale;
	view->origin[2] =  (pose->position.y - vr.trackingOrigin.y) * scale;

	if (vr.yawOffset != 0.0f) {
		const float radians = -vr.yawOffset * (float)M_PI / 180.0f;
		const float c = cosf(radians);
		const float sn = sinf(radians);
		const float x = view->origin[0];
		const float y = view->origin[1];

		view->origin[0] = x * c - y * sn;
		view->origin[1] = x * sn + y * c;
	}

	// Quaternion to a rotation matrix, in OpenXR's axes.
	m[0][0] = 1.0f - 2.0f * (q->y * q->y + q->z * q->z);
	m[0][1] =        2.0f * (q->x * q->y - q->z * q->w);
	m[0][2] =        2.0f * (q->x * q->z + q->y * q->w);
	m[1][0] =        2.0f * (q->x * q->y + q->z * q->w);
	m[1][1] = 1.0f - 2.0f * (q->x * q->x + q->z * q->z);
	m[1][2] =        2.0f * (q->y * q->z - q->x * q->w);
	m[2][0] =        2.0f * (q->x * q->z - q->y * q->w);
	m[2][1] =        2.0f * (q->y * q->z + q->x * q->w);
	m[2][2] = 1.0f - 2.0f * (q->x * q->x + q->y * q->y);

	// -Z in XR is the engine's forward, -X is left, +Y is up.
	forward[0] = -(-m[2][2]);
	forward[1] = -(-m[0][2]);
	forward[2] =  (-m[1][2]);

	right[0] = -m[2][0];
	right[1] = -m[0][0];
	right[2] =  m[1][0];

	up[0] = -m[2][1];
	up[1] = -m[0][1];
	up[2] =  m[1][1];

	VectorNormalize(forward);
	VectorNormalize(right);
	VectorNormalize(up);

	// Take out the heading the player happened to have when tracking started,
	// so their physical forward lines up with the character's.
	if (vr.yawOffset != 0.0f) {
		const float radians = -vr.yawOffset * (float)M_PI / 180.0f;
		const float c = cosf(radians);
		const float sn = sinf(radians);
		vec3_t     *axes[3] = { &forward, &right, &up };
		int         i;

		for (i = 0; i < 3; i++) {
			float x = (*axes[i])[0];
			float y = (*axes[i])[1];

			(*axes[i])[0] = x * c - y * sn;
			(*axes[i])[1] = x * sn + y * c;
		}
	}

	// viewaxis is forward, left, up - the engine's second axis points left,
	// not right.
	VectorCopy(forward, view->axis[0]);
	VectorSubtract(vec3_origin, right, view->axis[1]);
	VectorCopy(up, view->axis[2]);
}

/*
==================
VR_BeginFrame
==================
*/
qboolean VR_BeginFrame(void)
{
	XrFrameWaitInfo   waitInfo;
	XrFrameBeginInfo  beginInfo;
	XrViewLocateInfo  locateInfo;
	XrViewState       viewState;
	uint32_t          viewCount = 0;
	int               eye;

	if (!vr.enabled || vr.session == XR_NULL_HANDLE) {
		return qfalse;
	}

	VR_PollEvents();

	if (!vr.sessionRunning) {
		return qfalse;
	}

	// Not at xrBeginSession, which is the obvious place and the wrong one: the
	// Oculus runtime reports no rates at all that early. Retried from the frame
	// loop until it has some, then never again.
	if (!vr.refreshRateApplied) {
		VR_ApplyRefreshRate();
	}

	memset(&waitInfo, 0, sizeof(waitInfo));
	waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;

	memset(&vr.frameState, 0, sizeof(vr.frameState));
	vr.frameState.type = XR_TYPE_FRAME_STATE;

	{
		const int before = Sys_Milliseconds();
		const qboolean ok = XR_CHECK(xrWaitFrame(vr.session, &waitInfo, &vr.frameState));

		// Time spent here is the runtime pacing us, not work: it blocks until
		// the right moment to start the frame. It only grows when we are early,
		// so it should be near zero on anything that is running behind.
		vr.phaseWait += Sys_Milliseconds() - before;

		if (!ok) {
			return qfalse;
		}
	}

	memset(&beginInfo, 0, sizeof(beginInfo));
	beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;

	if (!XR_CHECK(xrBeginFrame(vr.session, &beginInfo))) {
		return qfalse;
	}

	vr.frameStarted = qtrue;
	vr.viewsValid = qfalse;

	memset(&locateInfo, 0, sizeof(locateInfo));
	locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = vr.frameState.predictedDisplayTime;
	locateInfo.space = vr.stageSpace;

	memset(&viewState, 0, sizeof(viewState));
	viewState.type = XR_TYPE_VIEW_STATE;

	if (XR_CHECK(xrLocateViews(vr.session, &locateInfo, &viewState, VR_MAX_EYES,
			&viewCount, vr.views))
		&& (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
		&& (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

		vr.viewsValid = qtrue;

		if (!vr.trackingOriginValid) {
			// Midway between the eyes is the head.
			vr.trackingOrigin.x = (vr.views[0].pose.position.x + vr.views[1].pose.position.x) * 0.5f;
			vr.trackingOrigin.y = (vr.views[0].pose.position.y + vr.views[1].pose.position.y) * 0.5f;
			vr.trackingOrigin.z = (vr.views[0].pose.position.z + vr.views[1].pose.position.z) * 0.5f;

			{
				const XrQuaternionf *q = &vr.views[0].pose.orientation;
				float yaw = atan2f(2.0f * (q->w * q->y + q->x * q->z),
				                   1.0f - 2.0f * (q->y * q->y + q->z * q->z));

				// XR yaw turns the opposite way to the engine's.
				vr.yawOffset = -yaw * 180.0f / M_PI;
			}

			vr.trackingOriginValid = qtrue;
		}

		{
			static int lastTrace;
			int now = Sys_Milliseconds();

			if (vr_traceTracking && vr_traceTracking->integer && now - lastTrace > 1000) {
				lastTrace = now;
				Com_Printf("VR head: xr pos %.3f %.3f %.3f  origin %.3f %.3f %.3f  scale %.0f\n",
					vr.views[0].pose.position.x, vr.views[0].pose.position.y, vr.views[0].pose.position.z,
					vr.trackingOrigin.x, vr.trackingOrigin.y, vr.trackingOrigin.z,
					vr.vr_worldscale->value);
			}
		}

		for (eye = 0; eye < VR_MAX_EYES; eye++) {
			VR_PoseToView(&vr.views[eye].pose, &vr.eyeViews[eye]);

			vr.eyeViews[eye].tanLeft  = tanf(vr.views[eye].fov.angleLeft);
			vr.eyeViews[eye].tanRight = tanf(vr.views[eye].fov.angleRight);
			vr.eyeViews[eye].tanUp    = tanf(vr.views[eye].fov.angleUp);
			vr.eyeViews[eye].tanDown  = tanf(vr.views[eye].fov.angleDown);
		}
	}

	VR_UpdateInput();

	// Once for the frame, not once per target: the same hands are drawn into
	// both eyes, and on a menu frame into both eye images again.
	vr.beamVertexCount = VR_BuildBeamGeometry(vr.beamVerts, ARRAY_LEN(vr.beamVerts) / 3);

	return vr.frameState.shouldRender ? qtrue : qfalse;
}

/*
==================
VR_GetEyeView
==================
*/
void VR_GetEyeView(int eye, vrEyeView_t *view)
{
	if (eye < 0 || eye >= VR_MAX_EYES || !view) {
		return;
	}

	*view = vr.eyeViews[eye];
}

/*
==================
VR_PrepareEye
==================
*/
void VR_PrepareEye(int eye)
{
	vrSwapchain_t                  *swapchain;
	XrSwapchainImageAcquireInfo     acquireInfo;
	XrSwapchainImageWaitInfo        waitInfo;

	if (!vr.frameStarted || eye < 0 || eye >= VR_MAX_EYES) {
		return;
	}

	swapchain = &vr.swapchains[eye];

	memset(&acquireInfo, 0, sizeof(acquireInfo));
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

	if (!XR_CHECK(xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, &swapchain->acquiredIndex))) {
		return;
	}

	memset(&waitInfo, 0, sizeof(waitInfo));
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = 100 * 1000 * 1000; // 100ms; never block the frame loop forever

	if (!XR_CHECK(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
		return;
	}

	swapchain->acquired = qtrue;
	vr.eyeStart = Sys_Milliseconds();

	// Tell the renderer before moving the target, not after.
	//
	// The renderer may be holding geometry it has not issued yet - under gl4es
	// it certainly is, since gl4es batches and issues lazily - and that work
	// belongs to whatever was bound when it was built. Giving the renderer the
	// chance to flush while the old target is still current is what keeps one
	// pass's leftovers out of the next.
	//
	// It does not do the binding. These framebuffers come from the driver's
	// glGenFramebuffers, and gl4es can only bind names it created itself, so
	// asking it to would raise GL_INVALID_VALUE and change nothing.
	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	}

	VR_BindFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	VR_Viewport(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
	VR_Scissor(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
	VR_GLDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// ...and its camera is this eye.
	if (re.SetVRView) {
		const vrEyeView_t *view = &vr.eyeViews[eye];

		re.SetVRView(view->origin, view->axis,
			view->tanLeft, view->tanRight, view->tanUp, view->tanDown,
			vr.baseYaw, eye);
	}
}

/*
==================
VR_SetBaseYaw
==================
*/
void VR_SetBaseYaw(float yaw)
{
	vr.baseYaw = yaw;
}

/*
==================
VR_TraceRenderTimes

The renderer's own accounting, added to the frame report.

Front end is the scene being built: culling, sorting, animation, all on the CPU.
Back end is that list being issued. Note that the back end number is still CPU
time - it is time spent inside GL calls - so a GPU that cannot keep up shows up
here too, as the driver blocking on a full queue rather than as work.
==================
*/
void VR_TraceRenderTimes(int frontEndMsec, int backEndMsec)
{
	vr.phaseFrontEnd += frontEndMsec;
	vr.phaseBackEnd += backEndMsec;
}

/*
==================
VR_TraceSceneTimes

Wall clock either side of the renderer's own accounting.

"scene" is the whole game frame - snapshots, prediction, entities, marks, temp
models, effects - of which the renderer's front end is only the last part. It is
also the part that runs once per eye, so anything expensive in it is being paid
for twice.
==================
*/
void VR_TraceSceneTimes(int sceneMsec, int issueMsec)
{
	vr.phaseScene += sceneMsec;
	vr.phaseIssue += issueMsec;
}

/*
==================
VR_TraceViewTimes
==================
*/
void VR_TraceViewTimes(int worldMsec, int hudMsec)
{
	vr.phaseWorld += worldMsec;
	vr.phaseHud += hudMsec;
}

/*
==================
VR_TraceHudTimes
==================
*/
void VR_TraceHudTimes(int cgameMsec)
{
	vr.phaseCgameHud += cgameMsec;
}

/*
==================
VR_TraceEvent
==================
*/
void VR_TraceEvent(int which)
{
	if (which >= 0 && which < VRTRACE_COUNT) {
		vr.traceEvents[which]++;
	}
}

/*
==================
VR_TraceState
==================
*/
void VR_TraceHudParts(int setup, int fades, int prints, int overlays, int tail)
{
	vr.hudSetup += setup;
	vr.hudFades += fades;
	vr.hudPrints += prints;
	vr.hudOverlays += overlays;
	vr.hudTail += tail;
}

/*
==================
VR_TraceState
==================
*/
void VR_TraceState(int drawMode, int hudPass, int noMenus)
{
	vr.traceDrawMode = drawMode;
	vr.traceHudPass = hudPass;
	vr.traceNoMenus = noMenus;
}

/*
==================
VR_FinishEye
==================
*/

/*
==================
VR_CaptureEye

Writes what the engine actually rendered into an eye, as a TGA next to the game
data, so it can be pulled off the device and looked at.

Everything before this had to be described out loud by whoever was wearing the
headset, and a description is a lossy channel for a rendering bug - "mangled" and
"white" each cost a round trip to pin down, and the readback that could not tell
black from nothing cost several. A picture settles in one look what pixel counts
only bound.

Deliberately reads the eye framebuffer rather than the swapchain image: this is
the engine's output before the compositor, before the blit, and before anything
the VR layer draws over it.

GL hands back rows bottom up and TGA with a zero origin bit wants them bottom up,
so the rows go straight out; only BGR ordering has to be undone.
==================
*/
static void VR_CaptureEye(int eye)
{
	static int    lastShot;
	static int    shotIndex;
	const GLsizei w = (GLsizei)vr.eyeWidth;
	const GLsizei h = (GLsizei)vr.eyeHeight;
	unsigned char header[18];
	unsigned char *rgba, *bgr;
	char           name[64];
	int            i, now;

	if (!vr_captureEye || !vr_captureEye->integer || eye != 0 || w <= 0 || h <= 0) {
		return;
	}

	now = Sys_Milliseconds();
	if (now - lastShot < 2000) {
		return;
	}
	lastShot = now;

	rgba = malloc((size_t)w * h * 4);
	bgr  = malloc((size_t)w * h * 3 + sizeof(header));
	if (!rgba || !bgr) {
		free(rgba);
		free(bgr);
		return;
	}

	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	memset(header, 0, sizeof(header));
	header[2]  = 2;                      // uncompressed true colour
	header[12] = w & 0xff;
	header[13] = (w >> 8) & 0xff;
	header[14] = h & 0xff;
	header[15] = (h >> 8) & 0xff;
	header[16] = 24;
	memcpy(bgr, header, sizeof(header));

	for (i = 0; i < w * h; i++) {
		bgr[sizeof(header) + i * 3 + 0] = rgba[i * 4 + 2];
		bgr[sizeof(header) + i * 3 + 1] = rgba[i * 4 + 1];
		bgr[sizeof(header) + i * 3 + 2] = rgba[i * 4 + 0];
	}

	Com_sprintf(name, sizeof(name), "vrshot%d.tga", shotIndex % 6);
	shotIndex++;
	FS_WriteFile(name, bgr, (int)(sizeof(header) + (size_t)w * h * 3));
	Com_Printf("VR capture: wrote %s (%dx%d)\n", name, (int)w, (int)h);

	free(rgba);
	free(bgr);
}

void VR_FinishEye(int eye)
{
	vrSwapchain_t                *swapchain;
	XrSwapchainImageReleaseInfo   releaseInfo;

	if (!vr.frameStarted || eye < 0 || eye >= VR_MAX_EYES) {
		return;
	}

	swapchain = &vr.swapchains[eye];

	if (!swapchain->acquired) {
		return;
	}

	// Before anything the VR layer draws over the top, so this is the engine's
	// picture and nothing else.
	VR_CaptureEye(eye);

	// Again before the direct calls below, and for the same reason as in
	// VR_PrepareEye: this is where the renderer flushes, and it has to do it
	// while this eye is still the bound target.
	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	}

	VR_BindFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	VR_GLDisable(GL_SCISSOR_TEST);

	// Hands and the pointer beam go in on top of the world, but only while the
	// wrist panel is up. A beam hanging off the hand at all times would be in
	// the way of a game that is mostly about looking at things and shooting
	// them; it is there to point at the panel, so it appears with the panel.
	if (vr.wristVisible && vr.beamReady && vr.beamVertexCount
		&& !(vr.vr_pointerBeam && !vr.vr_pointerBeam->integer)) {
		VR_Viewport(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
		VR_DrawBeam(eye, vr.beamVerts, vr.beamVertexCount);
	}

	// The compositor reads the alpha channel; the engine leaves it at whatever
	// the scene wrote, which shows up as a translucent image.
	VR_ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	VR_ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	// Diagnostic only, and expensive by design: waiting for the GPU to actually
	// finish is the only way to tell work from queueing. Everything measured so
	// far is wall clock, which charges whichever call happens to block.
	{
		const int gpuStart = Sys_Milliseconds();

		glFinish();
		vr.phaseGpu += Sys_Milliseconds() - gpuStart;
	}

	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(0);
	}

	VR_BindFramebuffer(0);

	// Back to the flat projection, so anything drawn outside an eye pass - the
	// screen layer, a loading screen - is not left with this eye's frustum.
	if (re.SetVRView) {
		re.SetVRView(NULL, NULL, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
	}

	memset(&releaseInfo, 0, sizeof(releaseInfo));
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

	swapchain->acquired = qfalse;
	vr.phaseEyes += Sys_Milliseconds() - vr.eyeStart;

	if (eye == VR_MAX_EYES - 1) {
		vr.layerReady = qtrue;
	}

	vr.projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
	vr.projectionViews[eye].next = NULL;
	vr.projectionViews[eye].pose = vr.views[eye].pose;
	vr.projectionViews[eye].fov = vr.views[eye].fov;
	vr.projectionViews[eye].subImage.swapchain = swapchain->handle;
	vr.projectionViews[eye].subImage.imageArrayIndex = 0;
	vr.projectionViews[eye].subImage.imageRect.offset.x = 0;
	vr.projectionViews[eye].subImage.imageRect.offset.y = 0;
	vr.projectionViews[eye].subImage.imageRect.extent.width = (int32_t)swapchain->width;
	vr.projectionViews[eye].subImage.imageRect.extent.height = (int32_t)swapchain->height;
}

/*
================================================================================

The pointer beam.

The menu pointer has always worked - the aim ray is intersected with the panel
and drives the engine's own cursor - but there was nothing to see, because a
screen layer frame submits a quad and no projection layer, and a quad has no
depth for a beam to travel through. So the eye buffers are drawn after all,
carrying nothing but the beam, and submitted underneath the panel. The beam is
therefore visible along its length up to the panel's edge, which is exactly the
part worth seeing; the panel itself shows the engine's cursor.

Its own GL program rather than the renderer's: this happens outside the
renderer's frame, it is two dozen triangles, and going through the shader system
would mean the renderer knowing about a mode it has no other reason to have.

================================================================================
*/

static const char *beamVertexShader =
	"#version 300 es\n"
	"layout(location = 0) in vec3 aPosition;\n"
	"uniform mat4 uMvp;\n"
	"void main() {\n"
	"	gl_Position = uMvp * vec4(aPosition, 1.0);\n"
	"}\n";

static const char *beamFragmentShader =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 uColor;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"	fragColor = uColor;\n"
	"}\n";

/*
The wrist panel's alpha.

The compositor needs to know which pixels of the panel are the panel and which
are the room behind it, and it reads that out of the alpha channel. The engine's
2D path does not leave anything useful there - it was written for a screen, where
there is nothing behind the screen and alpha never mattered. Forcing it opaque
gives a black slab with a compass on it; trusting it gives a panel where the
images survive and the text disappears, because the two go through different
draw paths that disagree about what to put in a channel neither was using.

So the alpha is worked out from what is actually on the panel instead. The HUD
draws bright marks on a background that was cleared to black, so brightness is a
good enough stand-in for coverage, and it does not care which path drew what.
*/

static const char *panelVertexShader =
	"#version 300 es\n"
	"out vec2 vTexCoord;\n"
	"void main() {\n"
	// One oversized triangle rather than two, from the vertex index alone, so
	// there is no buffer to feed and no seam down the diagonal.
	"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"	vTexCoord = p;\n"
	"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *panelFragmentShader =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D uTexture;\n"
	// xy is the corner, zw the size, in texture coordinates.
	"uniform vec4 uCrop;\n"
	"in vec2 vTexCoord;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"	vec4 c = texture(uTexture, uCrop.xy + vTexCoord * uCrop.zw);\n"
	"	float a = max(max(c.r, c.g), c.b);\n"
	// Lifted, so a thin antialiased stroke does not come out as a ghost, and
	// clamped so the bright middle of a glyph is fully solid.
	"	fragColor = vec4(c.rgb, clamp(a * 2.2, 0.0, 1.0));\n"
	"}\n";

/*
==================
VR_CompileShader
==================
*/
static GLuint VR_CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint  compiled = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

	if (!compiled) {
		char log[1024];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		Com_Printf("OpenXR: pointer beam shader failed to compile: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

/*
==================
VR_CreateBeam
==================
*/
static void VR_CreateBeam(void)
{
	GLuint vertexShader, fragmentShader;
	GLint  linked = 0;

	VR_DestroyBeam();

	vertexShader = VR_CompileShader(GL_VERTEX_SHADER, beamVertexShader);
	fragmentShader = VR_CompileShader(GL_FRAGMENT_SHADER, beamFragmentShader);

	if (!vertexShader || !fragmentShader) {
		if (vertexShader) {
			glDeleteShader(vertexShader);
		}
		if (fragmentShader) {
			glDeleteShader(fragmentShader);
		}
		return;
	}

	vr.beamProgram = glCreateProgram();
	glAttachShader(vr.beamProgram, vertexShader);
	glAttachShader(vr.beamProgram, fragmentShader);
	glLinkProgram(vr.beamProgram);
	glGetProgramiv(vr.beamProgram, GL_LINK_STATUS, &linked);

	// Attached shaders are reference counted by the program, so they can go now.
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	if (!linked) {
		char log[1024];

		glGetProgramInfoLog(vr.beamProgram, sizeof(log), NULL, log);
		Com_Printf("OpenXR: pointer beam program failed to link: %s\n", log);
		glDeleteProgram(vr.beamProgram);
		vr.beamProgram = 0;
		return;
	}

	vr.beamMvpLocation = glGetUniformLocation(vr.beamProgram, "uMvp");
	vr.beamColorLocation = glGetUniformLocation(vr.beamProgram, "uColor");

	// A vertex array of its own, so none of the attribute state the renderer
	// has set up is disturbed by binding a buffer here.
	glGenVertexArrays(1, &vr.beamVertexArray);
	glGenBuffers(1, &vr.beamVertexBuffer);

	vr.beamReady = qtrue;

	// The panel resolve, same lifetime and same tiny shape.
	vertexShader = VR_CompileShader(GL_VERTEX_SHADER, panelVertexShader);
	fragmentShader = VR_CompileShader(GL_FRAGMENT_SHADER, panelFragmentShader);

	if (!vertexShader || !fragmentShader) {
		if (vertexShader) {
			glDeleteShader(vertexShader);
		}
		if (fragmentShader) {
			glDeleteShader(fragmentShader);
		}
		return;
	}

	vr.panelProgram = glCreateProgram();
	glAttachShader(vr.panelProgram, vertexShader);
	glAttachShader(vr.panelProgram, fragmentShader);
	glLinkProgram(vr.panelProgram);
	glGetProgramiv(vr.panelProgram, GL_LINK_STATUS, &linked);

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	if (!linked) {
		char log[1024];

		glGetProgramInfoLog(vr.panelProgram, sizeof(log), NULL, log);
		Com_Printf("OpenXR: panel resolve program failed to link: %s\n", log);
		glDeleteProgram(vr.panelProgram);
		vr.panelProgram = 0;
		return;
	}

	vr.panelTextureLocation = glGetUniformLocation(vr.panelProgram, "uTexture");
	vr.panelCropLocation = glGetUniformLocation(vr.panelProgram, "uCrop");

	// Empty, but a vertex array still has to be bound to draw at all.
	glGenVertexArrays(1, &vr.panelVertexArray);

	vr.panelReady = qtrue;
}

/*
==================
VR_DestroyBeam
==================
*/
static void VR_DestroyBeam(void)
{
	if (vr.panelVertexArray) {
		glDeleteVertexArrays(1, &vr.panelVertexArray);
		vr.panelVertexArray = 0;
	}

	if (vr.panelProgram) {
		glDeleteProgram(vr.panelProgram);
		vr.panelProgram = 0;
	}

	vr.panelReady = qfalse;

	if (vr.beamVertexBuffer) {
		glDeleteBuffers(1, &vr.beamVertexBuffer);
		vr.beamVertexBuffer = 0;
	}

	if (vr.beamVertexArray) {
		glDeleteVertexArrays(1, &vr.beamVertexArray);
		vr.beamVertexArray = 0;
	}

	if (vr.beamProgram) {
		glDeleteProgram(vr.beamProgram);
		vr.beamProgram = 0;
	}

	vr.beamReady = qfalse;
}

/*
==================
VR_ProjectionMatrix

Straight from the runtime's frustum tangents. Asymmetric, like the eye it came
from - see R_SetupProjection for why that matters.
==================
*/
static void VR_ProjectionMatrix(const XrFovf *fov, float zNear, float zFar, float *m)
{
	const float tanLeft   = tanf(fov->angleLeft);
	const float tanRight  = tanf(fov->angleRight);
	const float tanDown   = tanf(fov->angleDown);
	const float tanUp     = tanf(fov->angleUp);
	const float tanWidth  = tanRight - tanLeft;
	const float tanHeight = tanUp - tanDown;

	memset(m, 0, 16 * sizeof(float));

	m[0]  = 2.0f / tanWidth;
	m[5]  = 2.0f / tanHeight;
	m[8]  = (tanRight + tanLeft) / tanWidth;
	m[9]  = (tanUp + tanDown) / tanHeight;
	m[10] = -(zFar + zNear) / (zFar - zNear);
	m[11] = -1.0f;
	m[14] = -(2.0f * zFar * zNear) / (zFar - zNear);
}

/*
==================
VR_ViewMatrix

The inverse of an eye pose. Everything the beam is built from is already in
stage space, so this is the only transform between the two.
==================
*/
static void VR_ViewMatrix(const XrPosef *pose, float *m)
{
	const XrQuaternionf *q = &pose->orientation;
	float                r[3][3];
	int                  i;

	r[0][0] = 1.0f - 2.0f * (q->y * q->y + q->z * q->z);
	r[0][1] =        2.0f * (q->x * q->y - q->z * q->w);
	r[0][2] =        2.0f * (q->x * q->z + q->y * q->w);
	r[1][0] =        2.0f * (q->x * q->y + q->z * q->w);
	r[1][1] = 1.0f - 2.0f * (q->x * q->x + q->z * q->z);
	r[1][2] =        2.0f * (q->y * q->z - q->x * q->w);
	r[2][0] =        2.0f * (q->x * q->z - q->y * q->w);
	r[2][1] =        2.0f * (q->y * q->z + q->x * q->w);
	r[2][2] = 1.0f - 2.0f * (q->x * q->x + q->y * q->y);

	memset(m, 0, 16 * sizeof(float));

	// Column major, and the rotation transposed because this is the inverse.
	for (i = 0; i < 3; i++) {
		m[i * 4 + 0] = r[i][0];
		m[i * 4 + 1] = r[i][1];
		m[i * 4 + 2] = r[i][2];
	}

	m[12] = -(r[0][0] * pose->position.x + r[1][0] * pose->position.y + r[2][0] * pose->position.z);
	m[13] = -(r[0][1] * pose->position.x + r[1][1] * pose->position.y + r[2][1] * pose->position.z);
	m[14] = -(r[0][2] * pose->position.x + r[1][2] * pose->position.y + r[2][2] * pose->position.z);
	m[15] = 1.0f;
}

/*
==================
VR_MatrixMultiply
==================
*/
static void VR_MatrixMultiply(const float *a, const float *b, float *out)
{
	int column, row, i;

	for (column = 0; column < 4; column++) {
		for (row = 0; row < 4; row++) {
			float sum = 0.0f;

			for (i = 0; i < 4; i++) {
				sum += a[i * 4 + row] * b[column * 4 + i];
			}

			out[column * 4 + row] = sum;
		}
	}
}

/*
==================
VR_AppendBox

A box rather than a billboarded strip: it is a handful more triangles and it
looks the same from every angle, which a strip only does if it is rebuilt per
eye.
==================
*/
static float *VR_AppendBox(float *out, const XrVector3f *centre,
	const XrVector3f *x, const XrVector3f *y, const XrVector3f *z)
{
	static const int faces[6][4] = {
		{ 0, 1, 3, 2 },	// -z
		{ 4, 6, 7, 5 },	// +z
		{ 0, 4, 5, 1 },	// -y
		{ 2, 3, 7, 6 },	// +y
		{ 0, 2, 6, 4 },	// -x
		{ 1, 5, 7, 3 },	// +x
	};
	XrVector3f corner[8];
	int        i, f;

	for (i = 0; i < 8; i++) {
		const float sx = (i & 4) ? 1.0f : -1.0f;
		const float sy = (i & 2) ? 1.0f : -1.0f;
		const float sz = (i & 1) ? 1.0f : -1.0f;

		corner[i].x = centre->x + x->x * sx + y->x * sy + z->x * sz;
		corner[i].y = centre->y + x->y * sx + y->y * sy + z->y * sz;
		corner[i].z = centre->z + x->z * sx + y->z * sy + z->z * sz;
	}

	for (f = 0; f < 6; f++) {
		static const int order[6] = { 0, 1, 2, 0, 2, 3 };

		for (i = 0; i < 6; i++) {
			const XrVector3f *v = &corner[faces[f][order[i]]];

			*out++ = v->x;
			*out++ = v->y;
			*out++ = v->z;
		}
	}

	return out;
}

/*
==================
VR_BuildBeamGeometry

Stage space, so it lines up with the eye poses without going through the
engine's axes at all. Returns the vertex count.
==================
*/
static int VR_BuildBeamGeometry(float *verts, int maxVerts)
{
	static const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	static const XrVector3f localRight   = { 1.0f, 0.0f, 0.0f };
	static const XrVector3f localUp      = { 0.0f, 1.0f, 0.0f };
	const float             thickness    = 0.0035f;
	const float             handSize     = 0.014f;
	float                  *out = verts;
	int                     hand;

	for (hand = 0; hand < 2; hand++) {
		XrVector3f forward, right, up, centre, ax, ay, az;
		float      length;
		qboolean   wearingPanel;

		if (!vr.handPoseValid[hand]) {
			continue;
		}

		if ((out - verts) / 3 + 72 > maxVerts) {
			break;
		}

		VR_RotateVector(&vr.handPoses[hand].orientation, &localForward, &forward);
		VR_RotateVector(&vr.handPoses[hand].orientation, &localRight, &right);
		VR_RotateVector(&vr.handPoses[hand].orientation, &localUp, &up);

		// The hand the panel is strapped to gets a marker and no beam. It is not
		// pointing at anything - it is the thing being pointed at.
		wearingPanel = (vr.wristVisible && hand == 0) ? qtrue : qfalse;

		if (!wearingPanel) {
			// The hand actually on the panel stops at it; the other gets a stub,
			// so it reads as a pointer without a beam running off into the room.
			if (hand == vr.pointerHand && vr.pointerDistance > 0.0f) {
				length = vr.pointerDistance;
			} else {
				length = 0.35f;
			}

			// The beam, from the hand along its aim.
			centre.x = vr.handPoses[hand].position.x + forward.x * length * 0.5f;
			centre.y = vr.handPoses[hand].position.y + forward.y * length * 0.5f;
			centre.z = vr.handPoses[hand].position.z + forward.z * length * 0.5f;

			ax.x = right.x * thickness;   ax.y = right.y * thickness;   ax.z = right.z * thickness;
			ay.x = up.x * thickness;      ay.y = up.y * thickness;      ay.z = up.z * thickness;
			az.x = forward.x * length * 0.5f;
			az.y = forward.y * length * 0.5f;
			az.z = forward.z * length * 0.5f;

			out = VR_AppendBox(out, &centre, &ax, &ay, &az);
		}

		// A marker where the hand is, so the beam reads as coming from
		// something rather than starting in mid air.
		centre = vr.handPoses[hand].position;

		ax.x = right.x * handSize;    ax.y = right.y * handSize;    ax.z = right.z * handSize;
		ay.x = up.x * handSize;       ay.y = up.y * handSize;       ay.z = up.z * handSize;
		az.x = forward.x * handSize;  az.y = forward.y * handSize;  az.z = forward.z * handSize;

		out = VR_AppendBox(out, &centre, &ax, &ay, &az);
	}

	return (int)(out - verts) / 3;
}

/*
==================
VR_DrawBeam

State is saved and put back rather than assumed: this runs between the
renderer's frames, and renderergl2 caches what it believes is bound. Leaving a
different program or vertex array behind makes the next frame draw nothing, in a
way that looks like a renderer bug rather than an overlay one.
==================
*/
static void VR_DrawBeam(int eye, const float *verts, int vertexCount)
{
	float proj[16], view[16], mvp[16];
	GLint prevProgram = 0, prevVertexArray = 0, prevArrayBuffer = 0;
	GLint prevBlendSrc = GL_ONE, prevBlendDst = GL_ZERO;
	GLboolean prevDepthMask = GL_TRUE;
	GLboolean wasDepthTest, wasBlend, wasCull;

	glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrc);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDst);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
	wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
	wasBlend = glIsEnabled(GL_BLEND);
	wasCull = glIsEnabled(GL_CULL_FACE);

	VR_ProjectionMatrix(&vr.views[eye].fov, 0.02f, 50.0f, proj);
	VR_ViewMatrix(&vr.views[eye].pose, view);
	VR_MatrixMultiply(proj, view, mvp);

	glUseProgram(vr.beamProgram);
	glUniformMatrix4fv(vr.beamMvpLocation, 1, GL_FALSE, mvp);
	glUniform4f(vr.beamColorLocation, 0.45f, 0.72f, 1.0f, 0.85f);

	glBindVertexArray(vr.beamVertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, vr.beamVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, vertexCount * 3 * sizeof(float), verts, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);

	VR_GLEnable(GL_DEPTH_TEST);
	VR_DepthMask(GL_FALSE);
	VR_GLDisable(GL_CULL_FACE);
	VR_GLEnable(GL_BLEND);
	VR_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glDrawArrays(GL_TRIANGLES, 0, vertexCount);

	glBindVertexArray((GLuint)prevVertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuffer);
	glUseProgram((GLuint)prevProgram);
	VR_DepthMask(prevDepthMask);
	VR_BlendFunc(prevBlendSrc, prevBlendDst);

	if (!wasDepthTest) {
		VR_GLDisable(GL_DEPTH_TEST);
	}
	if (!wasBlend) {
		VR_GLDisable(GL_BLEND);
	}
	if (wasCull) {
		VR_GLEnable(GL_CULL_FACE);
	}
}

/*
==================
VR_RenderPointerLayer

Draws the beam into both eye images and leaves the projection views set up, so
the panel can be composited over a real 3D layer instead of replacing it.
==================
*/
static void VR_RenderPointerLayer(void)
{
	int eye;

	vr.pointerLayerReady = qfalse;

	if (!vr.beamReady || !vr.viewsValid || !vr.frameState.shouldRender) {
		return;
	}

	if (vr.vr_pointerBeam && !vr.vr_pointerBeam->integer) {
		return;
	}

	if (!vr.beamVertexCount) {
		return;
	}

	for (eye = 0; eye < VR_MAX_EYES; eye++) {
		vrSwapchain_t               *swapchain = &vr.swapchains[eye];
		XrSwapchainImageAcquireInfo  acquireInfo;
		XrSwapchainImageWaitInfo     waitInfo;
		XrSwapchainImageReleaseInfo  releaseInfo;

		memset(&acquireInfo, 0, sizeof(acquireInfo));
		acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

		if (!XR_CHECK(xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, &swapchain->acquiredIndex))) {
			return;
		}

		memset(&releaseInfo, 0, sizeof(releaseInfo));
		releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

		memset(&waitInfo, 0, sizeof(waitInfo));
		waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		waitInfo.timeout = 100 * 1000 * 1000;

		// Released even when the wait fails: the image is acquired either way,
		// and holding one back starves the swapchain within a few frames.
		if (!XR_CHECK(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
			XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));
			return;
		}

		VR_BindFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
		VR_Viewport(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
		VR_GLDisable(GL_SCISSOR_TEST);
		// Cleared to nothing, so this layer is the beam and nothing else. It
		// goes over the panel rather than under it: a pointer you cannot see
		// touch the thing it is pointing at is not much of a pointer, and the
		// menu is the one place the beam actually has a job.
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		VR_DepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		VR_DrawBeam(eye, vr.beamVerts, vr.beamVertexCount);

		VR_BindFramebuffer(0);

		XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

		vr.projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		vr.projectionViews[eye].next = NULL;
		vr.projectionViews[eye].pose = vr.views[eye].pose;
		vr.projectionViews[eye].fov = vr.views[eye].fov;
		vr.projectionViews[eye].subImage.swapchain = swapchain->handle;
		vr.projectionViews[eye].subImage.imageArrayIndex = 0;
		vr.projectionViews[eye].subImage.imageRect.offset.x = 0;
		vr.projectionViews[eye].subImage.imageRect.offset.y = 0;
		vr.projectionViews[eye].subImage.imageRect.extent.width = (int32_t)swapchain->width;
		vr.projectionViews[eye].subImage.imageRect.extent.height = (int32_t)swapchain->height;
	}

	vr.pointerLayerReady = qtrue;
}

/*
==================
VR_UseScreenLayer

Menus, the loading screen and cinematics are all screen space content with no
world behind them.
==================
*/
qboolean VR_UseScreenLayer(void)
{
	return (clc.state != CA_ACTIVE) ? qtrue : qfalse;
}

/*
==================
VR_PlaceScreenAnchor

Pins the panel in world space, level and squarely in front of wherever the
viewer is facing at the moment flat content appears. Only the heading is
taken from the head: carrying pitch and roll across would leave the panel
tilted to match however the viewer happened to be looking.
==================
*/
static void VR_PlaceScreenAnchor(void)
{
	const float distance = vr.vr_screenDistance->value > 0.0f ? vr.vr_screenDistance->value : 2.5f;
	const XrQuaternionf *q;
	XrVector3f  head;
	float       yaw;

	// Taken from the eye views rather than by locating the view space again.
	// Those were located this frame as part of setting up the projection, so
	// they are known good; a second lookup can fail on its own, and when it did
	// the anchor was left at the origin of the play space - which put the panel
	// on the floor in the middle of the room, and made it jump back and forth
	// whenever the lookup succeeded only intermittently.
	if (!vr.viewsValid) {
		return;
	}

	head.x = (vr.views[0].pose.position.x + vr.views[1].pose.position.x) * 0.5f;
	head.y = (vr.views[0].pose.position.y + vr.views[1].pose.position.y) * 0.5f;
	head.z = (vr.views[0].pose.position.z + vr.views[1].pose.position.z) * 0.5f;

	q = &vr.views[0].pose.orientation;
	yaw = atan2f(2.0f * (q->w * q->y + q->x * q->z),
	             1.0f - 2.0f * (q->y * q->y + q->z * q->z));

	// OpenXR looks down -Z, so forward for a given heading is (-sin, 0, -cos).
	vr.screenAnchor.position.x = head.x - sinf(yaw) * distance;
	vr.screenAnchor.position.y = head.y;
	vr.screenAnchor.position.z = head.z - cosf(yaw) * distance;

	vr.screenAnchor.orientation.x = 0.0f;
	vr.screenAnchor.orientation.y = sinf(yaw * 0.5f);
	vr.screenAnchor.orientation.z = 0.0f;
	vr.screenAnchor.orientation.w = cosf(yaw * 0.5f);

	vr.screenAnchorValid = qtrue;
}

/*
==================
VR_PrepareScreenLayer
==================
*/
void VR_PrepareScreenLayer(void)
{
	if (!vr.frameStarted) {
		return;
	}

	// Placed the first time flat content appears, and kept until it goes away
	// again, so the panel holds still while the head moves around it.
	if (!vr.screenAnchorValid) {
		VR_PlaceScreenAnchor();
	}

	// Renderer first, so it flushes what it is still holding into the target
	// that work was drawn for; see VR_PrepareEye. It does not bind - the call
	// below does.
	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(vr.uiFramebuffer);
	}

	// Deliberately not cleared: the engine's 2D path treats the screen as
	// something that persists between frames, and this buffer is the only
	// place that is true.
	VR_BindFramebuffer(vr.uiFramebuffer);
	VR_Viewport(0, 0, (GLsizei)vr.uiSwapchain.width, (GLsizei)vr.uiSwapchain.height);

	// Diagnostic, and the reason the menu is still unexplained.
	//
	// Because the panel is never cleared, "the UI drew nothing" and "the UI drew
	// black over what was already there" leave an identical buffer, and the
	// readback in VR_FinishScreenLayer - which counted samples brighter than 8 -
	// could not tell them apart. It reported 0/256 and that was written down as
	// "nothing is written at all". It is not evidence for that. The frame before
	// the menu is the end of a cinematic, which fades to black, so an untouched
	// panel is black too.
	//
	// Starting from a colour nothing in this game draws makes the two cases
	// different, both in the log and in the headset: magenta means the UI never
	// arrived, black means it arrived and painted black.
	if (vr_debugPanel && vr_debugPanel->integer) {
		VR_GLDisable(GL_SCISSOR_TEST);
		glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}
}

/*
==================
VR_FinishScreenLayer
==================
*/
void VR_FinishScreenLayer(void)
{
	vrSwapchain_t               *swapchain = &vr.uiSwapchain;
	XrSwapchainImageAcquireInfo  acquireInfo;
	XrSwapchainImageWaitInfo     waitInfo;
	XrSwapchainImageReleaseInfo  releaseInfo;

	if (!vr.frameStarted) {
		return;
	}

	// Diagnostic. Reads the panel back on a 16x16 grid and sorts each sample
	// into the three answers the magenta clear in VR_PrepareScreenLayer makes
	// distinguishable:
	//
	//   untouched - the UI never reached this buffer at all
	//   black     - it reached it and drew black
	//   drawn     - it reached it and drew something
	//
	// The previous version of this counted only "brighter than 8" against a
	// buffer that is never cleared, so the first two answers were the same
	// number and the menu looked like it was drawing nothing when the evidence
	// did not say that.
	if (vr_debugPanel && vr_debugPanel->integer) {
		static int panelReport;
		int panelNow = Sys_Milliseconds();

		if (panelNow - panelReport > 1000) {
			unsigned char px[4];
			int gx, gy;
			int untouched = 0, black = 0, drawn = 0;
			const int bw = (int)vr.uiSwapchain.width;
			const int bh = (int)vr.uiSwapchain.height;

			panelReport = panelNow;

			// Through gl4es, which sets the driver's read and draw bindings
			// both; going straight at the driver here would leave gl4es holding
			// a target it no longer has.
			VR_BindFramebuffer(vr.uiFramebuffer);

			for (gy = 0; gy < 16; gy++) {
				for (gx = 0; gx < 16; gx++) {
					glReadPixels(gx * (bw / 16) + bw / 32, gy * (bh / 16) + bh / 32,
						1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

					if (px[0] > 240 && px[1] < 16 && px[2] > 240) {
						untouched++;
					} else if (px[0] <= 8 && px[1] <= 8 && px[2] <= 8) {
						black++;
					} else {
						drawn++;
					}
				}
			}

			Com_Printf("VR panel: %d untouched, %d black, %d drawn (of 256)\n",
				untouched, black, drawn);
		}
	}



	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(0);
	}

	memset(&acquireInfo, 0, sizeof(acquireInfo));
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

	if (!XR_CHECK(xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, &swapchain->acquiredIndex))) {
		return;
	}

	memset(&waitInfo, 0, sizeof(waitInfo));
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = 100 * 1000 * 1000;

	if (!XR_CHECK(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
		return;
	}

	VR_GLDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, vr.uiFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, swapchain->frameBuffers[swapchain->acquiredIndex]);
	glBlitFramebuffer(0, 0, (GLint)swapchain->width, (GLint)swapchain->height,
		0, 0, (GLint)swapchain->width, (GLint)swapchain->height,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// The compositor reads alpha; the engine leaves whatever the scene wrote.
	VR_BindFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	VR_ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	VR_ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	VR_BindFramebuffer(0);

	memset(&releaseInfo, 0, sizeof(releaseInfo));
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

	swapchain->acquired = qfalse;
	vr.screenLayerActive = qtrue;
	vr.layerReady = qtrue;

	// After the panel, because it goes underneath it.
	VR_RenderPointerLayer();
}

/*
==================
VR_ResolveWristPanel

Copies the panel into the swapchain image, working out the alpha as it goes.
==================
*/
static void VR_ResolveWristPanel(GLuint target)
{
	GLint     prevProgram = 0, prevVertexArray = 0, prevTexture = 0, prevActive = GL_TEXTURE0;
	GLboolean wasDepthTest, wasBlend, wasCull;

	if (!vr.panelReady) {
		return;
	}

	glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
	wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
	wasBlend = glIsEnabled(GL_BLEND);
	wasCull = glIsEnabled(GL_CULL_FACE);

	VR_BindFramebuffer(target);
	VR_Viewport(0, 0, (GLsizei)vr.uiSwapchain.width, (GLsizei)vr.uiSwapchain.height);

	VR_GLDisable(GL_DEPTH_TEST);
	VR_GLDisable(GL_BLEND);
	VR_GLDisable(GL_CULL_FACE);

	glUseProgram(vr.panelProgram);
	glBindTexture(GL_TEXTURE_2D, vr.uiTexture);
	glUniform1i(vr.panelTextureLocation, 0);

	{
		float cropX, cropY, cropWidth, cropHeight;

		VR_WristCrop(&cropX, &cropY, &cropWidth, &cropHeight);

		// The crop is given from the top, the way the engine's screen is
		// measured; texture coordinates run the other way, because the 2D ortho
		// puts the top of the screen at the top of the framebuffer.
		glUniform4f(vr.panelCropLocation, cropX, 1.0f - (cropY + cropHeight),
			cropWidth, cropHeight);
	}

	glBindVertexArray(vr.panelVertexArray);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glBindVertexArray((GLuint)prevVertexArray);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexture);
	glActiveTexture((GLenum)prevActive);
	glUseProgram((GLuint)prevProgram);

	if (wasDepthTest) {
		VR_GLEnable(GL_DEPTH_TEST);
	}
	if (wasBlend) {
		VR_GLEnable(GL_BLEND);
	}
	if (wasCull) {
		VR_GLEnable(GL_CULL_FACE);
	}
}

/*
==================
VR_PrepareWristPanel

The HUD and any open menu, drawn once into the same buffer the room sized panel
uses. The two are never up together - one is what the player sees instead of the
world, the other is what they see over it - so they can share.

Cleared, unlike the screen layer: that one is deliberately left alone because
the engine's 2D path treats the screen as something that persists between
frames, but a HUD that accumulated would smear.
==================
*/
void VR_PrepareWristPanel(void)
{
	if (!vr.frameStarted) {
		return;
	}

	// Renderer first, so it flushes what it is still holding into the target
	// that work was drawn for; see VR_PrepareEye. It does not bind - the call
	// below does.
	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(vr.uiFramebuffer);
	}

	VR_BindFramebuffer(vr.uiFramebuffer);
	VR_Viewport(0, 0, (GLsizei)vr.uiSwapchain.width, (GLsizei)vr.uiSwapchain.height);
	VR_GLDisable(GL_SCISSOR_TEST);

	// Cleared to nothing at all, not to black. The panel is a strip of readouts
	// worn on the arm, not a screen: what the HUD does not draw on should be the
	// room behind it. The alpha the 2D path leaves behind is what the compositor
	// uses to decide that, so the clear has to start it at zero.
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

/*
==================
VR_FinishWristPanel
==================
*/
void VR_FinishWristPanel(void)
{
	vrSwapchain_t               *swapchain = &vr.uiSwapchain;
	XrSwapchainImageAcquireInfo  acquireInfo;
	XrSwapchainImageWaitInfo     waitInfo;
	XrSwapchainImageReleaseInfo  releaseInfo;

	vr.wristLayerReady = qfalse;

	if (!vr.frameStarted) {
		return;
	}

	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(0);
	}

	memset(&acquireInfo, 0, sizeof(acquireInfo));
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

	if (!XR_CHECK(xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, &swapchain->acquiredIndex))) {
		return;
	}

	memset(&releaseInfo, 0, sizeof(releaseInfo));
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

	memset(&waitInfo, 0, sizeof(waitInfo));
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = 100 * 1000 * 1000;

	if (!XR_CHECK(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
		XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));
		return;
	}

	VR_GLDisable(GL_SCISSOR_TEST);

	// Through the resolve rather than a straight blit, because the alpha has to
	// be worked out on the way across - see panelFragmentShader. A blit would
	// carry over whatever the 2D path happened to leave in that channel, which
	// is what made the text vanish while the compass survived.
	VR_ResolveWristPanel(swapchain->frameBuffers[swapchain->acquiredIndex]);

	VR_BindFramebuffer(0);

	XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

	swapchain->acquired = qfalse;
	vr.wristLayerReady = qtrue;
}

/*
==================
VR_TraceFrameTiming

What the frame actually costs, once a second, to the log - there being no
console in a headset to put a counter on.

The display period comes from the runtime rather than from what was asked for,
so this reports the rate the compositor is really running at whether or not the
refresh rate request landed. Frames below that rate are frames the compositor
had to reproject, which is the number that decides how ambitious anything else
can be.
==================
*/
static void VR_TraceFrameTiming(void)
{
	static int lastReport;
	static int lastFrame;
	static int frames;
	static int worst;
	static int over;
	int        now = Sys_Milliseconds();
	int        elapsed;
	float      target;

	target = (vr.frameState.predictedDisplayPeriod > 0)
		? 1000000000.0f / (float)vr.frameState.predictedDisplayPeriod
		: 0.0f;

	if (lastFrame) {
		const int frameTime = now - lastFrame;

		if (frameTime > worst) {
			worst = frameTime;
		}

		// A frame that took longer than the display period is one the
		// compositor had to make something up for.
		if (target > 0.0f && frameTime > (int)(1000.0f / target)) {
			over++;
		}
	}

	lastFrame = now;
	frames++;

	elapsed = now - lastReport;

	if (elapsed < 1000) {
		return;
	}

	Com_Printf("VR frame: %.1f fps, display %.0fHz, worst %dms, %d/%d late"
		" | eyes %dms = world %dms + hud %dms (cgame %dms) + rest %dms\n",
		frames * 1000.0f / (float)elapsed, target, worst, over, frames,
		vr.phaseEyes / frames, vr.phaseWorld / frames,
		vr.phaseHud / frames, vr.phaseCgameHud / frames,
		(vr.phaseEyes - vr.phaseWorld - vr.phaseHud) / frames);

	Com_Printf("VR paths: draw2d %d cgame2d %d updateviews %d view3d %d"
		" | mode %d hudPass %d noMenus %d | res %ux%u | gpuwait %dms\n",
		vr.traceEvents[VRTRACE_DRAW2D], vr.traceEvents[VRTRACE_CGAME_HUD],
		vr.traceEvents[VRTRACE_UPDATEVIEWS], vr.traceEvents[VRTRACE_VIEW3D],
		vr.traceDrawMode, vr.traceHudPass, vr.traceNoMenus,
		vr.eyeWidth, vr.eyeHeight, vr.phaseGpu / frames);

	Com_Printf("VR hud: setup %dms fades %dms cgame %dms prints %dms overlays %dms tail %dms\n",
		vr.hudSetup / frames, vr.hudFades / frames, vr.phaseCgameHud / frames,
		vr.hudPrints / frames, vr.hudOverlays / frames, vr.hudTail / frames);

	vr.hudSetup = 0;
	vr.hudFades = 0;
	vr.hudPrints = 0;
	vr.hudOverlays = 0;
	vr.hudTail = 0;

	memset(vr.traceEvents, 0, sizeof(vr.traceEvents));
	vr.phaseGpu = 0;

	lastReport = now;
	frames = 0;
	worst = 0;
	over = 0;
	vr.phaseEyes = 0;
	vr.phaseWait = 0;
	vr.phaseSubmit = 0;
	vr.phaseFrontEnd = 0;
	vr.phaseBackEnd = 0;
	vr.phaseScene = 0;
	vr.phaseIssue = 0;
	vr.phaseWorld = 0;
	vr.phaseHud = 0;
	vr.phaseCgameHud = 0;
}

/*
==================
VR_SubmitFrame
==================
*/
void VR_SubmitFrame(void)
{
	const int                           submitStart = Sys_Milliseconds();
	XrCompositionLayerProjection        projection;
	XrCompositionLayerQuad              quad;
	const XrCompositionLayerBaseHeader  *layers[2];
	XrFrameEndInfo                      endInfo;
	int                                 layerCount = 0;

	if (!vr.frameStarted) {
		return;
	}

	memset(&projection, 0, sizeof(projection));
	projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection.layerFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
	projection.space = vr.stageSpace;
	projection.viewCount = VR_MAX_EYES;
	projection.views = vr.projectionViews;

	if (vr.screenLayerActive && vr.screenAnchorValid) {
		const float size = vr.vr_screenSize->value > 0.0f ? vr.vr_screenSize->value : 3.0f;

		memset(&quad, 0, sizeof(quad));
		quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quad.layerFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
		// World space, not view space: the panel stays where it was placed so
		// the viewer can look around it, rather than being dragged along by
		// every head movement.
		quad.space = vr.stageSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = vr.uiSwapchain.handle;
		quad.subImage.imageArrayIndex = 0;
		quad.subImage.imageRect.offset.x = 0;
		quad.subImage.imageRect.offset.y = 0;
		quad.subImage.imageRect.extent.width = (int32_t)vr.uiSwapchain.width;
		quad.subImage.imageRect.extent.height = (int32_t)vr.uiSwapchain.height;
		quad.pose = vr.screenAnchor;
		quad.size.width = size;
		quad.size.height = size * (float)vr.uiSwapchain.height / (float)vr.uiSwapchain.width;

		layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&quad;

		// Layers composite in the order they go in, so the beam goes last and
		// lands on top of the menu it is pointing at. It only carries the beam;
		// everywhere else it is transparent, which is what lets it sit over the
		// panel without hiding it.
		if (vr.pointerLayerReady) {
			projection.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
				| XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
			layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projection;
		}
	} else {
		layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projection;

		// The world first, then the wrist panel over it. Same ordering rule as
		// the menu: the layer submitted later is the one on top.
		if (vr.wristLayerReady) {
			const float size = vr.vr_wristSize->value > 0.0f ? vr.vr_wristSize->value : 0.32f;

			memset(&quad, 0, sizeof(quad));
			quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			// Blended against the world rather than laid over it, using the
			// alpha the HUD wrote. Unpremultiplied because that is what ordinary
			// alpha blending leaves in the buffer - claiming otherwise darkens
			// every edge.
			quad.layerFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT
				| XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
				| XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
			quad.space = vr.stageSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = vr.uiSwapchain.handle;
			quad.subImage.imageArrayIndex = 0;
			{
				float cropX, cropY, cropWidth, cropHeight;

				VR_WristCrop(&cropX, &cropY, &cropWidth, &cropHeight);

				// The whole image: the crop happened on the way in, in the
				// resolve, so the swapchain image already holds only the part
				// that is meant to be seen.
				quad.subImage.imageRect.offset.x = 0;
				quad.subImage.imageRect.offset.y = 0;
				quad.subImage.imageRect.extent.width = (int32_t)vr.uiSwapchain.width;
				quad.subImage.imageRect.extent.height = (int32_t)vr.uiSwapchain.height;

				quad.pose = vr.wristPose;
				quad.size.width = size;
				// The shape of what was cropped, not of the buffer, or a strip
				// off the top of the screen comes out as tall as the screen.
				quad.size.height = size
					* (cropHeight * (float)vr.uiSwapchain.height)
					/ (cropWidth * (float)vr.uiSwapchain.width);
			}

			layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&quad;
		}
	}

	memset(&endInfo, 0, sizeof(endInfo));
	endInfo.type = XR_TYPE_FRAME_END_INFO;
	endInfo.displayTime = vr.frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (vr.frameState.shouldRender && vr.layerReady) ? layerCount : 0;
	endInfo.layers = layers;

	XR_CHECK(xrEndFrame(vr.session, &endInfo));

	vr.phaseSubmit += Sys_Milliseconds() - submitStart;

	if (!vr_traceFrame || vr_traceFrame->integer) {
		VR_TraceFrameTiming();
	}

	vr.frameStarted = qfalse;
	vr.layerReady = qfalse;
	vr.pointerLayerReady = qfalse;
	vr.wristLayerReady = qfalse;

	if (!vr.screenLayerActive) {
		// Back in the world, so forget where the panel was; the next time flat
		// content appears it is placed in front of wherever the viewer is then,
		// rather than left behind at the old spot.
		vr.screenAnchorValid = qfalse;
	}

	vr.screenLayerActive = qfalse;
}
