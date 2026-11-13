/* PSP main menu layout and action names rendered using the PS2 GS.
 * The geometry and sprite indices come from actual PSP menu .txt files via
 * make_menu_ps2.py. Nonported submenus display a visible status screen.
 */
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <3ds.h>
#include <gsKit.h>
#include <dmaKit.h>
#include "src/c2d_ps2.h"
#include "src/ctru_ps2_input.h"
#include "menu_layout.h"

extern const unsigned char gd_ps2_ui2_begin[];
extern const unsigned char gd_ps2_ui2_end[];

static void sprite(C2D_SpriteSheet sheet, int index,
                   float center_x, float center_y, float scale_x,
                   float scale_y, float rotation)
{
    C2D_Image image = C2D_SpriteSheetGetImage(sheet, index);
    if (!image.tex || !image.subtex || !image.subtex->width) return;
    C2D_Sprite obj;
    C2D_SpriteFromImage(&obj, image);
    C2D_SpriteSetCenter(&obj, 0.5f, 0.5f);
    C2D_SpriteSetScale(&obj, scale_x, scale_y);
    C2D_SpriteSetPos(&obj, center_x, center_y);
    C2D_SpriteSetRotation(&obj, rotation);
    C2D_DrawSprite(&obj);
}

static int move_focus(int current, int dx, int dy)
{
    float best_score = 1e9f;
    int best = current;
    const Ps2MenuEntry *from = &ps2_menu_entries[current];
    for (size_t i=0; i<PS2_MENU_ENTRY_COUNT; ++i) {
        const Ps2MenuEntry *to = &ps2_menu_entries[i];
        if (!to->is_button || (int)i == current) continue;
        float along = (to->x - from->x)*dx + (to->y - from->y)*dy;
        if (along <= 0) continue;
        float sideways = fabsf((to->x - from->x)*dy -
                               (to->y - from->y)*dx);
        float score = along + sideways * 2.0f;
        if (score < best_score) { best = (int)i; best_score = score; }
    }
    return best;
}

