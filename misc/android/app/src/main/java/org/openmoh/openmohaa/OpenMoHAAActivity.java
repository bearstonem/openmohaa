package org.openmoh.openmohaa;

import org.libsdl.app.SDLActivity;

/**
 * Entry point for the Android build.
 *
 * SDLActivity loads the libraries named below in order, then dlopen()s the last
 * one and calls SDL_main in it - which is the engine's main(), renamed by
 * SDL_main.h. libc++_shared.so and libopenal.so are pulled in automatically as
 * dependencies of libopenmohaa.so, and libgame.so / libcgame.so are dlopen()ed
 * later by the engine itself, so none of them are listed here.
 */
public class OpenMoHAAActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "openmohaa",
        };
    }
}
