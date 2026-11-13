/* PS2 video: owns the GS and its display. ref_gu draws into it through
 * ref_gs/gs_gu.c; nothing is drawn by the CPU. */
#include "common.h"
#include "client.h"
#include "vid_common.h"
#include "platform/platform.h"

#include <gsKit.h>
#include <dmaKit.h>
#include <stdio.h>

/* MainUI inherited the original PSP layout at 480x272. Keeping that logical
 * resolution prevents its menu labels and descriptions from overlapping;
 * gs_gu.c scales it to the GS framebuffer. */
#define PS2_FRAME_W 480
#define PS2_FRAME_H 272

static vidmode_t ps2_mode[] = { { "480x272", PS2_FRAME_W, PS2_FRAME_H } };
static GSGLOBAL *ps2_gs;
void PS2_ScreenLogStop(void);

GSGLOBAL *PS2_GS_Global(void)
{
    return ps2_gs;
}

static int init_gs(void)
{
    if (ps2_gs) return 1;
    /* gsKit only queues the buffer flip: gs_gu.c sends its own packets. */
    ps2_gs = gsKit_init_global_custom(64 * 1024, 16 * 1024);
    if (!ps2_gs) return 0;
    PS2_ScreenLogStop(); /* the debug screen must not write into gsKit's VRAM */
    ps2_gs->Mode = GS_MODE_NTSC;
    ps2_gs->Width = 640;
    ps2_gs->Height = 448;
    ps2_gs->PSM = GS_PSM_CT16;
    ps2_gs->PSMZ = GS_PSMZ_16;
    ps2_gs->DoubleBuffering = GS_SETTING_ON;
    ps2_gs->ZBuffering = GS_SETTING_ON;
    ps2_gs->PrimAlphaEnable = GS_SETTING_OFF;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(ps2_gs);
    gsKit_mode_switch(ps2_gs, GS_ONESHOT);
    /* VRAM holds garbage at boot: black out both buffers before the first frame */
    gsKit_clear(ps2_gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_queue_exec(ps2_gs);
    gsKit_sync_flip(ps2_gs);
    gsKit_clear(ps2_gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_queue_exec(ps2_gs);
    Cvar_Get("ps2_profile_present", "0", FCVAR_ARCHIVE,
        "Print the PS2 frame phase timers every 120 in-game frames");
    printf("[XASH-PS2] STAGE=GS_READY\n");
    return 1;
}

int R_MaxVideoModes(void) { return 1; }
vidmode_t *R_GetVideoMode(int index) { return index == 0 ? &ps2_mode[0] : NULL; }

void GL_UpdateSwapInterval(void) {}
void *GL_GetProcAddress(const char *name) { (void)name; return NULL; }
int GL_SetAttribute(int attribute, int value)
{ (void)attribute; (void)value; return 0; }
int GL_GetAttribute(int attribute, int *value)
{ (void)attribute; (void)value; return 0; }
void GL_SwapBuffers(void) {} /* sceGuSwapBuffers flips */

/* The engine's renderer table asks for these; only ref_soft uses them. */
qboolean SW_CreateBuffer(int width, int height, uint *stride, uint *bpp,
                         uint *red, uint *green, uint *blue)
{
    (void)width; (void)height; (void)stride; (void)bpp;
    (void)red; (void)green; (void)blue;
    return false;
}
void *SW_LockBuffer(void) { return NULL; }
void SW_UnlockBuffer(void) {}

qboolean R_Init_Video(const int type)
{
    if (type != REF_GL) {
        Con_Reportf("[XASH-PS2] Only the GS renderer is supported\n");
        return false;
    }
    if (!init_gs()) return false;
    glw_state.software = false;
    refState.desktopBitsPixel = 16;
    VID_StartupGamma();
    if (!VID_SetMode()) return false;
    ref.dllFuncs.GL_InitExtensions();
    return true;
}

qboolean VID_SetMode(void)
{
    R_SaveVideoMode(PS2_FRAME_W, PS2_FRAME_H, PS2_FRAME_W, PS2_FRAME_H);
    host.renderinfo_changed = false;
    printf("[XASH-PS2] STAGE=VIDEO_MODE 480x272\n");
    return true;
}

rserr_t R_ChangeDisplaySettings(int width, int height, qboolean fullscreen)
{
    (void)width; (void)height; (void)fullscreen;
    return VID_SetMode() ? rserr_ok : rserr_invalid_mode;
}

void R_Free_Video(void)
{
}
