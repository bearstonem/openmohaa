/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../renderercommon/tr_common.h"
#include "../sys/sys_local.h"
#include "sdl_icon.h"

#ifdef USE_GL4ES
#include <dlfcn.h>
#include <EGL/egl.h>
#endif

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

SDL_Window *SDL_window = NULL;
static SDL_GLContext SDL_glContext = NULL;

cvar_t *r_allowSoftwareGL; // Don't abort out if a hardware visual can't be obtained
cvar_t *r_allowResize; // make window resizable
cvar_t *r_centerWindow;
cvar_t *r_sdlDriver;
cvar_t *r_preferOpenGLES;

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize( void )
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}


/*
===============
GLimp_DetectAvailableModes
===============
*/
static void GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	int numSDLModes;
	SDL_Rect *modes;
	int numModes = 0;

	SDL_DisplayMode windowMode;
	int display = SDL_GetWindowDisplayIndex( SDL_window );
	if( display < 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display index, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}
	numSDLModes = SDL_GetNumDisplayModes( display );

	if( SDL_GetWindowDisplayMode( SDL_window, &windowMode ) < 0 || numSDLModes <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display mode, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}

	modes = SDL_calloc( (size_t)numSDLModes, sizeof( SDL_Rect ) );
	if ( !modes )
	{
		ri.Error( ERR_FATAL, "Out of memory" );
	}

	for( i = 0; i < numSDLModes; i++ )
	{
		SDL_DisplayMode mode;

		if( SDL_GetDisplayMode( display, i, &mode ) < 0 )
			continue;

		if( !mode.w || !mode.h )
		{
			ri.Printf( PRINT_ALL, "Display supports any resolution\n" );
			SDL_free( modes );
			return;
		}

		if( windowMode.format != mode.format )
			continue;

		// SDL can give the same resolution with different refresh rates.
		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode.w == modes[ j ].w && mode.h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = mode.w;
		modes[ numModes ].h = mode.h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}
	SDL_free( modes );
}

/*
===============
OpenGL ES compatibility
===============
*/
static void APIENTRY GLimp_GLES_ClearDepth( GLclampd depth ) {
	qglClearDepthf( depth );
}

static void APIENTRY GLimp_GLES_DepthRange( GLclampd near_val, GLclampd far_val ) {
	qglDepthRangef( near_val, far_val );
}

static void APIENTRY GLimp_GLES_DrawBuffer( GLenum mode ) {
	// unsupported
}

static void APIENTRY GLimp_GLES_PolygonMode( GLenum face, GLenum mode ) {
	// unsupported
}

#ifdef USE_GL4ES
/*
===============
GLimp_GL4ES_GetProcAddress

The fixed function renderer's entry points have to come from gl4es and from
nowhere else.

SDL cannot be asked for them. On EGL 1.5 - which the Quest is -
SDL_EGL_GetProcAddress tries eglGetProcAddress before it tries the library it
loaded, and the driver answers for every name OpenGL 1.x and ES have in common:
glEnable, glBindTexture, glDrawArrays, glTexImage2D and eighty more. Only the
names that exist nowhere but desktop GL - glBegin, glMatrixMode - would reach
gl4es. That split is worse than either half on its own, because gl4es batches
geometry and tracks the matrix stack against calls the driver would then never
be told about.

So resolve against the gl4es handle directly. It is already in the process, in
DT_NEEDED, so this dlopen only takes a reference to it.
===============
*/
static void *GLimp_GL4ES_GetProcAddress( const char *name ) {
	static void *gl4es = NULL;

	if ( !gl4es ) {
		gl4es = dlopen( "libgl4es.so", RTLD_NOW | RTLD_LOCAL );

		if ( !gl4es ) {
			Com_Error( ERR_FATAL, "Could not open libgl4es.so: %s", dlerror() );
		}

		// NO_INIT_CONSTRUCTOR is set, so gl4es does not attach itself to the
		// context behind our back; it is told to, once the context is current.
		void ( *initialize_gl4es )( void ) = dlsym( gl4es, "initialize_gl4es" );

		if ( !initialize_gl4es ) {
			Com_Error( ERR_FATAL, "libgl4es.so has no initialize_gl4es" );
		}

		initialize_gl4es();
	}

	return dlsym( gl4es, name );
}

