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

// The tag every log entry is filed under; `adb logcat -s openmohaa` shows
// only ours.
#define SYS_ANDROID_LOG_TAG "openmohaa"

// Writes a single line to logcat. Safe to call before the engine has started
// and from a signal handler.
void Sys_AndroidLog(const char *message);

// Installs handlers for the fatal signals that report the faulting address and
// program counter. The generic Unix handler cannot: it is installed with
// signal(), so it never sees siginfo, and _Unwind_Backtrace cannot walk out of
// the kernel's signal trampoline - which leaves a crash reported as four frames
// of the handler itself and nothing about where the fault actually happened.
void Sys_AndroidInstallCrashHandler(void);

// Points the install path at the app's external files directory and makes it
// the working directory. Must run before Com_Init, which is where FS_Startup
// reads the install path. A no-op in the dedicated server, which has no
// activity to ask.
void Sys_AndroidSetupPaths(void);
