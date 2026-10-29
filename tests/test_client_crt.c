/* Engine-owned offscreen SDL surface; no window, user input or OS capture. */
int q2_test_crt_client_main(int argc, char **argv);
#define main q2_test_crt_client_main
#include "../src/client/main.c"
#undef main

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
#include "client_fixture.h"

static void presentation(void)
{
    client *c = calloc(1, sizeof(*c));
    SDL_Surface *target = SDL_CreateSurface(800, 600, SDL_PIXELFORMAT_ABGR8888);
    SDL_Surface *read, *rgba;
    q2_settings_blob settings;
    psx_framebuffer *back;
    int x, y;
    if (!c || !target) exit(2);
    q2_menu_settings_defaults(&c->settings);
    q2_menu_init(&c->menu, &c->settings, 248);
    CHECK(c->settings.v[Q2_SET_CRT] == 0);
    CHECK(c->settings.v[Q2_SET_BLOOM] == 0);
    CHECK(q2_screen_init(&c->screen, Q2_VIDEO_PAL) == Q2_OK);
    c->width = 512; c->height = 248; c->fit = Q2_SCREEN_FIT_FULL_4_3;
    c->renderer = SDL_CreateSoftwareRenderer(target);
    c->texture = SDL_CreateTexture(c->renderer, SDL_PIXELFORMAT_XBGR1555,
                                    SDL_TEXTUREACCESS_STREAMING, 512, 248);
    if (!c->renderer || !c->texture) exit(2);
    SDL_SetTextureScaleMode(c->texture, SDL_SCALEMODE_NEAREST);
    back = q2_screen_back(&c->screen);
    for (y = 0; y < back->height; y++) for (x = 0; x < back->width; x++)
        back->px[y*back->width+x] = psx_rgb555(x%256, y%256, (x+y)%256);
    c->settings.v[Q2_SET_CRT] = 1;
    client_present(c);
    CHECK(c->crt_presented && c->crt.width == 800 && c->crt.height == 600);
    read = SDL_RenderReadPixels(c->renderer, NULL);
    rgba = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888) : NULL;
    CHECK(rgba != NULL);
    if (rgba) for (y = 0; y < 600; y++)
        CHECK(!memcmp((u8 *)rgba->pixels + y*rgba->pitch, c->crt.pixels+y*800, 800*sizeof(u32)));
    SDL_DestroySurface(rgba); SDL_DestroySurface(read);
    c->settings.v[Q2_SET_BLOOM] = 1;
    client_settings_pack(c, &settings);
    c->settings.v[Q2_SET_CRT] = 0;
    CHECK(client_settings_unpack(c, &settings));
    CHECK(c->settings.v[Q2_SET_CRT] == 1);
    CHECK(c->settings.v[Q2_SET_BLOOM] == 1);
    {
        q2_settings_blob previous = settings;
        memmove(&previous.value[2+Q2_SET_BLOOM], &previous.value[2+Q2_SET_COUNT],
                  Q2_MP_SETTINGS_SETUP_WORDS*sizeof(s16));
        previous.value[0] = 1;
        previous.value[1] = Q2_SET_BLOOM;
        previous.count = Q2_MP_SETTINGS_HEADER_WORDS + Q2_SET_BLOOM + Q2_MP_SETTINGS_SETUP_WORDS;
        CHECK(client_settings_unpack(c, &previous));
        CHECK(c->settings.v[Q2_SET_CRT] == 1 && c->settings.v[Q2_SET_BLOOM] == 0);
    }
    /* Remove the appended setting to model a card written by the old client. */
    memmove(&settings.value[2+Q2_SET_CRT], &settings.value[2+Q2_SET_COUNT],
              Q2_MP_SETTINGS_SETUP_WORDS*sizeof(s16));
    settings.value[0] = 1;
    settings.value[1] = Q2_SET_CRT;
    settings.count = Q2_MP_SETTINGS_HEADER_WORDS + Q2_SET_CRT + Q2_MP_SETTINGS_SETUP_WORDS;
    CHECK(client_settings_unpack(c, &settings));
    CHECK(c->settings.v[Q2_SET_CRT] == 0);
    CHECK(c->settings.v[Q2_SET_BLOOM] == 0);
    CHECK(client_display_texture(c, q2_screen_front(&c->screen), 800, 600) == c->texture);
    CHECK(!c->crt_presented && !c->crt.data && !c->crt_texture);
    c->settings.v[Q2_SET_CRT] = 1;
    CHECK(client_display_texture(c, q2_screen_front(&c->screen), 853, 479) == c->crt_texture);
    CHECK(c->crt_presented && c->crt.width == 853 && c->crt.height == 479);
    q2_crt_free(&c->crt);
    SDL_DestroyTexture(c->crt_texture); SDL_DestroyTexture(c->texture);
    SDL_DestroyRenderer(c->renderer); SDL_DestroySurface(target);
    q2_screen_free(&c->screen);
    free(c);
}

