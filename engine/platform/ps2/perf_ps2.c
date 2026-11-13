/* Report for the phase timers in common/ps2_perf.h. */
#include "common.h"
#include "client.h"
#include "platform/platform.h"
#include "ps2_perf.h"
#include "ps2_bot_perf.h"
#include "ps2_studio_perf.h"
#include "arena_ps2.h"
#include "pspgu.h"

#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PS2_PERF_WINDOW 120u
#define PS2_EE_CYCLES_PER_MS 294912u

unsigned long long ps2_perf_cycles[PS2_PERF_COUNT];
unsigned long long ps2_bot_cycles[PS2_BOT_COUNT];
unsigned long long ps2_studio_cycles[PS2_STUDIO_COUNT];
unsigned int ps2_studio_counts[PS2_STUDIO_N_COUNT];

static const char *const ps2_perf_names[PS2_PERF_COUNT] =
{
	"frame", "sv", "bots", "svent", "svsend", "cl", "cldll", "clpkt", "clemit",
	"scr", "view", "world", "ents", "hud", "upload", "draw", "submit", "flip", "snd"
};

static unsigned ps2_perf_frames;
static double ps2_perf_window_start;

void PS2_Perf_EndFrame( void )
{
	static convar_t *enabled;
	char line[512];
	size_t used;
	double now, elapsed;
	int i;

	if( !enabled )
		enabled = Cvar_FindVar( "ps2_profile_present" );

	if( cls.state != ca_active || !enabled || enabled->value == 0.0f )
	{
		ps2_perf_frames = 0;
		memset( ps2_perf_cycles, 0, sizeof( ps2_perf_cycles ));
		memset( ps2_studio_cycles, 0, sizeof( ps2_studio_cycles ));
		memset( ps2_studio_counts, 0, sizeof( ps2_studio_counts ));
		memset( ps2_bot_cycles, 0, sizeof( ps2_bot_cycles ));
		return;
	}

	now = Platform_DoubleTime();

	if( ps2_perf_frames == 0 )
	{
		// the first frame only opens the window: its cycles are not in "elapsed"
		ps2_perf_window_start = now;
		memset( ps2_perf_cycles, 0, sizeof( ps2_perf_cycles ));
		memset( ps2_studio_cycles, 0, sizeof( ps2_studio_cycles ));
		memset( ps2_studio_counts, 0, sizeof( ps2_studio_counts ));
		memset( ps2_bot_cycles, 0, sizeof( ps2_bot_cycles ));
	}

	if( ++ps2_perf_frames <= PS2_PERF_WINDOW )
		return;

	elapsed = now - ps2_perf_window_start;
	used = snprintf( line, sizeof( line ), "[XASH-PS2] PERF fps=%.1f ms/frame:",
		elapsed > 0.0 ? PS2_PERF_WINDOW / elapsed : 0.0 );

	for( i = 0; i < PS2_PERF_COUNT && used < sizeof( line ); i++ )
	{
		// tenths of a millisecond per frame
		unsigned tenths = (unsigned)( ps2_perf_cycles[i] * 10u / ( PS2_PERF_WINDOW * PS2_EE_CYCLES_PER_MS ));
		used += snprintf( line + used, sizeof( line ) - used, " %s=%u.%u",
			ps2_perf_names[i], tenths / 10u, tenths % 10u );
	}

	{
		static const char *const bot_names[PS2_BOT_COUNT] = { "upkeep", "think", "chatter", "vision", "state", "move" };
		char bot_line[128];
		size_t bot_used = snprintf( bot_line, sizeof( bot_line ), "[XASH-PS2] BOTPERF ms/frame:" );

		for( i = 0; i < PS2_BOT_COUNT && bot_used < sizeof( bot_line ); i++ )
		{
			unsigned tenths = (unsigned)( ps2_bot_cycles[i] * 10u / ( PS2_PERF_WINDOW * PS2_EE_CYCLES_PER_MS ));
			bot_used += snprintf( bot_line + bot_used, sizeof( bot_line ) - bot_used, " %s=%u.%u", bot_names[i], tenths / 10u, tenths % 10u );
		}
		printf( "%s\n", bot_line );
		memset( ps2_bot_cycles, 0, sizeof( ps2_bot_cycles ));
	}

	printf( "%s\n[XASH-PS2] GS tris=%u draws=%u uploads=%u upload_kb=%u per frame\n", line,
		ps2_gs_stats.tris / PS2_PERF_WINDOW, ps2_gs_stats.draws / PS2_PERF_WINDOW,
		ps2_gs_stats.uploads / PS2_PERF_WINDOW, ps2_gs_stats.upload_bytes / 1024u / PS2_PERF_WINDOW );
	{
		static const char *const kind_names[3] = { "world", "light", "color" };

		for( i = 0; i < 3; i++ )
		{
			unsigned tenths = (unsigned)( ps2_gs_stats.kind_cycles[i] * 10u / ( PS2_PERF_WINDOW * PS2_EE_CYCLES_PER_MS ));

			printf( "[XASH-PS2] GSKIND %s ms=%u.%u draws=%u tris=%u verts=%u per frame\n", kind_names[i], tenths / 10u, tenths % 10u,
				ps2_gs_stats.kind_draws[i] / PS2_PERF_WINDOW, ps2_gs_stats.kind_tris[i] / PS2_PERF_WINDOW,
				ps2_gs_stats.kind_verts[i] / PS2_PERF_WINDOW );
		}
	}
	{
		static const char *const studio_names[PS2_STUDIO_COUNT] = { "bones", "matrix", "merge", "verts", "light", "mesh", "setup", "dlight", "total", "prep", "save", "final", "skin" };
		char studio_line[320];
		size_t studio_used = snprintf( studio_line, sizeof( studio_line ), "[XASH-PS2] STUDIOPERF ms/frame:" );

		for( i = 0; i < PS2_STUDIO_COUNT && studio_used < sizeof( studio_line ); i++ )
		{
			unsigned hundredths = (unsigned)( ps2_studio_cycles[i] * 100u / ( PS2_PERF_WINDOW * PS2_EE_CYCLES_PER_MS ));
			studio_used += snprintf( studio_line + studio_used, sizeof( studio_line ) - studio_used, " %s=%u.%02u", studio_names[i], hundredths / 100u, hundredths % 100u );
		}
		printf( "%s draws=%u bones=%u verts=%u norms=%u strips=%u stripverts=%u lightcalls=%u lightmisses=%u per frame\n", studio_line,
			ps2_studio_counts[PS2_STUDIO_N_DRAWS] / PS2_PERF_WINDOW, ps2_studio_counts[PS2_STUDIO_N_BONES] / PS2_PERF_WINDOW,
			ps2_studio_counts[PS2_STUDIO_N_VERTS] / PS2_PERF_WINDOW, ps2_studio_counts[PS2_STUDIO_N_NORMS] / PS2_PERF_WINDOW,
			ps2_studio_counts[PS2_STUDIO_N_STRIPS] / PS2_PERF_WINDOW, ps2_studio_counts[PS2_STUDIO_N_STRIPVERTS] / PS2_PERF_WINDOW,
			ps2_studio_counts[PS2_STUDIO_N_LIGHTCALLS] / PS2_PERF_WINDOW, ps2_studio_counts[PS2_STUDIO_N_LIGHTMISSES] / PS2_PERF_WINDOW );
		memset( ps2_studio_cycles, 0, sizeof( ps2_studio_cycles ));
		memset( ps2_studio_counts, 0, sizeof( ps2_studio_counts ));
	}
	memset( &ps2_gs_stats, 0, sizeof( ps2_gs_stats ));
	{
		struct mallinfo mi = mallinfo();

		printf( "[XASH-PS2] HEAP in_use=%u free_in_arena=%u largest_kb=%u\n",
			(unsigned)mi.uordblks, (unsigned)mi.fordblks, (unsigned)( PS2_Heap_LargestBlock( 16 * 1024 ) / 1024 ));
		PS2_Arena_Report( "in_game" );
	}
	{
		static unsigned breakdown_count;
		void Mem_PS2PrintBreakdown( void );
		void GL_PS2PrintTextureUse( void );

		// repeat about once a minute: comparing dumps shows what grows in long sessions
		if(( breakdown_count++ % 30 ) == 0 )
		{
			Mem_PS2PrintBreakdown();
			GL_PS2PrintTextureUse();
		}
	}
	ps2_perf_frames = 0;
}