/*
===============
GLimp_ExtensionSupported

And the extension *string* has to come from gl4es too. SDL_GL_ExtensionSupported
resolves glGetString through SDL, so it reads the driver's list - which is the
ES one, and names none of the desktop extensions the renderer asks after. Left
to SDL, GL_ARB_multitexture reads as absent and the renderer quietly drops to a
single texture unit.

qglGetString is gl4es's by this point; GLimp_GetProcAddresses has already run.
===============
*/
static qboolean GLimp_ExtensionSupported( const char *extension ) {
	const char *extensions = (const char *)qglGetString( GL_EXTENSIONS );
	const char *p;
	size_t len = strlen( extension );

	if ( !extensions ) {
		return qfalse;
	}

	// Whole tokens only, or GL_EXT_texture_compression_s3tc would answer for a
	// query about GL_EXT_texture_compression.
	for ( p = extensions; ( p = strstr( p, extension ) ) != NULL; p += len ) {
		if ( ( p == extensions || p[-1] == ' ' ) && ( p[len] == ' ' || p[len] == '\0' ) ) {
			return qtrue;
		}
	}

	return qfalse;
}
#else
#define GLimp_ExtensionSupported( name ) SDL_GL_ExtensionSupported( name )
#endif

/*
===============
GLimp_GetProcAddress

The renderer's one way of asking for a GL entry point, so that everything it
calls comes from the same implementation. Everything above resolves through
this, and so does anything a renderer needs to load for itself - renderergl1
has no extension probe of its own and asks for the framebuffer entry points
here.
===============
*/
void *GLimp_GetProcAddress( const char *name ) {
#ifdef USE_GL4ES
	return GLimp_GL4ES_GetProcAddress( name );
#else
	return SDL_GL_GetProcAddress( name );
#endif
}

/*
===============
GLimp_MakeCurrent

Bind the window's GL context to the calling thread again.

The VR layer hands the OpenXR session the EGL context the engine is already
using, so it needs one to be current at the moment it asks. Whether it is
depends on what SDL and the platform have done with the window since - a hidden
window on Android has no surface to be current against yet, and being current
is per thread in any case. Rather than assume, the session asks for it.

Returns false if there is no context to bind, which is a different thing from
failing to bind one.
===============
*/
qboolean GLimp_MakeCurrent( void ) {
	if ( !SDL_window || !SDL_glContext ) {
		ri.Printf( PRINT_ALL, "GLimp_MakeCurrent: no window (%p) or context (%p)\n",
			(void *)SDL_window, (void *)SDL_glContext );
		return qfalse;
	}

	if ( SDL_GL_MakeCurrent( SDL_window, SDL_glContext ) < 0 ) {
		ri.Printf( PRINT_ALL, "SDL_GL_MakeCurrent failed: %s\n", SDL_GetError() );
		return qfalse;
	}

	return qtrue;
}

/*
===============
GLimp_GetProcAddresses

Get addresses for OpenGL functions.
===============
*/

#ifdef USE_GL4ES
/*
===============
GLimp_BindContextToPbuffer

Give the GL context something to be current against, before anything asks it a
question.

SDL binds the context to the window's EGL surface, and in a headset no such
surface ever arrives - so SDL_EGL_MakeCurrent unbinds everything and returns
success, leaving SDL certain a context is current while EGL reports none.

That was already known; what was missed is how early it matters. gl4es
initialises on the first proc address request, which is the next thing that
happens after the context is created. With nothing current, its hardware
detection and its shader pipeline are set up against no GL at all. The result
is not a failure anyone sees: clears go straight through to the driver and work
perfectly, while every piece of geometry gl4es is asked to draw disappears in
silence, no error raised.

A 16x16 pbuffer is enough to be current against, which is what RTCWQuest uses
(TBXR_Common.c, egl->TinySurface). The VR layer does the same thing later for
the OpenXR session; this is the same fix moved to where it has to happen first.
===============
*/
static void GLimp_BindContextToPbuffer( void )
{
	static const EGLint pbufferAttribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	EGLDisplay display;
	EGLContext context;
	EGLConfig  config = NULL;
	EGLint     configId = 0, numConfigs = 0;
	EGLint     configAttribs[3] = { EGL_CONFIG_ID, 0, EGL_NONE };
	EGLSurface pbuffer;

	if ( eglGetCurrentContext() != EGL_NO_CONTEXT ) {
		return;
	}

	display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
	context = (EGLContext)SDL_glContext;

	if ( display == EGL_NO_DISPLAY || !context ) {
		ri.Printf( PRINT_ALL, "gl4es: no EGL context to bind; geometry will not draw\n" );
		return;
	}

	eglQueryContext( display, context, EGL_CONFIG_ID, &configId );
	configAttribs[1] = configId;

	if ( !eglChooseConfig( display, configAttribs, &config, 1, &numConfigs ) || numConfigs < 1 ) {
		ri.Printf( PRINT_ALL, "gl4es: could not recover EGLConfig %d\n", configId );
		return;
	}

	pbuffer = eglCreatePbufferSurface( display, config, pbufferAttribs );

	if ( pbuffer == EGL_NO_SURFACE ) {
		ri.Printf( PRINT_ALL, "gl4es: could not create the pbuffer (0x%x)\n", eglGetError() );
		return;
	}

	if ( !eglMakeCurrent( display, pbuffer, pbuffer, context ) ) {
		ri.Printf( PRINT_ALL, "gl4es: eglMakeCurrent on the pbuffer failed (0x%x)\n", eglGetError() );
		return;
	}

	ri.Printf( PRINT_ALL, "gl4es: context bound to a 16x16 pbuffer before init\n" );
}
#endif

