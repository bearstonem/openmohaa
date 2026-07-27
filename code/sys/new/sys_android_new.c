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

#include "../sys_local.h"
#include "../sys_android.h"

#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#ifndef DEDICATED
#	ifdef USE_INTERNAL_SDL_HEADERS
#		include "SDL.h"
#	else
#		include <SDL.h>
#	endif
#endif

/*
Android counterpart to sys_unix_new.c.

bionic has no <execinfo.h>, so the backtrace is walked with the unwinder
directly and the addresses resolved through dladdr. This matters more here
than on the desktop: tombstoned does not reliably put a backtrace in logcat,
and when it does not, this is the only account of where the engine died.
*/

/*
==================
Sys_AndroidLog
==================
*/
void Sys_AndroidLog(const char *message)
{
	__android_log_write(ANDROID_LOG_INFO, SYS_ANDROID_LOG_TAG, message);
}

/*
==================
Sys_PlatformInit_New
==================
*/
void Sys_PlatformInit_New() {

}

/*
==================
Sys_AndroidSetupPaths
==================
*/
void Sys_AndroidSetupPaths(void)
{
#ifndef DEDICATED
	const char *path = SDL_AndroidGetExternalStoragePath();
	char message[MAX_OSPATH + 64];

	if (!path) {
		Com_sprintf(message, sizeof(message),
			"No external files directory (%s); the game data will not be found",
			SDL_GetError());
		Sys_AndroidLog(message);
		return;
	}

	// The engine resolves relative paths against the working directory, and an
	// Android process starts in "/" where it can neither read nor write.
	if (chdir(path) != 0) {
		Com_sprintf(message, sizeof(message), "Could not enter %s", path);
		Sys_AndroidLog(message);
		return;
	}

	Sys_SetDefaultInstallPath(path);

	Com_sprintf(message, sizeof(message), "Game data directory: %s", path);
	Sys_AndroidLog(message);
#endif
}

/*
==================
Sys_PrepareBackTrace
==================
*/
void Sys_PrepareBackTrace() {
}

/*
==================
Sys_UnwindCallback

Collects the program counter of each frame as the unwinder walks up the
stack.
==================
*/
typedef struct {
	void **frames;
	size_t count;
	size_t max;
} backTraceState_t;

static _Unwind_Reason_Code Sys_UnwindCallback(struct _Unwind_Context *context, void *arg)
{
	backTraceState_t *state = (backTraceState_t *)arg;
	uintptr_t pc = _Unwind_GetIP(context);

	if (!pc) {
		return _URC_NO_REASON;
	}

	if (state->count >= state->max) {
		return _URC_END_OF_STACK;
	}

	state->frames[state->count++] = (void *)pc;
	return _URC_NO_REASON;
}

/*
==================
Sys_PrintBackTrace

Written straight to logcat rather than to stderr: this runs from the signal
handler, and anything still sitting in a buffer when the process aborts is
lost.
==================
*/
void Sys_PrintBackTrace() {
	void *frames[128];
	backTraceState_t state;
	char line[1024];
	size_t i;

	state.frames = frames;
	state.count = 0;
	state.max = ARRAY_LEN(frames);

	_Unwind_Backtrace(Sys_UnwindCallback, &state);

	for (i = 0; i < state.count; i++) {
		Dl_info info;
		const char *module = "?";
		const char *symbol = NULL;
		uintptr_t offset = 0;

		if (dladdr(frames[i], &info) && info.dli_fname) {
			const char *slash = strrchr(info.dli_fname, '/');

			module = slash ? slash + 1 : info.dli_fname;
			symbol = info.dli_sname;

			if (info.dli_saddr) {
				offset = (uintptr_t)frames[i] - (uintptr_t)info.dli_saddr;
			}
		}

		if (symbol) {
			Com_sprintf(line, sizeof(line), "  #%02u  %p  %s  %s+%u",
				(unsigned int)i, frames[i], module, symbol, (unsigned int)offset);
		} else {
			Com_sprintf(line, sizeof(line), "  #%02u  %p  %s",
				(unsigned int)i, frames[i], module);
		}

		Sys_AndroidLog(line);
	}
}

