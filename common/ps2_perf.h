/* PS2 per-frame phase timers, read from the EE cycle counter (COP0 Count).
 * Phases nest, so a parent includes its children. Printed by PS2_Perf_EndFrame
 * every 120 in-game frames when ps2_profile_present is 1. */
#ifndef PS2_PERF_H
#define PS2_PERF_H

#include "build.h"

#if XASH_PS2

enum
{
	PS2_PERF_FRAME,   // Host_Frame work
	PS2_PERF_SV,      // Host_ServerFrame
	PS2_PERF_BOTS,    //   pfnStartFrame: game rules and bots
	PS2_PERF_SVENT,   //   entity physics and think
	PS2_PERF_SVSEND,  //   SV_SendClientMessages
	PS2_PERF_CL,      // Host_ClientFrame
	PS2_PERF_CLDLL,   //   client.dll pfnFrame
	PS2_PERF_CLPKT,   //   CL_ReadPackets
	PS2_PERF_CLEMIT,  //   CL_EmitEntities
	PS2_PERF_SCR,     //   SCR_UpdateScreen
	PS2_PERF_VIEW,    //     V_RenderView
	PS2_PERF_WORLD,   //       R_DrawWorld
	PS2_PERF_ENTS,    //       R_DrawEntitiesOnList
	PS2_PERF_HUD,     //     CL_DrawHUD and VGui_Paint
	PS2_PERF_UPLOAD,  //     textures copied to GS memory (anywhere in the frame)
	PS2_PERF_DRAW,    //     sceGuDrawArray: transform, clip, packets (anywhere in the frame)
	PS2_PERF_SUBMIT,  //     waiting for GIF DMA (anywhere in the frame)
	PS2_PERF_FLIP,    //     gsKit_sync_flip
	PS2_PERF_SND,     //   SND_UpdateSound
	PS2_PERF_COUNT
};

extern unsigned long long ps2_perf_cycles[PS2_PERF_COUNT]; // 32 bits wrap in 14.5 s
extern unsigned int cpu_ticks( void );
void PS2_Perf_EndFrame( void );

#define PS2_PERF_BEGIN( id ) unsigned int ps2_perf_start_##id = cpu_ticks()
#define PS2_PERF_END( id ) ps2_perf_cycles[PS2_PERF_##id] += cpu_ticks() - ps2_perf_start_##id

#else

#define PS2_PERF_BEGIN( id )
#define PS2_PERF_END( id )

#endif // XASH_PS2

#endif // PS2_PERF_H