static const unsigned char uppercase_glyph[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {15,16,16,16,16,16,15}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {15,16,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

static void draw_label(const char *str, float x, float y, float pixel, u32 color)
{
    for (const char *p=str; *p; ++p) {
        int ch = *p;
        if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
        if (ch == '_') ch = ' ';
        if (ch >= 'A' && ch <= 'Z') {
            for (int row=0; row<7; row++)
                for (int col=0; col<5; col++)
                    if (uppercase_glyph[ch-'A'][row] & (16 >> col))
                        C2D_DrawRectSolid(x+col*pixel, y+row*pixel, 0,
                                          pixel, pixel, color);
        }
        x += pixel * 6;
    }
}

int main(void)
{
    GSGLOBAL *gs;
    C2D_SpriteSheet ui;
    C3D_RenderTarget *top, *bot;
    unsigned frames = 0;
    int selected = 1; /* First actionable element is the play button. */
    const char *active_action = NULL;
    bool alternate_title = false;

    sceSifInitRpc(0);
    PS2_InputInit();
    gs = gsKit_init_global();
    if (!gs) return 1;
    gs->Mode = GS_MODE_NTSC;
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->PrimAlphaEnable = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    /* The demo uses T8 sprites with full and fractional CLUT alpha.
     * Disable the GS alpha *test* separately from depth testing: blending
     * stays enabled and transparent texels remain governed by CLUT alpha. */
    gsKit_set_test(gs, GS_ATEST_OFF);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    gsKit_set_clamp(gs, GS_CMODE_CLAMP);
    gsKit_set_texfilter(gs, GS_FILTER_LINEAR);

    PS2_C2D_Attach(gs);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(512)) return 2;
    PS2_C2D_SetMergedBottom(PS2_MENU_MERGE_X,
                             PS2_MENU_MERGE_Y, PS2_MENU_MERGE_SCALE);
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!top || !bot) return 2;
    ui = C2D_SpriteSheetLoadFromMem(gd_ps2_ui2_begin,
                                    (size_t)(gd_ps2_ui2_end - gd_ps2_ui2_begin));
    if (!ui) {
        printf("PS2: unable to load original menu atlas\n");
        return 2;
    }
    printf("Geometry Dash PS2: %u selected menu sprites loaded\n",
           (unsigned)C2D_SpriteSheetCount(ui));
    printf("D-pad navigate; Cross activate; Circle back; Select alternate title\n");

    for (;;) {
        hidScanInput();
        unsigned pressed = hidKeysDown();
        if (active_action) {
            if (pressed & (KEY_B | KEY_START)) active_action = NULL;
        } else {
            if (pressed & KEY_DLEFT) selected = move_focus(selected, -1, 0);
            if (pressed & KEY_DRIGHT) selected = move_focus(selected, 1, 0);
            if (pressed & KEY_DUP) selected = move_focus(selected, 0, -1);
            if (pressed & KEY_DDOWN) selected = move_focus(selected, 0, 1);
            if (pressed & KEY_SELECT) alternate_title = !alternate_title;
            for (size_t i=0; i<PS2_MENU_ENTRY_COUNT; i++) {
                if (!ps2_menu_entries[i].is_button) continue;
                if ((int)i == selected && (pressed & KEY_A)) {
                    active_action = ps2_menu_entries[i].action;
                    break;
                }
                if ((ps2_menu_entries[i].shortcut & pressed) &&
                    (pressed & ~(KEY_A | KEY_START))) {
                    selected = (int)i;
                    active_action = ps2_menu_entries[i].action;
                    break;
                }
                if ((pressed & KEY_START) && (ps2_menu_entries[i].shortcut & KEY_START)) {
                    selected = (int)i;
                    active_action = ps2_menu_entries[i].action;
                    break;
                }
            }
            if (active_action) printf("Action selected: %s (pending screen port)\n", active_action);
        }

        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return 3;
        C2D_SceneBegin(top);
        C2D_TargetClear(top, C2D_Color32(8, 14, 36, 255));
        C2D_ViewReset();
        C2D_DrawRectSolid(0, 0, 0, 400, 240, C2D_Color32(14, 40, 92, 255));
        C2D_DrawRectSolid(0, 200, 0, 400, 40, C2D_Color32(20, 86, 115, 255));
        C2D_DrawRectSolid(0, 196, 0, 400, 4, C2D_Color32(90, 212, 235, 255));

        if (!active_action) {
            int last_screen = GFX_TOP;
            for (size_t i=0; i<PS2_MENU_ENTRY_COUNT; i++) {
                const Ps2MenuEntry *entry = &ps2_menu_entries[i];
                if (entry->screen != last_screen) {
                    last_screen = entry->screen;
                    C2D_SceneBegin(last_screen == GFX_BOTTOM ? bot : top);
                }
                float scale = ((int)i == selected) ? 1.08f : 1.0f;
                float bounce = ((int)i == selected) ? sinf(frames * 0.08f) * 1.5f : 0;
                int index = entry->sprite;
                if (alternate_title && entry->tag[0] &&
                    !strcmp(entry->tag, "title")) index = PS2_MENU_ALT_TITLE_SPRITE;
                sprite(ui, index, entry->x, entry->y + bounce,
                       entry->sx * scale, entry->sy * scale, 0);
            }
        } else {
            C2D_DrawRectSolid(28, 62, 0, 344, 125, C2D_Color32(23, 42, 83, 255));
            C2D_DrawRectSolid(34, 69, 0, 332, 110, C2D_Color32(46, 91, 147, 255));
            sprite(ui, ps2_menu_entries[selected].sprite, 200, 111, 0.65f, 0.65f, 0);
            draw_label(active_action, 112, 145, 1.5f,
                       C2D_Color32(255, 255, 255, 255));
            draw_label("NOT YET PORTED", 129, 165, 1.0f,
                       C2D_Color32(245, 200, 110, 255));
        }
        frames++;
        C3D_FrameEnd(0);
    }
}
