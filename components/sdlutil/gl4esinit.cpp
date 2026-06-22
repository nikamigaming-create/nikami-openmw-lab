// EGL does not work reliably for feature detection.
// Instead, we initialize gl4es manually.
#ifdef OPENMW_GL4ES_MANUAL_INIT
#include "gl4esinit.h"

// For glHint
#include <GL/gl.h>
#include <dlfcn.h>

extern "C"
{

#include <gl4eshint.h>

    static SDL_Window* gWindow;

    void openmw_gl4es_GetMainFBSize(int* width, int* height)
    {
        SDL_GetWindowSize(gWindow, width, height);
    }

    void openmw_gl4es_init(SDL_Window* window)
    {
        using SetGetProcAddressFn = void (*)(void* (*)(const char*));
        using SetGetMainFBSizeFn = void (*)(void (*)(int*, int*));
        using InitializeGl4esFn = void (*)();

        auto set_getprocaddress_fn = reinterpret_cast<SetGetProcAddressFn>(dlsym(RTLD_DEFAULT, "set_getprocaddress"));
        auto set_getmainfbsize_fn = reinterpret_cast<SetGetMainFBSizeFn>(dlsym(RTLD_DEFAULT, "set_getmainfbsize"));
        auto initialize_gl4es_fn = reinterpret_cast<InitializeGl4esFn>(dlsym(RTLD_DEFAULT, "initialize_gl4es"));

        gWindow = window;
        if (set_getprocaddress_fn)
            set_getprocaddress_fn(SDL_GL_GetProcAddress);
        if (set_getmainfbsize_fn)
            set_getmainfbsize_fn(openmw_gl4es_GetMainFBSize);
        if (initialize_gl4es_fn)
            initialize_gl4es_fn();

        // merge glBegin/glEnd in beams and console
        glHint(GL_BEGINEND_HINT_GL4ES, 1);
        // dxt unpacked to 16-bit looks ugly
        glHint(GL_AVOID16BITS_HINT_GL4ES, 1);
    }

} // extern "C"

#endif // OPENMW_GL4ES_MANUAL_INIT
