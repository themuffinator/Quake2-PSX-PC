/*
 * launcher.h — choose the game's disc inside its own window.
 *
 * Shown when the client starts without --disc. It lists the disc played last
 * (selected), the ones before it, disc images found beside the executable and
 * in its disc/ folder, and every optical drive; Browse opens a controller-ready
 * in-window file picker, and an image dropped on the window joins the list. Whatever is
 * played is remembered (launchcfg.h) and selected again next time.
 *
 * It can draw before a disc is found. When one is available its original PSX
 * menu letterforms are read from QFRONT; the last successful atlas is cached
 * beside launcher.cfg. Optional local cover art lives in launcher/cover.bmp.
 */
#ifndef Q2PSX_LAUNCHER_H
#define Q2PSX_LAUNCHER_H

#include <SDL3/SDL.h>

#include "q2psx.h"
#include "crt_gl.h"

typedef struct q2_launcher_host {
    SDL_Window *window;
    q2_gl_display *display;
} q2_launcher_host;

typedef struct q2_launcher_options {
    int width, height;
    bool headless, cpu;
    const char *shot;
    const char *disc;
} q2_launcher_options;

enum { Q2_LAUNCH_ERROR = -1, Q2_LAUNCH_QUIT, Q2_LAUNCH_PLAY, Q2_LAUNCH_CAPTURE };

/*
 * Run until the player picks a disc or closes the window.
 *
 * PLAY transfers the window AND its GL context to the game, at the same size
 * and position. Other results leave no resources open. A headless capture
 * renders to an engine-owned surface without creating a window or reading input.
 */
int q2_launcher_run(char *out, size_t cap, q2_launcher_host *host,
                    const q2_launcher_options *options);
void q2_launcher_host_close(q2_launcher_host *host);

#endif /* Q2PSX_LAUNCHER_H */
