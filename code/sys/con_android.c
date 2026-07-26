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

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"
#include "sys_android.h"

#include <pthread.h>
#include <unistd.h>

/*
There is no terminal to talk to, so the console goes to logcat instead.

Engine output arrives through CON_Print and is written directly. Anything
that bypasses it and writes to stdout or stderr - the signal handler, the
C library, third party code - would otherwise be discarded by Android, so
those two descriptors are pumped into logcat as well. Losing that output
is worse than it sounds: a diagnostic that never appears is indistinguishable
from one that was never reached.
*/

/*
==================
CON_FlushLine

logcat is line oriented, Com_Printf is not, so lines are assembled here and
emitted one at a time.
==================
*/
static char con_lineBuffer[1024];
static size_t con_lineLength;

static void CON_FlushLine(void)
{
	if (!con_lineLength) {
		return;
	}

	con_lineBuffer[con_lineLength] = '\0';
	Sys_AndroidLog(con_lineBuffer);
	con_lineLength = 0;
}

/*
==================
CON_StdioPump

Reads whatever was written to stdout/stderr and re-emits it, one line per
log entry.
==================
*/
static int con_stdioPipe[2] = {-1, -1};
static pthread_t con_stdioThread;

static void *CON_StdioPump(void *arg)
{
	char line[1024];
	size_t length = 0;
	char c;

	while (read(con_stdioPipe[0], &c, 1) == 1) {
		if (c == '\n' || length == sizeof(line) - 1) {
			line[length] = '\0';
			Sys_AndroidLog(line);
			length = 0;
		} else if (c != '\r') {
			line[length++] = c;
		}
	}

	return NULL;
}

/*
==================
CON_Init
==================
*/
void CON_Init(void)
{
	if (pipe(con_stdioPipe) == -1) {
		Sys_AndroidLog("Failed to create the stdio pipe; stdout and stderr will be lost");
		return;
	}

	// Unbuffered, so output shows up when it is written rather than when the
	// process happens to flush - which on a crash is never.
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	dup2(con_stdioPipe[1], STDOUT_FILENO);
	dup2(con_stdioPipe[1], STDERR_FILENO);

	if (pthread_create(&con_stdioThread, NULL, CON_StdioPump, NULL) != 0) {
		Sys_AndroidLog("Failed to start the stdio pump; stdout and stderr will be lost");
		return;
	}

	pthread_detach(con_stdioThread);
}

/*
==================
CON_Shutdown
==================
*/
void CON_Shutdown(void)
{
	CON_FlushLine();
}

/*
==================
CON_Input
==================
*/
char *CON_Input(void)
{
	return NULL;
}

/*
==================
CON_Print
==================
*/
void CON_Print(const char *msg)
{
	while (*msg) {
		if (Q_IsColorString(msg)) {
			// logcat has no colours, so drop the escape rather than print it
			msg += 2;
			continue;
		}

		if (*msg == '\n') {
			CON_FlushLine();
		} else if (*msg != '\r') {
			con_lineBuffer[con_lineLength++] = *msg;

			if (con_lineLength == sizeof(con_lineBuffer) - 1) {
				CON_FlushLine();
			}
		}

		msg++;
	}
}