#ifdef USE_GL4ES
static const qboolean usingGL4ES = qtrue;
#else
static const qboolean usingGL4ES = qfalse;
#endif

static qboolean GLimp_GetProcAddresses( qboolean fixedFunction ) {
	qboolean success = qtrue;
	const char *version;

#ifdef __SDL_NOGETPROCADDR__
#define GLE( ret, name, ... ) qgl##name = gl#name;
#elif defined( USE_GL4ES )
#define GLE( ret, name, ... ) qgl##name = (name##proc *) GLimp_GL4ES_GetProcAddress("gl" #name); \
	if ( qgl##name == NULL ) { \
		ri.Printf( PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name ); \
		success = qfalse; \
	}
#else
#define GLE( ret, name, ... ) qgl##name = (name##proc *) SDL_GL_GetProcAddress("gl" #name); \
	if ( qgl##name == NULL ) { \
		ri.Printf( PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name ); \
		success = qfalse; \
	}
#endif

	// OpenGL 1.0 and OpenGL ES 1.0
	GLE(const GLubyte *, GetString, GLenum name)

	if ( !qglGetString ) {
		Com_Error( ERR_FATAL, "glGetString is NULL" );
	}

	version = (const char *)qglGetString( GL_VERSION );

	if ( !version ) {
		Com_Error( ERR_FATAL, "GL_VERSION is NULL" );
	}

	if ( Q_stricmpn( "OpenGL ES", version, 9 ) == 0 ) {
		char profile[6]; // ES, ES-CM, or ES-CL
		sscanf( version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion );
		// common lite profile (no floating point) is not supported
		if ( Q_stricmp( profile, "ES-CL" ) == 0 ) {
			qglesMajorVersion = 0;
			qglesMinorVersion = 0;
		}
	} else {
		sscanf( version, "%d.%d", &qglMajorVersion, &qglMinorVersion );
	}

	if ( fixedFunction ) {
		if ( QGL_VERSION_ATLEAST( 1, 1 ) ) {
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
		} else if ( qglesMajorVersion == 1 && qglesMinorVersion >= 1 ) {
			// OpenGL ES 1.1 (2.0 is not backward compatible)
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_ES_1_1_FIXED_FUNCTION_PROCS;
			// error so this doesn't segfault due to NULL desktop GL functions being used
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version: %s", version );
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 1.1 is required", version );
		}
		// Added in OPM
		//  Add compression-related GL functions for the renderer
		if ( QGL_VERSION_ATLEAST( 1, 3 ) ) {
			QGL_1_3_PROCS;
		}

#ifdef USE_GL4ES
		// gl4es reports a desktop version, so this takes the desktop branch
		// above and picks up the real glDrawBuffer and glPolygonMode. Neither
		// means anything underneath, where the context is OpenGL ES - which is
		// why the ES branch below stubs exactly these two out.
		//
		// glDrawBuffer is the one that matters. The back end names GL_BACK once
		// a frame in RB_DrawBuffer, and GL_BACK is not a buffer a framebuffer
		// object has; asking for it while an eye or the panel is bound leaves
		// the draw buffer in a state where everything afterwards is discarded.
		// Correct geometry, correct matrices, no error on the draw itself, and
		// nothing written anywhere - which is precisely what was happening.
		qglDrawBuffer = GLimp_GLES_DrawBuffer;
		qglPolygonMode = GLimp_GLES_PolygonMode;
#endif
	} else {
		if ( QGL_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;
		} else if ( QGLES_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;

			qglClearDepth = GLimp_GLES_ClearDepth;
			qglDepthRange = GLimp_GLES_DepthRange;
			qglDrawBuffer = GLimp_GLES_DrawBuffer;
			qglPolygonMode = GLimp_GLES_PolygonMode;
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 2.0 is required", version );
		}
	}

	if ( QGL_VERSION_ATLEAST( 3, 0 ) || QGLES_VERSION_ATLEAST( 3, 0 ) ) {
		QGL_3_0_PROCS;
	}

#undef GLE

	return success;
}

