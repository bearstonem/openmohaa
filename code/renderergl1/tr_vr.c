/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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

// tr_vr.c -- everything the fixed function renderer needs to know about a
// headset, which is deliberately very little. The VR layer owns the session,
// the poses and the swapchains; this is the surface where it meets the
// renderer.

#include "tr_local.h"

vrViewState_t vrView;

// Framebuffer objects are past this renderer's era, so its qgl.h names none of
// them. Spelled out here the same way qgl.h spells out the multitexture
// extension it also predates.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif

static void(APIENTRY *qglBindFramebuffer)(GLenum target, GLuint framebuffer);

/*
============
RE_SetVRView
============
*/
void RE_SetVRView(
    const float *origin,
    const vec3_t *axis,
    float         tanLeft,
    float         tanRight,
    float         tanUp,
    float         tanDown,
    float         baseYaw,
    int           eye
)
{
    if (!origin || !axis) {
        vrView.active = qfalse;
        return;
    }

    vrView.baseYaw = baseYaw;
    vrView.eye     = eye;

    VectorCopy(origin, vrView.origin);
    VectorCopy(axis[0], vrView.axis[0]);
    VectorCopy(axis[1], vrView.axis[1]);
    VectorCopy(axis[2], vrView.axis[2]);

    vrView.tanLeft  = tanLeft;
    vrView.tanRight = tanRight;
    vrView.tanUp    = tanUp;
    vrView.tanDown  = tanDown;
    vrView.active   = qtrue;
}

/*
============
RE_SetDefaultFramebuffer

Substitutes the framebuffer the renderer treats as the screen, so that an eye
is drawn straight into its OpenXR swapchain image.

This renderer predates framebuffer objects and has none of its own - it draws
to whatever is bound and never touches the binding. That makes this the whole
of the redirect: bind the eye once per pass and everything that follows lands
there. There is no equivalent of rend2's FBO_Bind to intercept and nothing to
restore, which is the one respect in which the fixed function path is the
easier of the two to put a headset behind.

The entry point is asked for rather than declared with the rest because
renderergl1 has no extension probe - it is loaded on first use, through the
same resolver as everything else so that it reaches gl4es and gl4es stays
aware of which framebuffer is bound.
============
*/
void RE_SetDefaultFramebuffer(unsigned int framebuffer)
{
    static qboolean resolved = qfalse;

    if (!resolved) {
        resolved          = qtrue;
        qglBindFramebuffer = GLimp_GetProcAddress("glBindFramebuffer");

        if (!qglBindFramebuffer) {
            ri.Printf(PRINT_WARNING, "glBindFramebuffer unavailable; VR eye redirect disabled\n");
        }
    }

    if (!qglBindFramebuffer) {
        return;
    }

    qglBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Drawing into a framebuffer means something else is presenting it, so the
    // window must not also be swapped - two presentation paths running at
    // unrelated rates is what flicker looks like.
    GLimp_SetPresentsToWindow(framebuffer == 0 ? qtrue : qfalse);
}

