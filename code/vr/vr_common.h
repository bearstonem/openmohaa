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

// The panel on the player's wrist, which carries the HUD and any in game menu
// while the world is being drawn behind it. Shown by raising the off hand and
// turning it towards the face, the way a watch is read.
qboolean VR_WristPanelEnabled(void);
qboolean VR_WristPanelVisible(void);
void VR_PrepareWristPanel(void);
void VR_FinishWristPanel(void);

// Hands the finished images to the compositor.
void VR_SubmitFrame(void);

// Where the headset is, for the frame that VR_BeginFrame set up.
void VR_GetEyeView(int eye, vrEyeView_t *view);

// Reads the controllers and, while flat content is on screen, points the
// engine's cursor at wherever the player is aiming on the panel.
void VR_UpdateInput(void);

/*
What the player is doing, for the client to turn into a usercmd.
*/
typedef struct {
	qboolean	valid;

	float		moveForward;	// -1..1, left stick
	float		moveRight;
	float		turn;			// -1..1, right stick

	// How far the head moved this frame, in engine units, in the play space's
	// own frame. Fed into the usercmd as movement so that walking about the room
	// takes the character with it - see CL_VRMove.
	float		stepForward;
	float		stepRight;

	float		headYaw;		// degrees, relative to where they started
	float		headPitch;

	// Where each hand points, same reference as the head. The off hand steers
	// walking so the player can look around without veering.
	float		offhandYaw;
	float		weaponYaw;
	float		weaponPitch;

	// Separately, because they are used for different things and either hand can
	// drop out on its own - the off hand steering walking must not stop working
	// because the weapon hand went out of view.
	qboolean	offhandTracked;
	qboolean	weaponTracked;
} vrInput_t;

qboolean VR_GetInput(vrInput_t *input);

/*
Where the weapon hand is pointing, for the game to fire along.

Not an absolute direction. The controller's own yaw is meaningless to the game -
the player can turn on the spot with the stick, and the play space has an
arbitrary forward - so what travels is how far the hand *leads the head*, which
the game then adds to the view yaw it already has. That is what RTCWQuest does
(rtcw/src/game/g_weapon.c:1904):

	viewang[YAW] = ent->client->ps.viewangles[YAW]
	             + (gVR->weaponangles[YAW] - gVR->hmdorientation[YAW]);

	out[PITCH]  the controller's pitch, absolute - the head contributes nothing
	            to it, and blending the two would fight the player's own wrist
	out[YAW]    degrees the hand leads the head by
	out[ROLL]   unused; roll does not change a forward vector

False when there is no headset, no tracked weapon hand, or VR is off - the
caller must keep its existing aim in that case rather than firing at zero.
*/
qboolean VR_GetWeaponAim(vec3_t out);

/*
Where the weapon hand is, for the view model to hang off.

offset is the hand relative to the head, in engine units and the engine's frame.
angles carry the controller's pitch and roll and, in YAW, how far the hand leads
the head. headHeight is the head above the floor in metres, so the caller can
put the weapon at the player's real hand height rather than at eye level.
*/
qboolean VR_GetWeaponPose(vec3_t offset, vec3_t angles, float *headHeight);

// How much of the head's heading the client has already written into the game's
// view angles. The renderer takes it back off before composing the headset onto
// the game's camera, so the same rotation is not applied twice.
void VR_SetBaseYaw(float yaw);

// The renderer's own split of the frame, for the timing report: how long was
// spent building the scene against how long was spent issuing it.
void VR_TraceRenderTimes(int frontEndMsec, int backEndMsec);

// How long the game spent building the frame, against how long the renderer
// spent issuing it. The first covers everything cgame does per frame, which is
// far more than drawing.
void VR_TraceSceneTimes(int sceneMsec, int issueMsec);

// Inside the scene: the 3D half against the flat half. Splits cgame's own per
// frame work from the 2D path, which flushes the render command buffer once per
// string drawn and is a suspect in its own right.
void VR_TraceViewTimes(int worldMsec, int hudMsec);

// Of the flat half, how much is the game's own HUD as against the engine's
// overlays around it.
void VR_TraceHudTimes(int cgameMsec);

// Counters for the paths that draw the flat content, so a path that is never
// reached can be told apart from one that is reached and draws nothing.
#define VRTRACE_DRAW2D      0	// View3D::Draw2D entered
#define VRTRACE_CGAME_HUD   1	// cge->CG_Draw2D entered
#define VRTRACE_UPDATEVIEWS 2	// uWinMan.UpdateViews entered
#define VRTRACE_VIEW3D      3	// View3D::Draw entered
#define VRTRACE_COUNT       4
void VR_TraceEvent(int which);

// The client's idea of the frame, so the report can say what mode it was in.
void VR_TraceState(int drawMode, int hudPass, int noMenus);

// The flat pass broken down further: the fade and letterbox overlays, the
// centre print, and the sound/net/subtitle overlays, against the set2D that
// precedes them all.
void VR_TraceHudParts(int setup, int fades, int prints, int overlays, int tail);

#ifdef __cplusplus
}
#endif
