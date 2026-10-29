/* GUI-subsystem entry point: Explorer never creates a console for the game.
 * Developer mode may allocate one; command-line tools attach only to an
 * existing terminal and preserve pipes/redirection. SDL supplies UTF-8 argv. */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv);

static bool redirected(HANDLE h)
{
    DWORD type;
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    type = GetFileType(h);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

static void bind_output(FILE *stream, HANDLE inherited, bool console)
{
    if (redirected(inherited)) {
        HANDLE copy;
        if (DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(),
                            &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            int fd = _open_osfhandle((intptr_t)copy, _O_TEXT);
            if (fd >= 0) {
                if (freopen("NUL", "w", stream)) _dup2(fd, _fileno(stream));
                _close(fd);
            } else CloseHandle(copy);
        }
    } else if (console) {
        (void)freopen("CONOUT$", "w", stream);
    }
}

static int run_client(int argc, char **argv)
{
    bool developer = false, cli = false, console;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE errors = GetStdHandle(STD_ERROR_HANDLE);
    char log_path[2048] = "";
    int i, result;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--developer")) developer = true;
        if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--version") ||
            !strcmp(argv[i], "-v") || !strcmp(argv[i], "--help") ||
            !strcmp(argv[i], "-h") || !strcmp(argv[i], "--report")) cli = true;
    }
    console = GetConsoleWindow() != NULL;
    if (!console && (developer || cli)) console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!console && developer) console = AllocConsole() != 0;
    if (console || redirected(output) || redirected(errors)) {
        bind_output(stdout, output, console);
        bind_output(stderr, errors, console);
        if (console) { SetConsoleOutputCP(CP_UTF8); setvbuf(stdout, NULL, _IONBF, 0); }
    } else if (!cli) {
        char *dir = SDL_GetPrefPath("", "Q2PSX-PC");
        if (dir) {
            snprintf(log_path, sizeof(log_path), "%sq2psx.log", dir);
            SDL_free(dir);
            if (freopen(log_path, "w", stderr)) {
                setvbuf(stderr, NULL, _IONBF, 0);
                if (freopen("NUL", "w", stdout)) _dup2(_fileno(stderr), _fileno(stdout));
            }
        }
    }
    result = main(argc, argv);
    fflush(stdout);
    fflush(stderr);
    if (result && !cli && !developer) {
        char message[2304];
        snprintf(message, sizeof(message), "Quake II could not start.%s%s",
                 log_path[0] ? "\n\nDetails are saved in:\n" : "\nRun with --developer for details.", log_path);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Quake II", message, NULL);
    }
    return result;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    (void)instance; (void)previous; (void)command; (void)show;
    return SDL_RunApp(0, NULL, run_client, NULL);
}