/*
====================
R_ApplyVRView

Puts the headset on top of whatever camera the game asked for.

Done in the renderer rather than the game because every camera reaches it
through RE_RenderScene - the player's view, cutscene cameras, ladder climbs,
death animations - so head tracking follows all of them without each having to
know about VR.

The game's camera decides where the body is and which way it faces. Everything
else comes from the headset: pitch and roll are taken from the head alone,
since blending them with the game's would fight the viewer's own neck.
====================
*/
void R_ApplyVRView(refdef_t *fd)
{
    vec3_t baseAngles, bodyAngles, bodyAxis[3];
    int    i;

    if (!vrView.active) {
        return;
    }

    // The game's camera decides where the body stands and which way it faces;
    // the headset decides everything about where the eyes are relative to that.
    // Only the heading is taken from the game - its pitch and roll would fight
    // the viewer's own neck.
    //
    // Minus whatever heading the client already fed into the view angles. The
    // game is told where the player is looking so that objectives and anything
    // else consulting the view angles agree with what is in front of them, but
    // that same heading then comes back through the player state and into this
    // camera - and composing the head onto it again turns it twice. Yaw ends up
    // doubled while pitch, which the game's camera does not carry here, does
    // not, and half of the doubled yaw arrives a frame late through prediction.
    // The result reads as a world that swims when the head turns.
    //
    // Taking it back off leaves the game's own contributions - recoil, lean,
    // cutscene cameras, ladders - composed with the current headset pose, which
    // is what this function is for.
    vectoangles(fd->viewaxis[0], baseAngles);
    VectorSet(bodyAngles, 0, baseAngles[YAW] - vrView.baseYaw, 0);
    AnglesToAxis(bodyAngles, bodyAxis);

    // Rotate the head's axes into the body's frame. Composed as a rotation
    // rather than by adding Euler angles: adding them only happens to work
    // while pitch and roll are small, and turns every head movement into a
    // twist once they are not.
    for (i = 0; i < 3; i++) {
        vec3_t rotated;

        VectorScale(bodyAxis[0], vrView.axis[i][0], rotated);
        VectorMA(rotated, vrView.axis[i][1], bodyAxis[1], rotated);
        VectorMA(rotated, vrView.axis[i][2], bodyAxis[2], rotated);
        VectorCopy(rotated, fd->viewaxis[i]);
    }

    if (r_vrTrace && r_vrTrace->integer) {
        static int lastTrace;
        int        now = ri.Milliseconds();

        if (now - lastTrace > 1000) {
            lastTrace = now;
            ri.Printf(
                PRINT_ALL,
                "VR view: org %.0f %.0f %.0f body yaw %.1f | head off %.1f %.1f %.1f fwd %.2f %.2f %.2f\n",
                fd->vieworg[0],
                fd->vieworg[1],
                fd->vieworg[2],
                baseAngles[YAW],
                vrView.origin[0],
                vrView.origin[1],
                vrView.origin[2],
                vrView.axis[0][0],
                vrView.axis[0][1],
                vrView.axis[0][2]
            );
        }
    }

    // The head's offset within the play space, turned to face the same way the
    // body does before it is added. Leaning left stays leaning left whichever
    // way the player has turned.
    VectorMA(fd->vieworg, vrView.origin[0], bodyAxis[0], fd->vieworg);
    VectorMA(fd->vieworg, vrView.origin[1], bodyAxis[1], fd->vieworg);
    VectorMA(fd->vieworg, vrView.origin[2], bodyAxis[2], fd->vieworg);
}

/*
================
R_VRAdjust2DOrtho

Bends a 2D ortho window to suit the eye it is about to be drawn into.

Flat content in a headset needs three corrections that no screen ever does.
Each eye looks through a frustum that is not centred on its own axis, so
identical 2D lands in a different place in each eye; there is no disparity, so
what does line up sits at infinity; and the edges of the display are outside
what can comfortably be read.

Expressed as a change to the ortho window rather than a transform on every
vertex, because the window is one matrix and the vertices are thousands.

Everything 2D in this renderer goes through Set2DWindow, including RB_SetGL2D,
so unlike rend2 there is a single place to apply it.
================
*/
void R_VRAdjust2DOrtho(float *left, float *right, float *bottom, float *top)
{
    float scale, depth;
    float tanWidth, tanHeight;
    float offX, offY, parallax;
    float windowWidth, windowHeight;

    if (!vrView.active) {
        return;
    }

    tanWidth  = vrView.tanRight - vrView.tanLeft;
    tanHeight = vrView.tanUp - vrView.tanDown;

    // No frustum yet means nothing to centre on, and dividing by it would leave
    // a NaN in the projection - which does not look like an error, it looks like
    // the HUD simply not being there.
    if (tanWidth < 0.0001f || tanHeight < 0.0001f) {
        return;
    }

    scale = vr_hudScale ? vr_hudScale->value : 1.0f;
    depth = vr_hudDepth ? vr_hudDepth->value : 2.0f;

    if (scale < 0.1f) {
        scale = 0.1f;
    } else if (scale > 1.0f) {
        scale = 1.0f;
    }
    if (depth < 0.5f) {
        depth = 0.5f;
    }

    windowWidth  = *right - *left;
    windowHeight = *bottom - *top;

    offX = -(vrView.tanRight + vrView.tanLeft) / tanWidth;
    offY = -(vrView.tanUp + vrView.tanDown) / tanHeight;

    // Half an interpupillary distance at the given range, as a share of the
    // eye's half width, opposite ways round for the two eyes. This is what puts
    // the HUD at that range instead of infinitely far away.
    parallax = (2.0f * 0.032f) / (depth * tanWidth);
    offX += (vrView.eye == 0) ? parallax : -parallax;

    // A window wider than the content draws the content smaller and keeps it
    // centred; the offsets then slide it bodily.
    *left  = *left - windowWidth * (1.0f + offX - scale) / (2.0f * scale);
    *right = *left + windowWidth / scale;

    *top    = *top + windowHeight * (offY + scale - 1.0f) / (2.0f * scale);
    *bottom = *top + windowHeight / scale;
}