static void bloom_capture(client *c, const char *output, const char *name)
{
    int w=1024,h=768,x,y,protected_pixels=0,changed=0,wrong=0;
    char path[1024];
    const psx_framebuffer *front;
    const u8 *mask;
    u8 *on=malloc((size_t)w*h*4), *off=malloc((size_t)w*h*4);
    if (!on || !off) exit(2);
    c->settings.v[Q2_SET_CRT]=0;
    c->settings.v[Q2_SET_BLOOM]=1;
    client_frame(c);
    front=q2_screen_front(&c->screen);
    mask=c->screen.ui_mask[front-c->screen.buf];
    CHECK(mask != NULL);
    snprintf(path,sizeof(path),"%s/%s-bloom.ppm",output,name);
    CHECK(client_display_screenshot(c,path) == Q2_OK);
    CHECK(c->gl_display && !c->crt.data);
    CHECK(q2_gl_read(c->gl_display,on));
    c->settings.v[Q2_SET_BLOOM]=0;
    snprintf(path,sizeof(path),"%s/%s-off.ppm",output,name);
    CHECK(client_display_screenshot(c,path) == Q2_OK);
    CHECK(q2_gl_read(c->gl_display,off));
    if (mask) for(y=0;y<h;y++) for(x=0;x<w;x++) {
        int sx=(x*2+1)*front->width/(2*w), sy=(y*2+1)*front->height/(2*h);
        bool different=memcmp(on+(y*w+x)*4,off+(y*w+x)*4,4)!=0;
        if(mask[sy*front->width+sx]) { protected_pixels++; if(different)wrong++; }
        else if(different)changed++;
    }
    printf("%s bloom: %d protected UI pixels, %d modified UI pixels, %d changed scene pixels\n",
           name,protected_pixels,wrong,changed);
    CHECK(protected_pixels>100 && wrong==0 && changed>100);
    c->settings.v[Q2_SET_BLOOM]=1; c->settings.v[Q2_SET_CRT]=1;
    snprintf(path,sizeof(path),"%s/%s-crt-bloom.ppm",output,name);
    CHECK(client_display_screenshot(c,path) == Q2_OK);
    free(on); free(off);
}

/* Disc-backed regression: the decoded film replaces gameplay before display
 * effects, retains its letterbox, and must never reuse the scene bloom buffer. */
static void film_capture(client *c, const char *output)
{
    const char *pal[] = { "TAKE1BP.STX", "ROGUEINP.STX", "OUTRO1P.STX" };
    const char *ntsc[] = { "TAKE1B.STX", "ROGUEIN1.STX", "OUTRO1.STX" };
    const char *labels[] = { "intro", "opening", "outro" };
    const size_t bytes = 1024u*768u*4u;
    u8 *off = malloc(bytes), *on = malloc(bytes), *again = malloc(bytes);
    q2_crt reference = {0};
    int film, i;
    char path[1024];
    if (!off || !on || !again) exit(2);
    for (film=0; film<3; film++) {
        const psx_framebuffer *front;
        int changed=0, mismatch=0, bars=0;
        bool started=client_film_start(c, c->build.video==Q2_VIDEO_PAL ? pal[film] : ntsc[film]);
        CHECK(started);
        if (!started) continue;
        for (i=0;i<240;i++) client_film_tick(c,1.0f/30.0f);
        CHECK(c->film_open && c->film_have_frame && c->film_frames>0);
        c->settings.v[Q2_SET_BLOOM]=1;
        c->settings.v[Q2_SET_CRT]=0;
        client_frame(c);
        front=q2_screen_front(&c->screen);
        CHECK(!c->screen.scene[front-c->screen.buf].px);
        snprintf(path,sizeof(path),"%s/%s-off.ppm",output,labels[film]);
        CHECK(client_display_screenshot(c,path)==Q2_OK);
        CHECK(q2_gl_read(c->gl_display,off));
        c->settings.v[Q2_SET_CRT]=1;
        snprintf(path,sizeof(path),"%s/%s-on.ppm",output,labels[film]);
        CHECK(client_display_screenshot(c,path)==Q2_OK);
        CHECK(q2_gl_read(c->gl_display,on));
        CHECK(!c->crt.data); /* GPU route, not the CPU fallback. */
        c->settings.v[Q2_SET_BLOOM]=0;
        CHECK(client_gpu_render(c,front,1024,768));
        CHECK(q2_gl_read(c->gl_display,again));
        CHECK(!memcmp(on,again,bytes));
        CHECK(q2_crt_render(&reference,front,1024,768)==Q2_OK);
        for (i=0;i<1024*768;i++) {
            int channel;
            if (memcmp(off+i*4,on+i*4,3)) changed++;
            for(channel=0;channel<3;channel++) {
                int ref=(reference.pixels[i]>>(channel*8))&255;
                if(abs((int)on[i*4+channel]-ref)>1) mismatch++;
                if ((i<1024*16 || i>=1024*(768-16)) && on[i*4+channel]) bars++;
            }
        }
        CHECK(changed>1000 && mismatch==0 && bars==0);
        c->crt_cpu=true;
        snprintf(path,sizeof(path),"%s/%s-cpu.ppm",output,labels[film]);
        CHECK(client_display_screenshot(c,path)==Q2_OK);
        CHECK(c->crt.width==1024 && c->crt.height==768);
        CHECK(!memcmp(c->crt.pixels,reference.pixels,bytes));
        c->crt_cpu=false;
        q2_crt_free(&c->crt);
        printf("%s cinematic: %d changed pixels, %d GPU/reference mismatches, %d nonblack letterbox channels\n",
               labels[film],changed,mismatch,bars);
        client_film_stop(c);
    }
    q2_crt_free(&reference);
    free(c->film_rgb); c->film_rgb=NULL;
    free(off); free(on); free(again);
}