/*
===============
GLimp_ClearProcAddresses

Clear addresses for OpenGL functions.
===============
*/
static void GLimp_ClearProcAddresses( void ) {
#define GLE( ret, name, ... ) qgl##name = NULL;

	qglMajorVersion = 0;
	qglMinorVersion = 0;
	qglesMajorVersion = 0;
	qglesMinorVersion = 0;

	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
	QGL_ES_1_1_PROCS;
	QGL_ES_1_1_FIXED_FUNCTION_PROCS;
	QGL_1_3_PROCS;
	QGL_1_5_PROCS;
	QGL_2_0_PROCS;
	QGL_3_0_PROCS;
	QGL_ARB_occlusion_query_PROCS;
	QGL_ARB_framebuffer_object_PROCS;
	QGL_ARB_vertex_array_object_PROCS;
	QGL_EXT_direct_state_access_PROCS;

	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	qglMultiTexCoord2fARB = NULL;

	qglLockArraysEXT = NULL;
	qglUnlockArraysEXT = NULL;

#undef GLE
}

/*
===============
GLimp_SetMode
===============
*/
static int GLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean fixedFunction)
{
	struct GLimp_ContextType {
		int profileMask;
		int majorVersion;
		int minorVersion;
	} contexts[4];
	int numContexts, type;
	const char *glstring;
	int perChannelColorBits;
	int colorBits, depthBits, stencilBits;
	int samples;
	int i = 0;
	SDL_Surface *icon = NULL;
	Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL;
	SDL_DisplayMode desktopMode;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;

	ri.Printf( PRINT_ALL, "Initializing OpenGL display\n");

	if ( r_allowResize->integer )
		flags |= SDL_WINDOW_RESIZABLE;

#ifdef USE_ICON
	icon = SDL_CreateRGBSurfaceFrom(
			(void *)CLIENT_WINDOW_ICON.pixel_data,
			CLIENT_WINDOW_ICON.width,
			CLIENT_WINDOW_ICON.height,
			CLIENT_WINDOW_ICON.bytes_per_pixel * 8,
			CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width,
#ifdef Q3_LITTLE_ENDIAN
			0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
#else
			0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#endif
			);
#endif

	// If a window exists, note its display index
	if( SDL_window != NULL )
	{
		display = SDL_GetWindowDisplayIndex( SDL_window );
		if( display < 0 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GetWindowDisplayIndex() failed: %s\n", SDL_GetError() );
			display = 0;
		}
	}

	if( SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 )
	{
		displayAspect = (float)desktopMode.w / (float)desktopMode.h;

		ri.Printf( PRINT_ALL, "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Memset( &desktopMode, 0, sizeof( SDL_DisplayMode ) );

		ri.Printf( PRINT_ALL,
				"Cannot determine display aspect, assuming 1.333\n" );
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	if (mode == -2)
	{
		// use desktop video resolution
		if( desktopMode.h > 0 )
		{
			glConfig.vidWidth = desktopMode.w;
			glConfig.vidHeight = desktopMode.h;
		}
		else
		{
			glConfig.vidWidth = 640;
			glConfig.vidHeight = 480;
			ri.Printf( PRINT_ALL,
					"Cannot determine display resolution, assuming 640x480\n" );
		}

		glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
	}
	else if ( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ) )
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_ALL, " %d %d\n", glConfig.vidWidth, glConfig.vidHeight);

	// Center window
	if( r_centerWindow->integer && !fullscreen )
	{
		x = ( desktopMode.w / 2 ) - ( glConfig.vidWidth / 2 );
		y = ( desktopMode.h / 2 ) - ( glConfig.vidHeight / 2 );
	}

	// Destroy existing state if it exists
	if( SDL_glContext != NULL )
	{
		GLimp_ClearProcAddresses();
		SDL_GL_DeleteContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if( SDL_window != NULL )
	{
		SDL_GetWindowPosition( SDL_window, &x, &y );
		ri.Printf( PRINT_DEVELOPER, "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	if( fullscreen )
	{
		flags |= SDL_WINDOW_FULLSCREEN;
		glConfig.isFullscreen = qtrue;
	}
	else
	{
		if( noborder )
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig.isFullscreen = qfalse;
	}

	colorBits = r_colorbits->value;
	if ((!colorBits) || (colorBits >= 32))
		colorBits = 24;

	if (!r_depthbits->value)
		depthBits = 24;
	else
		depthBits = r_depthbits->value;

	stencilBits = r_stencilbits->value;
	samples = r_ext_multisample->value;

	numContexts = 0;

	if ( !fixedFunction ) {
		int profileMask;
		qboolean preferOpenGLES;

		SDL_GL_ResetAttributes();
		SDL_GL_GetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, &profileMask );

		preferOpenGLES = ( r_preferOpenGLES->integer == 1 ||
		                 ( r_preferOpenGLES->integer == -1 && profileMask == SDL_GL_CONTEXT_PROFILE_ES ) );

		if ( preferOpenGLES ) {
			// ES 3 has to be asked for by name. WebGL 2.0 isn't fully backward
			// compatible, and EGL hands back a real ES 2.0 context when that is
			// what was requested - so asking for 2.0 first would settle for it
			// even where 3 is available.
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}

		contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_CORE;
		contexts[numContexts].majorVersion = 3;
		contexts[numContexts].minorVersion = 2;
		numContexts++;

		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 2;
		contexts[numContexts].minorVersion = 0;
		numContexts++;

		if ( !preferOpenGLES ) {
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}
	} else {
#ifdef USE_GL4ES
		// The fixed function renderer asks for a fixed function context, and on
		// this platform there is no such thing - the request fails at
		// SDL_CreateWindow with "Couldn't get a visual" and takes the renderer
		// down with it.
		//
		// gl4es is what makes the request answerable, and it wants the opposite
		// of what the renderer thinks it is getting: it presents desktop GL 1.x
		// upwards while running on OpenGL ES underneath. So ask for ES here.
		// The renderer is never told - it reads its version through gl4es, which
		// reports desktop GL, and GLimp_GetProcAddresses takes the desktop
		// fixed function branch on the strength of it.
		//
		// ES 3 first and by name, for the reason given above: EGL hands back a
		// real ES 2.0 context when 2.0 is what was asked for, so asking for it
		// first would settle for it even where 3 is available.
		contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
		contexts[numContexts].majorVersion = 3;
		contexts[numContexts].minorVersion = 0;
		numContexts++;

		contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
		contexts[numContexts].majorVersion = 2;
		contexts[numContexts].minorVersion = 0;
		numContexts++;
#else
		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 1;
		contexts[numContexts].minorVersion = 1;
		numContexts++;
#endif
	}

	for (i = 0; i < 16; i++)
	{
		int testColorBits, testDepthBits, testStencilBits;
		int realColorBits[3];

		// 0 - default
		// 1 - minus colorBits
		// 2 - minus depthBits
		// 3 - minus stencil
		if ((i % 4) == 0 && i)
		{
			// one pass, reduce
			switch (i / 4)
			{
				case 2 :
					if (colorBits == 24)
						colorBits = 16;
					break;
				case 1 :
					if (depthBits == 24)
						depthBits = 16;
					else if (depthBits == 16)
						depthBits = 8;
				case 3 :
					if (stencilBits == 24)
						stencilBits = 16;
					else if (stencilBits == 16)
						stencilBits = 8;
			}
		}

		testColorBits = colorBits;
		testDepthBits = depthBits;
		testStencilBits = stencilBits;

		if ((i % 4) == 3)
		{ // reduce colorBits
			if (testColorBits == 24)
				testColorBits = 16;
		}

		if ((i % 4) == 2)
		{ // reduce depthBits
			if (testDepthBits == 24)
				testDepthBits = 16;
			else if (testDepthBits == 16)
				testDepthBits = 8;
		}

		if ((i % 4) == 1)
		{ // reduce stencilBits
			if (testStencilBits == 24)
				testStencilBits = 16;
			else if (testStencilBits == 16)
				testStencilBits = 8;
			else
				testStencilBits = 0;
		}

		if (testColorBits == 24)
			perChannelColorBits = 8;
		else
			perChannelColorBits = 4;

#ifdef __sgi /* Fix for SGIs grabbing too many bits of color */
		if (perChannelColorBits == 4)
			perChannelColorBits = 0; /* Use minimum size for 16-bit color */

		/* Need alpha or else SGIs choose 36+ bit RGB mode */
		SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 1);
#endif

		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, testDepthBits );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, testStencilBits );

		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

		if(r_stereoEnabled->integer)
		{
			glConfig.stereoEnabled = qtrue;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
		}
		else
		{
			glConfig.stereoEnabled = qfalse;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
		}
		
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

#if 0 // if multisampling is enabled on X11, this causes create window to fail.
		// If not allowing software GL, demand accelerated
		if( !r_allowSoftwareGL->integer )
			SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1 );
