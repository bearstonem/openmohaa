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

#include <EGL/egl.h>
#include <GLES3/gl3.h>

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
	XrPath          handPaths[2];
	XrSpace         aimSpaces[2];
	qboolean        actionsReady;
	qboolean        selectWasDown;
	int             pointerHand;

	cvar_t         *vr_worldscale;
	cvar_t         *vr_screenDistance;
	cvar_t         *vr_screenSize;
} vr;

static cvar_t *vr_traceTracking;

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
VR_Init
==================
*/
qboolean VR_Init(void)
{
	const char *extensions[] = {
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	};

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
	vr_traceTracking = Cvar_Get("vr_traceTracking", "0", 0);

	if (!VR_InitLoader()) {
		return qfalse;
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
	instanceInfo.enabledExtensionCount = ARRAY_LEN(extensions);
	instanceInfo.enabledExtensionNames = extensions;

	// A missing runtime is not an error: the game should still run flat.
	if (!XR_CHECK(xrCreateInstance(&instanceInfo, &vr.instance))) {
		Com_Printf("OpenXR: no runtime available, running without VR\n");
		vr.instance = XR_NULL_HANDLE;
		return qfalse;
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
		glDeleteFramebuffers(swapchain->imageCount, swapchain->frameBuffers);
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
VR_CreateSwapchain

One swapchain per eye, each image wrapped in a framebuffer with its own depth
buffer so the engine can render straight into it.
==================
*/
static qboolean VR_CreateSwapchain(vrSwapchain_t *swapchain, uint32_t width, uint32_t height)
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
	glGenFramebuffers(swapchain->imageCount, swapchain->frameBuffers);

	for (i = 0; i < swapchain->imageCount; i++) {
		glBindRenderbuffer(GL_RENDERBUFFER, swapchain->depthBuffers[i]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, swapchain->width, swapchain->height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		glBindFramebuffer(GL_FRAMEBUFFER, swapchain->frameBuffers[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			swapchain->images[i].image, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
			swapchain->depthBuffers[i]);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			Com_Printf("OpenXR: incomplete eye framebuffer %u\n", i);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return qfalse;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

	if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
		Com_Printf("OpenXR: no current EGL context; cannot create a session\n");
		return;
	}

	// The runtime wants the EGLConfig the context was made with, which EGL will
	// only hand back by id.
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

	for (i = 0; i < VR_MAX_EYES; i++) {
		if (!VR_CreateSwapchain(&vr.swapchains[i], vr.eyeWidth, vr.eyeHeight)) {
			Com_Printf("OpenXR: failed to create the swapchain for eye %d\n", i);
			VR_DestroySession();
			return;
		}
	}

	if (!VR_CreateSwapchain(&vr.uiSwapchain, vr.eyeWidth, vr.eyeHeight)) {
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

	glGenFramebuffers(1, &vr.uiFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, vr.uiFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr.uiTexture, 0);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	VR_CreateActions();

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
	XrActionSuggestedBinding      bindings[8];
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

			// Thumbsticks only exist on the Touch profile; the simple
			// controller has none, and a binding it does not know would have
			// the runtime reject the whole set.
			if (i == 0) {
				Com_sprintf(path, sizeof(path), "/user/hand/%s/input/thumbstick", hands[hand]);
				bindings[count].action = (hand == 0) ? vr.moveAction : vr.turnAction;
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
VR_PointAtScreen

Casts the hand's aim ray at the panel and, if it lands on it, moves the
engine's cursor to the same spot. The engine then draws its own cursor there,
so the menus behave as they always did.
==================
*/
static qboolean VR_PointAtScreen(const XrPosef *aim)
{
	XrVector3f  forward  = { 0.0f, 0.0f, -1.0f };
	XrVector3f  rightAxis = { 1.0f, 0.0f, 0.0f };
	XrVector3f  upAxis   = { 0.0f, 1.0f, 0.0f };
	XrVector3f  normalAxis = { 0.0f, 0.0f, 1.0f };
	XrVector3f  dir, right, up, normal, toPlane, hit;
	float       denom, distance, localX, localY;
	float       halfWidth, halfHeight;
	const float size = vr.vr_screenSize->value > 0.0f ? vr.vr_screenSize->value : 3.0f;

	if (!vr.screenAnchorValid) {
		return qfalse;
	}

	halfWidth = size * 0.5f;
	halfHeight = halfWidth * (float)vr.uiSwapchain.height / (float)vr.uiSwapchain.width;

	VR_RotateVector(&aim->orientation, &forward, &dir);
	VR_RotateVector(&vr.screenAnchor.orientation, &rightAxis, &right);
	VR_RotateVector(&vr.screenAnchor.orientation, &upAxis, &up);
	VR_RotateVector(&vr.screenAnchor.orientation, &normalAxis, &normal);

	denom = dir.x * normal.x + dir.y * normal.y + dir.z * normal.z;

	// Parallel to the panel, or aiming at its back.
	if (fabsf(denom) < 0.0001f) {
		return qfalse;
	}

	toPlane.x = vr.screenAnchor.position.x - aim->position.x;
	toPlane.y = vr.screenAnchor.position.y - aim->position.y;
	toPlane.z = vr.screenAnchor.position.z - aim->position.z;

	distance = (toPlane.x * normal.x + toPlane.y * normal.y + toPlane.z * normal.z) / denom;

	if (distance <= 0.0f) {
		return qfalse;
	}

	hit.x = aim->position.x + dir.x * distance - vr.screenAnchor.position.x;
	hit.y = aim->position.y + dir.y * distance - vr.screenAnchor.position.y;
	hit.z = aim->position.z + dir.z * distance - vr.screenAnchor.position.z;

	localX = hit.x * right.x + hit.y * right.y + hit.z * right.z;
	localY = hit.x * up.x + hit.y * up.y + hit.z * up.z;

	if (fabsf(localX) > halfWidth || fabsf(localY) > halfHeight) {
		return qfalse;
	}

	// Panel space is centred and Y up; the engine's screen is corner based and
	// Y down.
	CL_SetMousePos(
		(int)((localX / halfWidth * 0.5f + 0.5f) * cls.glconfig.vidWidth),
		(int)((0.5f - localY / halfHeight * 0.5f) * cls.glconfig.vidHeight));

	return qtrue;
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

	// Whichever hand is pointing at the panel drives the cursor, so it does not
	// matter which one the player picks up. The last hand to be on target wins,
	// which keeps it steady when both are pointing.
	for (hand = 0; hand < 2 && !onScreen; hand++) {
		int which = (vr.pointerHand + hand) & 1;

		memset(&location, 0, sizeof(location));
		location.type = XR_TYPE_SPACE_LOCATION;

		if (!XR_SUCCEEDED(xrLocateSpace(vr.aimSpaces[which], vr.stageSpace,
				vr.frameState.predictedDisplayTime, &location))
			|| !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			|| !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			continue;
		}

		if (VR_UseScreenLayer() && VR_PointAtScreen(&location.pose)) {
			vr.pointerHand = which;
			onScreen = qtrue;
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
		input->turn = stick.currentState.x;
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
			} else {
				input->weaponYaw = handAngles[YAW];
				input->weaponPitch = handAngles[PITCH];
				input->handsTracked = qtrue;
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

	if (vr.uiFramebuffer) {
		glDeleteFramebuffers(1, &vr.uiFramebuffer);
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

	memset(&waitInfo, 0, sizeof(waitInfo));
	waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;

	memset(&vr.frameState, 0, sizeof(vr.frameState));
	vr.frameState.type = XR_TYPE_FRAME_STATE;

	if (!XR_CHECK(xrWaitFrame(vr.session, &waitInfo, &vr.frameState))) {
		return qfalse;
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

	// Bind it here rather than leaving it to the renderer: the renderer only
	// binds framebuffers when its own framebuffer support is enabled, and the
	// eye target has to be current either way.
	glBindFramebuffer(GL_FRAMEBUFFER, swapchain->frameBuffers[swapchain->acquiredIndex]);
	glViewport(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
	glScissor(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// From here the renderer's idea of "the screen" is this eye's image.
	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(swapchain->frameBuffers[swapchain->acquiredIndex]);
	}

	// ...and its camera is this eye.
	if (re.SetVRView) {
		const vrEyeView_t *view = &vr.eyeViews[eye];

		re.SetVRView(view->origin, view->axis,
			view->tanLeft, view->tanRight, view->tanUp, view->tanDown);
	}
}

/*
==================
VR_FinishEye
==================
*/
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

	// The compositor reads the alpha channel; the engine leaves it at whatever
	// the scene wrote, which shows up as a translucent image.
	glBindFramebuffer(GL_FRAMEBUFFER, swapchain->frameBuffers[swapchain->acquiredIndex]);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(0);
	}

	// Back to the flat projection, so anything drawn outside an eye pass - the
	// screen layer, a loading screen - is not left with this eye's frustum.
	if (re.SetVRView) {
		re.SetVRView(NULL, NULL, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	memset(&releaseInfo, 0, sizeof(releaseInfo));
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

	swapchain->acquired = qfalse;

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

	// Deliberately not cleared: the engine's 2D path treats the screen as
	// something that persists between frames, and this buffer is the only
	// place that is true.
	glBindFramebuffer(GL_FRAMEBUFFER, vr.uiFramebuffer);
	glViewport(0, 0, (GLsizei)vr.uiSwapchain.width, (GLsizei)vr.uiSwapchain.height);

	if (re.SetDefaultFramebuffer) {
		re.SetDefaultFramebuffer(vr.uiFramebuffer);
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

	glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, vr.uiFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, swapchain->frameBuffers[swapchain->acquiredIndex]);
	glBlitFramebuffer(0, 0, (GLint)swapchain->width, (GLint)swapchain->height,
		0, 0, (GLint)swapchain->width, (GLint)swapchain->height,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// The compositor reads alpha; the engine leaves whatever the scene wrote.
	glBindFramebuffer(GL_FRAMEBUFFER, swapchain->frameBuffers[swapchain->acquiredIndex]);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	memset(&releaseInfo, 0, sizeof(releaseInfo));
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));

	swapchain->acquired = qfalse;
	vr.screenLayerActive = qtrue;
	vr.layerReady = qtrue;
}

/*
==================
VR_SubmitFrame
==================
*/
void VR_SubmitFrame(void)
{
	XrCompositionLayerProjection        projection;
	XrCompositionLayerQuad              quad;
	const XrCompositionLayerBaseHeader  *layers[1];
	XrFrameEndInfo                      endInfo;
	int                                 layerCount = 0;

	if (!vr.frameStarted) {
		return;
	}

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
	} else {
		memset(&projection, 0, sizeof(projection));
		projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		projection.layerFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
		projection.space = vr.stageSpace;
		projection.viewCount = VR_MAX_EYES;
		projection.views = vr.projectionViews;

		layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projection;
	}

	memset(&endInfo, 0, sizeof(endInfo));
	endInfo.type = XR_TYPE_FRAME_END_INFO;
	endInfo.displayTime = vr.frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (vr.frameState.shouldRender && vr.layerReady) ? layerCount : 0;
	endInfo.layers = layers;

	XR_CHECK(xrEndFrame(vr.session, &endInfo));

	vr.frameStarted = qfalse;
	vr.layerReady = qfalse;

	if (!vr.screenLayerActive) {
		// Back in the world, so forget where the panel was; the next time flat
		// content appears it is placed in front of wherever the viewer is then,
		// rather than left behind at the old spot.
		vr.screenAnchorValid = qfalse;
	}

	vr.screenLayerActive = qfalse;
}