/*
==================
Sys_AndroidDescribeAddress

Names whatever the address falls inside, so a bare pointer becomes a library
and a symbol.
==================
*/
static void Sys_AndroidDescribeAddress(const void *address, char *out, size_t outSize)
{
	Dl_info info;

	if (dladdr(address, &info) && info.dli_fname) {
		const char *slash = strrchr(info.dli_fname, '/');
		const char *module = slash ? slash + 1 : info.dli_fname;

		if (info.dli_sname && info.dli_saddr) {
			Com_sprintf(out, outSize, "%p  %s  %s+%u", address, module, info.dli_sname,
				(unsigned int)((uintptr_t)address - (uintptr_t)info.dli_saddr));
			return;
		}

		Com_sprintf(out, outSize, "%p  %s+%u", address, module,
			(unsigned int)((uintptr_t)address - (uintptr_t)info.dli_fbase));
		return;
	}

	Com_sprintf(out, outSize, "%p  <unmapped>", address);
}

/*
==================
Sys_AndroidCrashHandler

Reports the fault before handing over to the engine's own handler. The register
state is the part that matters: the unwinder stops at the signal frame, so the
program counter carried in the context is the only reliable account of where
the process actually died.
==================
*/
static void Sys_AndroidCrashHandler(int signum, siginfo_t *info, void *contextPtr)
{
	char line[512];
	char described[256];

	Com_sprintf(line, sizeof(line), "Fatal signal %d at address %p", signum,
		info ? info->si_addr : NULL);
	Sys_AndroidLog(line);

#if defined(__aarch64__)
	if (contextPtr) {
		const ucontext_t *context = (const ucontext_t *)contextPtr;

		Sys_AndroidDescribeAddress((const void *)context->uc_mcontext.pc, described, sizeof(described));
		Com_sprintf(line, sizeof(line), "  pc  %s", described);
		Sys_AndroidLog(line);

		// x30 is the link register: where the faulting function would have
		// returned to, which identifies the caller when pc is in a stripped or
		// JIT-generated region.
		Sys_AndroidDescribeAddress((const void *)context->uc_mcontext.regs[30], described, sizeof(described));
		Com_sprintf(line, sizeof(line), "  lr  %s", described);
		Sys_AndroidLog(line);
	}
#endif

	Sys_SigHandler(signum);
}

/*
==================
Sys_AndroidInstallCrashHandler
==================
*/
void Sys_AndroidInstallCrashHandler(void)
{
	static char      signalStack[SIGSTKSZ * 4];
	const int        signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	struct sigaction action;
	stack_t          altStack;
	size_t           i;

	// A stack overflow faults again the moment the handler pushes a frame, and
	// the second fault is delivered as an unrecoverable kill with nothing
	// logged. Give the handler its own stack so it survives to report.
	altStack.ss_sp = signalStack;
	altStack.ss_size = sizeof(signalStack);
	altStack.ss_flags = 0;
	sigaltstack(&altStack, NULL);

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = Sys_AndroidCrashHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&action.sa_mask);

	for (i = 0; i < ARRAY_LEN(signals); i++) {
		sigaction(signals[i], &action, NULL);
	}
}

/*
==============
Sys_DebugPrint
==============
*/
void Sys_DebugPrint(const char* message) {
	Sys_AndroidLog(message);
}

/*
==============
Sys_PumpMessageLoop
==============
*/
void Sys_PumpMessageLoop(void)
{
}

/*
==================
SetNormalThreadPriority
==================
*/
void SetNormalThreadPriority(void)
{
}

/*
==================
SetBelowNormalThreadPriority
==================
*/
void SetBelowNormalThreadPriority(void)
{
}
