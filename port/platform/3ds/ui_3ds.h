/* The 3DS bottom screen: the room radar, the settings cog and screen
 * (ui_3ds.c). */
#ifndef PORT_UI_3DS_H
#define PORT_UI_3DS_H

typedef struct UiSettings {
    int frame_skip;   /* 0 off, 1 on, 2 auto (on only while the game is slow; the default) */
    int stretch;      /* 1: widescreen (stretched), 0: 4:3 */
    int logging;      /* 1: write recvx_log.txt (off by default) */
    int fast_doors;   /* 1: door and stairs scenes run 3 times as fast */
} UiSettings;
extern UiSettings g_ui;

enum { UI_NONE, UI_SKIP, UI_PICTURE, UI_LOG, UI_ITEMS, UI_MAP, UI_TURN, UI_SAVE, UI_LOAD };

void ui_load_settings(const char *dir);   /* settings.txt; defaults where it has nothing */
int ui_init(void);                        /* after C3D_Init */
void ui_render(void);                     /* inside the frame, before C3D_FrameEnd */
int ui_touch(int x, int y);               /* a touch went down: UI_* changed */
void ui_close_settings(void);
void ui_message(const char *s);            /* a line on the settings screen, for a few seconds */
void ui_radar_tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned rgba);

#endif
