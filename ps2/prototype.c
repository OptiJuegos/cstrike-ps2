/*
 * Native PS2 bring-up test for the Geometry Dash port.
 * This is deliberately independent of source/: it verifies gsKit, timing,
 * and the first controller before the compatibility layer is written.
 */
#include <stdio.h>
#include <stdbool.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>

#define STAGE_W 400.0f
#define STAGE_H 240.0f
#define VIEW_SCALE 1.6f
#define VIEW_Y 32.0f
#define GROUND_Y 185.0f
#define PLAYER_X 68.0f
#define PLAYER_SIZE 19.0f
#define JUMP_SPEED -6.6f
#define GRAVITY 0.32f

static char pad_buffer[256] __attribute__((aligned(64)));
static bool pad_ready;
static unsigned previous_buttons;

static void init_pad(void)
{
    int sio = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    int pad = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (sio < 0 || pad < 0) {
        printf("Pad modules not available (%d, %d)\n", sio, pad);
        return;
    }
    if (padInit(0) == 0) {
        printf("padInit failed\n");
        return;
    }
    pad_ready = padPortOpen(0, 0, pad_buffer) != 0;
    if (!pad_ready) printf("padPortOpen failed\n");
}

/* libpad reports pressed buttons as cleared bits. 0 means no input. */
static unsigned read_buttons(void)
{
    struct padButtonStatus state;
    int status;
    if (!pad_ready) return 0;
    status = padGetState(0, 0);
    if (status != PAD_STATE_STABLE && status != PAD_STATE_FINDCTP1)
        return 0;
    if (padRead(0, 0, &state) == 0) return 0;
    return (~(unsigned)state.btns) & 0xFFFFu;
}

static void draw_rect(GSGLOBAL *gs, float x, float y, float w, float h, u64 rgba)
{
    gsKit_prim_sprite(gs, x * VIEW_SCALE, y * VIEW_SCALE + VIEW_Y,
                     (x + w) * VIEW_SCALE, (y + h) * VIEW_SCALE + VIEW_Y,
                     1, rgba);
}

int main(int argc, char **argv)
{
    GSGLOBAL *gs;
    float player_y = GROUND_Y - PLAYER_SIZE;
    float speed_y = 0.0f;
    float ground_scroll = 0.0f;
    bool playing = true;
    unsigned frame = 0;
    const u64 bg = GS_SETREG_RGBAQ(15, 28, 61, 0x80, 0);
    const u64 border = GS_SETREG_RGBAQ(6, 9, 24, 0x80, 0);
    const u64 ground = GS_SETREG_RGBAQ(66, 130, 230, 0x80, 0);
    const u64 player = GS_SETREG_RGBAQ(245, 208, 54, 0x80, 0);
    const u64 white = GS_SETREG_RGBAQ(255, 255, 255, 0x80, 0);
    (void)argc;
    (void)argv;

    sceSifInitRpc(0);
    init_pad();

    gs = gsKit_init_global();
    if (!gs) return 1;
    gs->Mode = GS_MODE_NTSC;
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    printf("Geometry Dash PS2: native display / pad prototype\n");
    printf("Cross: jump, Start: reset, Select: pause\n");

    for (;;) {
        unsigned buttons = read_buttons();
        unsigned down = buttons & ~previous_buttons;
        previous_buttons = buttons;

        if (down & PAD_SELECT) playing = !playing;
        if (down & PAD_START) {
            player_y = GROUND_Y - PLAYER_SIZE;
            speed_y = 0.0f;
            ground_scroll = 0.0f;
            playing = true;
        }

        /* Fixed update is for the NTSC bring-up test only. Real gameplay must
         * keep the engine's own fixed timestep and handle PAL separately. */
        if (playing) {
            if ((down & (PAD_CROSS | PAD_UP)) && player_y >= GROUND_Y - PLAYER_SIZE)
                speed_y = JUMP_SPEED;
            player_y += speed_y;
            speed_y += GRAVITY;
            if (player_y > GROUND_Y - PLAYER_SIZE) {
                player_y = GROUND_Y - PLAYER_SIZE;
                speed_y = 0.0f;
            }
            ground_scroll += 1.5f;
            if (ground_scroll >= 40.0f) ground_scroll -= 40.0f;
        }

        gsKit_clear(gs, border);
        draw_rect(gs, 0, 0, STAGE_W, STAGE_H, bg);
        draw_rect(gs, 0, GROUND_Y, STAGE_W, 2, white);
        draw_rect(gs, 0, GROUND_Y + 2, STAGE_W, STAGE_H - GROUND_Y - 2, ground);
        for (int i = -1; i < 12; ++i)
            draw_rect(gs, 40.0f * i - ground_scroll, GROUND_Y + 16, 17, 4, bg);
        draw_rect(gs, PLAYER_X, player_y, PLAYER_SIZE, PLAYER_SIZE, player);
        /* Small eye to make the sample's square orientation visible. */
        draw_rect(gs, PLAYER_X + 12, player_y + 4, 3, 3, bg);

        /* Marker pulses to show the render loop is alive even without a pad. */
        if ((frame / 30) & 1)
            draw_rect(gs, 385, 9, 6, 6, white);
        ++frame;
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
}