#endif

		for ( type = 0; type < numContexts; type++ ) {
			char contextName[32];

			switch ( contexts[type].profileMask ) {
				default:
				case 0:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_CORE:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d Core",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_ES:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL ES %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
			}

			SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, contexts[type].profileMask );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, contexts[type].majorVersion );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, contexts[type].minorVersion );

			if( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE, x, y,
					glConfig.vidWidth, glConfig.vidHeight, flags ) ) == NULL )
			{
				ri.Printf( PRINT_DEVELOPER, "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
				break;
			}

			SDL_glContext = SDL_GL_CreateContext( SDL_window );
			if ( !SDL_glContext )
			{
				SDL_DestroyWindow( SDL_window );
				SDL_window = NULL;
				ri.Printf( PRINT_ALL, "SDL_GL_CreateContext() for %s context failed: %s\n", contextName, SDL_GetError() );
				continue;
			}

#ifdef USE_GL4ES
			// The usual proof that a context is live is GL_VERSION coming back
			// non-NULL from GLimp_GetProcAddresses below. That test does not
			// work here: gl4es answers glGetString for VERSION, VENDOR and
			// RENDERER out of its own constants, without asking the driver
			// anything, so a context that was created but never bound sails
			// through it and is only noticed much later - when the VR layer
			// asks EGL for the current context and finds none.
			if ( !SDL_GL_GetCurrentContext() )
			{
				ri.Printf( PRINT_ALL, "%s context was created but is not current: %s\n",
					contextName, SDL_GetError() );
				SDL_GL_DeleteContext( SDL_glContext );
				SDL_glContext = NULL;
				SDL_DestroyWindow( SDL_window );
				SDL_window = NULL;
				continue;
			}
#endif

#ifdef USE_GL4ES
			// Before GLimp_GetProcAddresses, because that is what initialises
			// gl4es and gl4es needs a context to look at.
			GLimp_BindContextToPbuffer();
#endif

			if ( !GLimp_GetProcAddresses( fixedFunction ) )
			{
				ri.Printf( PRINT_ALL, "GLimp_GetProcAddresses() for %s context failed\n", contextName );
				GLimp_ClearProcAddresses();
				SDL_GL_DeleteContext( SDL_glContext );
				SDL_glContext = NULL;
				SDL_DestroyWindow( SDL_window );
				SDL_window = NULL;
				continue;
			}

			if ( contexts[type].profileMask == SDL_GL_CONTEXT_PROFILE_CORE ) {
				const char *renderer;

				renderer = (const char *)qglGetString( GL_RENDERER );

				if ( !renderer || strstr( renderer, "Software Renderer" ) || strstr( renderer, "Software Rasterizer" ) )
				{
					ri.Printf( PRINT_ALL, "GL_RENDERER is %s, rejecting %s context\n", renderer, contextName );

					GLimp_ClearProcAddresses();
					SDL_GL_DeleteContext( SDL_glContext );
					SDL_glContext = NULL;
					SDL_DestroyWindow( SDL_window );
					SDL_window = NULL;
					continue;
				}
			}

			break;
		}

		if ( !SDL_window ) {
			continue;
		}

		if ( !SDL_glContext ) {
			SDL_DestroyWindow( SDL_window );
			SDL_window = NULL;
			continue;
		}

		if( fullscreen )
		{
			SDL_DisplayMode desiredMode;

			switch( testColorBits )
			{
				case 16: desiredMode.format = SDL_PIXELFORMAT_RGB565; break;
				case 24: desiredMode.format = SDL_PIXELFORMAT_RGB24;  break;
				default: ri.Printf( PRINT_DEVELOPER, "testColorBits is %d, can't fullscreen\n", testColorBits ); continue;
			}

			desiredMode.w = glConfig.vidWidth;
			desiredMode.h = glConfig.vidHeight;
			desiredMode.refresh_rate = glConfig.displayFrequency = ri.Cvar_VariableIntegerValue( "r_displayRefresh" );
			desiredMode.driverdata = NULL;

			if( SDL_SetWindowDisplayMode( SDL_window, &desiredMode ) < 0 )
			{
				ri.Printf( PRINT_DEVELOPER, "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
				continue;
			}
		}

		SDL_SetWindowIcon( SDL_window, icon );

		qglClearColor( 0, 0, 0, 1 );
		qglClear( GL_COLOR_BUFFER_BIT );
		SDL_GL_SwapWindow( SDL_window );

		if( SDL_GL_SetSwapInterval( r_swapInterval->integer ) == -1 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError( ) );
		}

		SDL_GL_GetAttribute( SDL_GL_RED_SIZE, &realColorBits[0] );
		SDL_GL_GetAttribute( SDL_GL_GREEN_SIZE, &realColorBits[1] );
		SDL_GL_GetAttribute( SDL_GL_BLUE_SIZE, &realColorBits[2] );
		SDL_GL_GetAttribute( SDL_GL_DEPTH_SIZE, &glConfig.depthBits );
		SDL_GL_GetAttribute( SDL_GL_STENCIL_SIZE, &glConfig.stencilBits );

		glConfig.colorBits = realColorBits[0] + realColorBits[1] + realColorBits[2];

		ri.Printf( PRINT_ALL, "Using %d color bits, %d depth, %d stencil display.\n",
				glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
		break;
	}

	SDL_FreeSurface( icon );

	if( !SDL_window )
	{
		ri.Printf( PRINT_ALL, "Couldn't get a visual\n" );
		return RSERR_INVALID_MODE;
	}

	SDL_ShowWindow( SDL_window );

	GLimp_DetectAvailableModes();

	glstring = (char *) qglGetString (GL_RENDERER);
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glstring );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean gl3Core)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (SDL_Init(SDL_INIT_VIDEO) != 0)
		{
			ri.Printf( PRINT_ALL, "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver( );
		ri.Printf( PRINT_ALL, "SDL using driver \"%s\"\n", driverName );
		ri.Cvar_Set( "r_sdlDriver", driverName );
	}

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}
	
	err = GLimp_SetMode(mode, fullscreen, noborder, gl3Core);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}