static void menu_capture(const char *disc, const char *output)
{
    client *c = open_client(disc);
    char path[1024];
    int i;
    c->capture_scale = 2;
    c->fit = Q2_SCREEN_FIT_FULL_4_3;
    c->icons_ready = q2_icon_tables_load(&c->icons,c->disc,&c->build) == Q2_OK;
    CHECK(c->icons_ready);
    for (i=0;i<Q2_MP_MAX_PLAYERS;i++) {
        q2_statusbar_init(&c->sbar[i],&c->icons,1);
        q2_statusbar_set_palettes(&c->sbar[i],&c->hud_tables);
    }
    CHECK(client_load_zone(c, "BASE1", 0));
    q2_loading_close(&c->loading);
    bloom_capture(c,output,"gameplay");
    q2_menu_open(&c->menu);
    q2_menu_goto(&c->menu, Q2_PAGE_VIDEO);
    bloom_capture(c,output,"menu");
    c->menu.cursor = 1;
    c->settings.v[Q2_SET_CRT] = 1;
    client_frame(c);
    snprintf(path, sizeof(path), "%s/menu.ppm", output);
    CHECK(client_display_screenshot(c, path) == Q2_OK);
    film_capture(c,output);
    q2_gl_destroy(c->gl_display);
    c->gl_display = NULL;
    q2_crt_free(&c->crt);
    q2_icon_tables_free(&c->icons);
    close_client(c);
}

static void gpu_presentation(void)
{
    client *c = calloc(1, sizeof(*c));
    u8 *rgba;
    q2_crt reference = {0};
    int i;
    if (!c) exit(2);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { free(c); return; }
    q2_gl_attributes();
    c->window = SDL_CreateWindow("CRT client test", 640, 480,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (c->window) c->gl_display = q2_gl_create(c->window);
    if (!c->gl_display) {
        if (c->window) SDL_DestroyWindow(c->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO); free(c); return;
    }
    c->headless = true;
    c->fit = Q2_SCREEN_FIT_FULL_4_3;
    c->width = 512; c->height = 248;
    q2_menu_settings_defaults(&c->settings);
    CHECK(q2_screen_init(&c->screen, Q2_VIDEO_PAL) == Q2_OK);
    psx_fb_clear(q2_screen_back(&c->screen), psx_rgb555(96,160,224));
    c->settings.v[Q2_SET_CRT] = 1;
    client_present(c);
    CHECK(c->crt_presented && !c->crt.data && !c->renderer);
    CHECK(q2_crt_render(&reference,q2_screen_front(&c->screen),640,480) == Q2_OK);
    rgba = malloc(640*480*4);
    if (!rgba) exit(2);
    CHECK(q2_gl_read(c->gl_display,rgba));
    for (i = 0; i < 640*480*3; i++) {
        int p = i/3, channel = i%3;
        int difference = (int)rgba[p*4+channel] - (int)((reference.pixels[p]>>(8*channel))&255);
        if (abs(difference) > 1) { CHECK(false); break; }
    }
    c->settings.v[Q2_SET_CRT] = 0;
    client_present(c);
    CHECK(!c->crt_presented && !c->crt.data);
    CHECK(SDL_SetWindowSize(c->window,960,540));
    c->settings.v[Q2_SET_CRT] = 1;
    client_present(c);
    CHECK(c->crt_presented && !c->crt.data);
    free(rgba); q2_crt_free(&reference);
    q2_gl_destroy(c->gl_display); SDL_DestroyWindow(c->window);
    q2_screen_free(&c->screen); free(c);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char **argv)
{
    presentation();
    gpu_presentation();
    if (argc == 4 && !strcmp(argv[1], "--headless")) menu_capture(argv[2], argv[3]);
    else if (argc != 1) return 2;
    printf("%d CRT client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
