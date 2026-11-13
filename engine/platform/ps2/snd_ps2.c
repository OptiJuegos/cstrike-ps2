/*
snd_ps2.c - PS2 sound output through audsrv

The engine mixes on the EE into the dma ring buffer, as on PSP. audsrv on the
IOP queues that 16 bit stereo PCM, resamples it to 48 kHz and feeds the SPU2.
No EE thread: Submit hands over whatever the mixer painted since last time,
and the playback position is what was sent minus what audsrv still holds.
*/
#include "platform/platform.h"
#if XASH_SOUND == SOUND_PS2
#include <malloc.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <audsrv.h>

#include "sound.h"

extern unsigned char cs_ps2_libsd_start[], cs_ps2_libsd_end[];
extern unsigned char cs_ps2_audsrv_start[], cs_ps2_audsrv_end[];

static int snd_ps2_sent; // sample pairs handed to audsrv, follows paintedtime
static qboolean snd_ps2_modules;
static qboolean snd_ps2_open;

static qboolean SNDDMA_LoadModule( const char *name, unsigned char *start, unsigned char *end )
{
	int result = -1;
	int id = SifExecModuleBuffer( start, (unsigned)( end - start ), 0, NULL, &result );

	Con_Printf( "S_PS2: %s id=%d result=%d\n", name, id, result );
	return id >= 0 && result != 1;
}

qboolean SNDDMA_Init( void )
{
	struct audsrv_fmt_t fmt;
	int samplecount, err;

	if( !snd_ps2_modules )
	{
		// stock rom0:LOADFILE cannot load EE buffers without this patch; idempotent
		SifLoadFileInit();
		sbv_patch_enable_lmb();
		if( !SNDDMA_LoadModule( "libsd", cs_ps2_libsd_start, cs_ps2_libsd_end )
		 || !SNDDMA_LoadModule( "audsrv", cs_ps2_audsrv_start, cs_ps2_audsrv_end ))
			return false;
		snd_ps2_modules = true;
	}

	if(( err = audsrv_init( )) != AUDSRV_ERR_NOERROR )
	{
		Con_Printf( S_ERROR "S_PS2: audsrv_init failed (%d)\n", err );
		return false;
	}
	snd_ps2_open = true;

	fmt.freq = SOUND_DMA_SPEED; // the mixer always outputs 44.1 kHz
	fmt.bits = 16;
	fmt.channels = 2;
	if(( err = audsrv_set_format( &fmt )) != AUDSRV_ERR_NOERROR )
	{
		Con_Printf( S_ERROR "S_PS2: audsrv_set_format failed (%d)\n", err );
		SNDDMA_Shutdown();
		return false;
	}
	audsrv_set_volume( MAX_VOLUME );

	dma.format.speed = SOUND_DMA_SPEED;
	dma.format.channels = 2;
	dma.format.width = 2;

	// the mixer masks positions with dma.samples / 2 - 1: power of two only
	samplecount = s_samplecount->value;
	if( samplecount <= 0 || ( samplecount & ( samplecount - 1 )))
		samplecount = 0x4000;

	dma.samples = samplecount * 2;
	dma.samplepos = 0;
	dma.buffer = memalign( 64, dma.samples * 2 );
	if( !dma.buffer )
	{
		SNDDMA_Shutdown();
		return false;
	}
	memset( dma.buffer, 0, dma.samples * 2 );
	snd_ps2_sent = 0;

	Con_Printf( "Using PS2 audsrv driver: %d Hz\n", dma.format.speed );
	dma.initialized = true;
	return true;
}

int SNDDMA_GetSoundtime( void )
{
	// shortcut: paintedtime is never rewound, it wraps after ~6.7 hours of play
	return snd_ps2_sent - audsrv_queued() / 4;
}

void SNDDMA_BeginPainting( void )
{
}

void SNDDMA_Submit( void )
{
	int ring = dma.samples >> 1; // in sample pairs

	if( !dma.initialized )
		return;

	while( snd_ps2_sent != paintedtime )
	{
		int pos = snd_ps2_sent & ( ring - 1 );
		int count = Q_min( paintedtime - snd_ps2_sent, ring - pos );

		// shortcut: no audsrv_wait_audio, it never returned on PCSX2 when the ring did not drain.
		// play_audio takes what fits; the rest goes out on the next Submit. Upgrade if audio drops on hardware.
		int taken = audsrv_play_audio( (const char *)dma.buffer + pos * 4, count * 4 );

		if( taken <= 0 )
			break;
		snd_ps2_sent += taken / 4;
	}
}

void SNDDMA_Activate( qboolean active )
{
	(void)active;
}

void SNDDMA_Shutdown( void )
{
	Con_Printf( "Shutting down audio.\n" );

	if( snd_ps2_open )
	{
		audsrv_stop_audio();
		audsrv_quit();
		snd_ps2_open = false;
	}

	if( dma.buffer )
	{
		free( dma.buffer );
		dma.buffer = NULL;
	}

	dma.initialized = false;
}

#endif // XASH_SOUND == SOUND_PS2