/*
===============
GLimp_InitExtensions
===============
*/
static void GLimp_InitExtensions( qboolean fixedFunction )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "* IGNORING OPENGL EXTENSIONS *\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	glConfig.textureCompression = TC_NONE;

	// GL_EXT_texture_compression_s3tc
	if ( ( QGLES_VERSION_ATLEAST( 2, 0 ) || GLimp_ExtensionSupported( "GL_ARB_texture_compression" ) ) &&
	     GLimp_ExtensionSupported( "GL_EXT_texture_compression_s3tc" ) )
	{
		if ( r_ext_compressed_textures->value )
		{
			glConfig.textureCompression = TC_S3TC_ARB;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_compression_s3tc\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_compression_s3tc\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_compression_s3tc not found\n" );
	}

	// GL_S3_s3tc ... legacy extension before GL_EXT_texture_compression_s3tc.
	if (glConfig.textureCompression == TC_NONE)
	{
		if ( GLimp_ExtensionSupported( "GL_S3_s3tc" ) )
		{
			if ( r_ext_compressed_textures->value )
			{
				glConfig.textureCompression = TC_S3TC;
				ri.Printf( PRINT_ALL, "...using GL_S3_s3tc\n" );
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_S3_s3tc\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_S3_s3tc not found\n" );
		}
	}

	// OpenGL 1 fixed function pipeline
	if ( fixedFunction )
	{
		// GL_EXT_texture_env_add
		glConfig.textureEnvAddAvailable = qfalse;
		if ( GLimp_ExtensionSupported( "GL_EXT_texture_env_add" ) )
		{
			if ( r_ext_texture_env_add->integer )
			{
				glConfig.textureEnvAddAvailable = qtrue;
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
			}
			else
			{
				glConfig.textureEnvAddAvailable = qfalse;
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );
		}

		// GL_ARB_multitexture
		qglMultiTexCoord2fARB = NULL;
		qglActiveTextureARB = NULL;
		qglClientActiveTextureARB = NULL;
		if ( GLimp_ExtensionSupported( "GL_ARB_multitexture" ) )
		{
			if ( r_ext_multitexture->value )
			{
				qglMultiTexCoord2fARB = GLimp_GetProcAddress( "glMultiTexCoord2fARB" );
				qglActiveTextureARB = GLimp_GetProcAddress( "glActiveTextureARB" );
				qglClientActiveTextureARB = GLimp_GetProcAddress( "glClientActiveTextureARB" );

				if ( qglActiveTextureARB )
				{
					GLint glint = 0;
					qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &glint );
					glConfig.numTextureUnits = (int) glint;
					if ( glConfig.numTextureUnits > 1 )
					{
						ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
					}
					else
					{
						qglMultiTexCoord2fARB = NULL;
						qglActiveTextureARB = NULL;
						qglClientActiveTextureARB = NULL;
						ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
					}
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );
		}

		// GL_EXT_compiled_vertex_array
		//
		// Declined on gl4es. This was first adopted on evidence that turned out
		// to be worthless - it was measured against a gl4es that could not draw
		// a triangle at all - so it is kept on its own merits, not on that.
		//
		// The merits are thin but real: the extension is a 1997 hint about
		// re-transforming shared vertices between draws, gl4es's glLockArrays
		// only records first and count and sets a flag, and it re-uploads the
		// arrays per batch regardless. There is nothing here to win.
		//
		// This no longer decides which route R_DrawElements takes. It used to,
		// by accident: a NULL qglLockArraysEXT sent every surface down the
		// glBegin/glArrayElement path, which is not a path the reference has
		// ever run. That renderer now asks for glDrawElements under gl4es
		// outright, the way RTCWQuest does under HAVE_GLES.
		if ( !usingGL4ES && GLimp_ExtensionSupported( "GL_EXT_compiled_vertex_array" ) )
		{
			if ( r_ext_compiled_vertex_array->value )
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
				qglLockArraysEXT = ( void ( APIENTRY * )( GLint, GLint ) ) GLimp_GetProcAddress( "glLockArraysEXT" );
				qglUnlockArraysEXT = ( void ( APIENTRY * )( void ) ) GLimp_GetProcAddress( "glUnlockArraysEXT" );
				if (!qglLockArraysEXT || !qglUnlockArraysEXT)
				{
					ri.Error (ERR_FATAL, "bad getprocaddress");
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
		}
	}

	textureFilterAnisotropic = qfalse;
	if ( GLimp_ExtensionSupported( "GL_EXT_texture_filter_anisotropic" ) )
	{
		if ( r_ext_texture_filter_anisotropic->integer ) {
			qglGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, (GLint *)&maxAnisotropy );
			if ( maxAnisotropy <= 0 ) {
				ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not properly supported!\n" );
				maxAnisotropy = 0;
			}
			else
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_filter_anisotropic (max: %i)\n", maxAnisotropy );
				textureFilterAnisotropic = qtrue;
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not found\n" );
	}

	haveClampToEdge = qfalse;
	if ( QGL_VERSION_ATLEAST( 1, 2 ) || QGLES_VERSION_ATLEAST( 1, 0 ) || GLimp_ExtensionSupported( "GL_SGIS_texture_edge_clamp" ) )
	{
		ri.Printf( PRINT_ALL, "...using GL_SGIS_texture_edge_clamp\n" );
		haveClampToEdge = qtrue;
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_SGIS_texture_edge_clamp not found\n" );
	}
}

#define R_MODE_FALLBACK 3 // 640 * 480

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
void GLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_DEVELOPER, "Glimp_Init( )\n" );

	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );
	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );
#ifdef __ANDROID__
	// There is no desktop GL to fall back to, so don't leave it to detection
	r_preferOpenGLES = ri.Cvar_Get( "r_preferOpenGLES", "1", CVAR_ARCHIVE | CVAR_LATCH );
#else
	r_preferOpenGLES = ri.Cvar_Get( "r_preferOpenGLES", "-1", CVAR_ARCHIVE | CVAR_LATCH );
#endif

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	ri.Sys_GLimpInit( );

	// Create the window and set up the context
	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer, fixedFunction))
		goto success;

	// Try again, this time in a platform specific "safe mode"
	ri.Sys_GLimpSafeInit( );

	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse, fixedFunction))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );

		if(GLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse, fixedFunction))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );

success:
	// These values force the UI to disable driver selection
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	// Only using SDL_SetWindowBrightness to determine if hardware gamma is supported
	glConfig.deviceSupportsGamma = !r_ignorehwgamma->integer &&
		SDL_SetWindowBrightness( SDL_window, 1.0f ) >= 0;

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (char *) qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *) qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	if (*glConfig.renderer_string && glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] == '\n')
		glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (char *) qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );

	// manually create extension list if using OpenGL 3
	if ( qglGetStringi )
	{
		int i, numExtensions, extensionLength, listLength;
		const char *extension;

		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		listLength = 0;

		for ( i = 0; i < numExtensions; i++ )
		{
			extension = (char *) qglGetStringi( GL_EXTENSIONS, i );
			extensionLength = strlen( extension );

			if ( ( listLength + extensionLength + 1 ) >= sizeof( glConfig.extensions_string ) )
				break;

			if ( i > 0 ) {
				Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), " " );
				listLength++;
			}

			Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), extension );
			listLength += extensionLength;
		}
	}
	else
	{
		Q_strncpyz( glConfig.extensions_string, (char *) qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );
	}

	// initialize extensions
	GLimp_InitExtensions( fixedFunction );

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init( SDL_window );
}


/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
static qboolean glimp_presentsToWindow = qtrue;

void GLimp_SetPresentsToWindow( qboolean presents )
{
	glimp_presentsToWindow = presents;
}

void GLimp_EndFrame( void )
{
	// don't flip if drawing to front buffer, or if the window is not what the
	// viewer is actually looking at
	if ( glimp_presentsToWindow && Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
	{
		SDL_GL_SwapWindow( SDL_window );
	}

	if( r_fullscreen->modified )
	{
		int         fullscreen;
		qboolean    needToToggle;
		qboolean    sdlToggled = qfalse;

		// Find out the current state
		fullscreen = !!( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_FULLSCREEN );

		if( r_fullscreen->integer && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
			ri.Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
		}

		// Is the state we want different from the current state?
		needToToggle = !!r_fullscreen->integer != fullscreen;

		if( needToToggle )
		{
			sdlToggled = SDL_SetWindowFullscreen( SDL_window, r_fullscreen->integer ) >= 0;

			// SDL_WM_ToggleFullScreen didn't work, so do it the slow way
			if( !sdlToggled )
				ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");

			ri.IN_Restart( );
		}

		r_fullscreen->modified = qfalse;
	}
}
