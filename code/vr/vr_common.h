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

#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VR_EYE_LEFT  0
#define VR_EYE_RIGHT 1
#define VR_MAX_EYES  2

/*
The headset's view of a frame.

Angles are the engine's (pitch, yaw, roll) in degrees; origin is in engine
units. The four tangents describe the eye's frustum: OpenXR gives an
asymmetric field of view per eye, and using a symmetric approximation instead
misaligns the two images enough to be uncomfortable, so they are carried
through to the projection matrix rather than reduced to a single fov value.
*/
typedef struct {
	vec3_t origin;

	// Forward, left and up, in the engine's convention - the same layout as
	// refdef_t's viewaxis. Kept as axes rather than pitch/yaw/roll because the
	// round trip through Euler angles loses the distinction between rotations
	// that differ only near the poles, and reintroduces it as a twist.
	vec3_t axis[3];

	float tanLeft;
	float tanRight;
	float tanUp;
	float tanDown;
} vrEyeView_t;

// Brings up the OpenXR instance and picks a system. Must run before the GL
// context exists, and returns false if there is no headset or no runtime, in
// which case the engine carries on as an ordinary flat application.
qboolean VR_Init(void);
void     VR_Shutdown(void);

// True once the instance is up; the engine uses this to decide whether to
// render in stereo at all.
qboolean VR_Enabled(void);

// The session and its swapchains are bound to the GL context that is current
// when they are created, so both are torn down and rebuilt whenever the engine
// restarts the renderer.
void VR_CreateSession(void);
void VR_DestroySession(void);

// The per-eye render target size the runtime asks for.
void VR_GetRenderResolution(int *width, int *height);

// Begins a frame and updates the tracked poses. Returns false when the session
// is not running, meaning nothing should be rendered or submitted this frame.
qboolean VR_BeginFrame(void);

// Bracket each eye's rendering. Between them the engine draws its scene into
// the framebuffer VR_PrepareEye bound.
void VR_PrepareEye(int eye);
void VR_FinishEye(int eye);

// True when the frame is flat content - menus, the loading screen, a cinematic
// - which is shown on a quad fixed in front of the viewer rather than rendered
// into both eyes. Drawing screen space content identically into two eyes gives
// it zero disparity while each eye still reprojects through its own asymmetric
// frustum, so the two images never line up and the result cannot be fused.
qboolean VR_UseScreenLayer(void);

// Bracket the single pass that draws the flat content, in place of the two eye
// passes.
void VR_PrepareScreenLayer(void);
void VR_FinishScreenLayer(void);

// Hands the finished images to the compositor.
void VR_SubmitFrame(void);

// Where the headset is, for the frame that VR_BeginFrame set up.
void VR_GetEyeView(int eye, vrEyeView_t *view);

// Reads the controllers and, while flat content is on screen, points the
// engine's cursor at wherever the player is aiming on the panel.
void VR_UpdateInput(void);

#ifdef __cplusplus
}
#endif
