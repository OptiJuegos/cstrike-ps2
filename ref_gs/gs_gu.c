/*
 * sceGu* on the PS2 GS, so ref_gu draws through the hardware rasterizer.
 *
 * Fans, strips and triangle lists go to VU1 (ref_gs/gs_vu1.vsm), which transforms,
 * clips, culls and kicks them to the GS. Everything else in the frame travels in
 * the same VIF1 stream as GIF packets, so the order holds: 2D, lines, sprites and
 * registers.
 * The EE writes the stream to uncached memory and DMAs it. Textures stay in EE
 * RAM and are copied to GS memory over the GIF channel when bound.
 */
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <gsKit.h>

#include "build.h"
#include "ps2_perf.h"
#include "ps2_studio_perf.h"
#include "pspgu.h"

#define LOGICAL_W 480.0f
#define LOGICAL_H 272.0f

#define PACKET_BYTES (128 * 1024)
#define PACKET_ROOM 1024 /* a clipped triangle always fits in this */
#define ARENA_BYTES (256 * 1024)
#define CONVERT_BYTES (16 * 1024)

/* Vertices beyond these multiples of w would leave the GS 12.4 coordinates.
 * Triangles are clipped to them by VU1; lines and sprites are dropped. */
#define GUARD_X 5.0f
#define GUARD_Y 7.0f
#define MIN_W 0.001f

/* GS registers */
#define REG_PRIM 0x00
#define REG_TEX0_1 0x06
#define REG_TEX0_2 0x07
#define REG_CLAMP_1 0x08
#define REG_CLAMP_2 0x09
#define REG_TEX1_1 0x14
#define REG_TEX1_2 0x15
#define REG_XYOFFSET_1 0x18
#define REG_XYOFFSET_2 0x19
#define REG_PRMODECONT 0x1a
#define REG_PRMODE 0x1b
#define REG_SCISSOR_2 0x41
#define REG_ALPHA_2 0x43
#define REG_TEST_2 0x48
#define REG_FRAME_2 0x4d
#define REG_ZBUF_2 0x4f
#define REG_TEXFLUSH 0x3f
#define REG_SCISSOR_1 0x40
#define REG_ALPHA_1 0x42
#define REG_DTHE 0x45
#define REG_COLCLAMP 0x46
#define REG_TEST_1 0x47
#define REG_PABE 0x49
#define REG_FBA_1 0x4a
#define REG_FRAME_1 0x4c
#define REG_ZBUF_1 0x4e
#define REG_TEXA 0x3b
#define REG_BITBLTBUF 0x50
#define REG_TRXPOS 0x51
#define REG_TRXREG 0x52
#define REG_TRXDIR 0x53

#define PRIM_POINT 0
#define PRIM_LINE 1
#define PRIM_TRIANGLE 3
#define PRIM_SPRITE 6
#define PRIM_IIP (1 << 3)
#define PRIM_TME (1 << 4)
#define PRIM_ABE (1 << 6)
#define PRIM_CTXT (1 << 9)

#define GS_T8 0x13
#define GS_CT32 0x00
#define GS_CT16 0x02

#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_MADR ((volatile u32 *)0x1000A010)
#define D2_QWC ((volatile u32 *)0x1000A020)
#define D1_CHCR ((volatile u32 *)0x10009000)
#define D1_MADR ((volatile u32 *)0x10009010)
#define D1_QWC ((volatile u32 *)0x10009020)
#define VIF1_STAT ((volatile u32 *)0x10003C00)
#define VIF1_FBRST ((volatile u32 *)0x10003C10)

#define VIF_CODE( imm, num, cmd ) ((u32)(imm) | ((u32)(num) << 16) | ((u32)(cmd) << 24))
#define VIF_NOP 0x00
#define VIF_STCYCL 0x01
#define VIF_FLUSHE 0x10
#define VIF_FLUSH 0x11
#define VIF_FLUSHA 0x13
#define VIF_MSCAL 0x14
#define VIF_MPG 0x4a
#define VIF_DIRECT 0x50
#define VIF_UNPACK_V2_32 0x64
#define VIF_UNPACK_V3_32 0x68
#define VIF_UNPACK_V4_32 0x6c
#define VIF_UNPACK_V4_8 0x6e
#define VIF_UNPACK_USN 0x4000 /* in the immediate: bytes stay unsigned */

/* Addresses in VU1 data memory and code, see gs_vu1.vsm */
#define VU_PARAMS 0
#define VU_PARAMS_QW 7
#define VU_HEADER 8
#define VU_PLANES 10
#define VU_INPUT 16
#define VU_INPUT_QW 281
#define VU_WORLD 297 /* packets that open and close the second pass of a world batch */
#define VU_WORLD_QW 7
#define VU_KIND_STATE 3
#define VU_VERTEX_QW 3
/* One header QW plus three per vertex must fit VU_INPUT_QW. Keep even
 * for strip splits: 1 + 92 * 3 = 277; 94 would overwrite VU_WORLD. */
#define VU_MAX_VERTS 92
#define VU_MSCAL_INIT 0
#define VU_MSCAL_MAIN 3

extern u32 VU1Gs_CodeStart __attribute__(( section( ".vudata" )));
extern u32 VU1Gs_CodeEnd __attribute__(( section( ".vudata" )));

extern GSGLOBAL *PS2_GS_Global( void );

ps2_gs_stats_t ps2_gs_stats;

typedef struct
{
	float x, y, z, w, s, t;
	u32 c;
} clipvert_t;

typedef struct
{
	/* screen position, ready to write */
	float sx, sy;
	u32 z;
	float q;
} projected_t;

enum { CACHE_TEX0, CACHE_TEX1, CACHE_CLAMP, CACHE_ALPHA, CACHE_TEST, CACHE_ZBUF, CACHE_FRAME, CACHE_SCISSOR, CACHE_COUNT };

static const u8 cache_reg[CACHE_COUNT] =
{
	REG_TEX0_1, REG_TEX1_1, REG_CLAMP_1, REG_ALPHA_1, REG_TEST_1, REG_ZBUF_1, REG_FRAME_1, REG_SCISSOR_1
};

static struct
{
	GSGLOBAL *gs;

	/* DMA packets */
	u8 *buf[2];
	int buf_index;
	u64 *p, *end, *start;
	u64 *tag;
	u32 batch_count;
	u64 batch_key;
	int batch_regs;
	u64 *direct; /* VIF code of the open GIF block */

	/* VU1 batch */
	u64 *vu_start, *vu_header;
	int vu_prims, vu_used;
	u32 vu_prim;
	int vu_cull;
	int vu_world;
	int vu_params_dirty;

	/* world lightmap pass, GS context 2 */
	const void *lm_ptr;
	u64 lm_tex0; /* without CLD */
	int lm_nearest;
	int ctx2_dirty;

	/* state */
	u32 enabled;
	int zfunc, zmask;
	int zoffset;
	float znear, zfar;
	int bsrc, bdst;
	u32 bfixs, bfixd;
	int afunc, aref;
	int tfx, tcc;
	u32 color, clear_color;
	int shade, front;
	float vcx, vcy, vhw, vhh;
	float ofx, ofy;
	int sc[4];

	/* texture state */
	const void *tex_ptr;
	const void *clut_ptr;
	int tex_w, tex_h, tex_psm;
	int fmin, fmag, wrapu, wrapv;
	float tsx, tsy, tox, toy;
	int tex_dirty;
	int state_dirty;
	int tw, th; /* log2 of the texture size */
	u32 tbp, tbw, gs_psm;
	float inv_w, inv_h;

	u64 cache[CACHE_COUNT];
	u64 frame_base; /* FRAME_1 without the mask */

	/* matrices: M[column][row] */
	float mat[4][4][4];
	int mode;
	float mvp[4][4];
	int mvp_dirty;

	/* scratch vertices handed out by sceGuGetMemory */
	u8 *arena, *arena_cur, *arena_last;

	u8 *convert;

	/* per-draw values */
	int textured;
} g;

/* ---------- packets ---------- */

/* set by sceGuSwapBuffers until the vblank shows the finished buffer (flip_vblank) */
static volatile int flip_pending;
static void flip_wait( void );

static inline void dma_wait( void )
{
	while( *D2_CHCR & 0x100 ) { }
}

static inline void dma_send( const void *data, u32 qwc )
{
	if( flip_pending ) flip_wait(); /* the back buffer is still on screen */
	dma_wait();
	*D2_QWC = qwc;
	*D2_MADR = (u32)data & 0x0fffffff;
	*D2_CHCR = 0x101; /* from memory, normal mode, start */
}

static inline void vif_dma_wait( void )
{
	while( *D1_CHCR & 0x100 ) { }
}

static inline void vif_dma_send( const void *data, u32 qwc )
{
	if( flip_pending ) flip_wait(); /* the back buffer is still on screen */
	vif_dma_wait();
	*D1_QWC = qwc;
	*D1_MADR = (u32)data & 0x0fffffff;
	*D1_CHCR = 0x101;
}

/* VIF1 has run the stream to its end, including the FLUSHA buffer_kick adds */
static inline void vif_idle_wait( void )
{
	vif_dma_wait();
	while( *VIF1_STAT & 0x1f000003 ) { }
}

static inline void write_vif( u32 c0, u32 c1, u32 c2, u32 c3 )
{
	g.p[0] = (u64)c0 | ((u64)c1 << 32);
	g.p[1] = (u64)c2 | ((u64)c3 << 32);
	g.p += 2;
}

static void buffer_begin( void )
{
	g.start = (u64 *)((u32)g.buf[g.buf_index] | 0x30000000); /* uncached accelerated */
	g.p = g.start;
	g.end = (u64 *)((u8 *)g.start + PACKET_BYTES);
}

static void batch_close( void )
{
	if( !g.tag )
		return;

	if( !g.batch_count )
	{
		g.p = g.tag;
		g.tag = NULL;
		return;
	}

	g.tag[0] |= (u64)g.batch_count | ((u64)1 << 15);
	if(( g.batch_count * g.batch_regs ) & 1 )
		*g.p++ = 0;

	g.tag = NULL;
	g.batch_count = 0;
}

static void gif_close( void )
{
	u32 qwc;

	if( !g.direct )
		return;

	batch_close();
	qwc = (u32)( g.p - g.direct - 2 ) / 2;
	if( qwc )
	{
		g.direct[0] = (u64)VIF_CODE( 0, 0, VIF_NOP ) | ((u64)VIF_CODE( 0, 0, VIF_NOP ) << 32);
		/* FLUSH: VU1 is done and its last XGKICK went out, so the order holds */
		g.direct[1] = (u64)VIF_CODE( 0, 0, VIF_FLUSH ) | ((u64)VIF_CODE( qwc, 0, VIF_DIRECT ) << 32);
	}
	else g.p = g.direct;
	g.direct = NULL;
}

static void vu_close( void )
{
	if( !g.vu_start )
		return;

	if( g.vu_prims )
	{
		g.vu_header[0] = (u64)(u32)g.vu_prims | ((u64)(u32)g.vu_cull << 32);
		g.vu_header[1] = (u64)(u32)g.vu_world; /* w = 0: first pass */
		write_vif( 0, 0, 0, VIF_CODE( VU_MSCAL_MAIN, 0, VIF_MSCAL ));
	}
	else g.p = g.vu_start;

	g.vu_start = NULL;
}

/* Sends everything written so far. FLUSHA at the end keeps VIF1 busy until the
 * GS has it, which is what vif_idle_wait waits for. */
static void buffer_kick( void )
{
	PS2_PERF_BEGIN( SUBMIT );

	gif_close();
	vu_close();
	if( g.p != g.start )
	{
		write_vif( 0, 0, 0, VIF_CODE( 0, 0, VIF_FLUSHA ));
		vif_dma_send( g.buf[g.buf_index], (u32)((u8 *)g.p - (u8 *)g.start) >> 4 );
		g.buf_index ^= 1;
	}
	buffer_begin();

	PS2_PERF_END( SUBMIT );
}

/* The next writes are GIF data: they go in a VIF DIRECT block. */
static void gif_begin( void )
{
	if( g.direct )
		return;

	vu_close();
	g.direct = g.p;
	g.p += 2;
}

static inline void write_ad_tag( int count )
{
	g.p[0] = (u64)count | ((u64)1 << 15) | ((u64)1 << 60); /* PACKED, one register */
	g.p[1] = 0xE; /* A+D */
	g.p += 2;
}

static inline void write_ad( int reg, u64 value )
{
	g.p[0] = value;
	g.p[1] = reg;
	g.p += 2;
}

static void batch_open( void )
{
	u64 prim = g.batch_key & 0x7ff;

	if( g.end - g.p < PACKET_ROOM / 8 )
		buffer_kick();
	gif_begin();

	/* the GIF ignores the PRIM field of a REGLIST tag */
	write_ad_tag( 1 );
	write_ad( REG_PRIM, prim );

	g.tag = g.p;
	/* NLOOP and EOP are filled in on close */
	g.tag[0] = ((u64)1 << 58) | ((u64)g.batch_regs << 60);
	g.tag[1] = g.batch_regs == 3 ? 0x512 : 0x51; /* ST RGBAQ XYZ2, or RGBAQ XYZ2 */
	g.p += 2;
	g.batch_count = 0;
}

static inline void batch_begin( u32 prim )
{
	u64 key = prim | ((u64)g.textured << 32);

	if( !g.tag || g.batch_key != key )
	{
		batch_close();
		g.batch_key = key;
		g.batch_regs = g.textured ? 3 : 2;
		batch_open();
	}
	else if( g.end - g.p < PACKET_ROOM / 8 )
	{
		buffer_kick();
		batch_open();
	}
}

static void set_registers( const u64 *values )
{
	int i, changed = 0;

	for( i = 0; i < CACHE_COUNT; i++ )
		if( g.cache[i] != values[i] )
			changed++;

	if( !changed )
		return;

	batch_close();
	if( g.end - g.p < PACKET_ROOM / 8 )
		buffer_kick();
	gif_begin();

	write_ad_tag( changed );
	for( i = 0; i < CACHE_COUNT; i++ )
	{
		if( g.cache[i] != values[i] )
		{
			write_ad( cache_reg[i], values[i] );
			g.cache[i] = values[i];
		}
	}
}

/* ---------- GS memory ---------- */

#define CLUT_SLOTS 256
#define CLUT_WAYS 4
#define CLUT_BYTES 1024
#define TEX_ENTRIES 1024
#define TEX_HASH 512

typedef struct
{
	const void *ptr;
	u32 addr, size;
	short next; /* hash chain */
} tex_entry_t;

static tex_entry_t tex_entries[TEX_ENTRIES]; /* FIFO in the same order as the VRAM ring */
static short tex_hash[TEX_HASH];
static int tex_head, tex_tail;
static u32 vram_tex_start, vram_tex_end, vram_cursor, vram_clut;

static struct { const void *ptr; int variant; unsigned stamp; } clut_owner[CLUT_SLOTS];
static unsigned clut_clock;

static inline int tex_hash_of( const void *ptr )
{
	return (((u32)ptr >> 4) ^ ((u32)ptr >> 13)) & (TEX_HASH - 1);
}

static void tex_unlink( int index )
{
	tex_entry_t *e = &tex_entries[index];
	short *link;

	if( !e->ptr )
		return;

	for( link = &tex_hash[tex_hash_of( e->ptr )]; *link >= 0; link = &tex_entries[*link].next )
	{
		if( *link == index )
		{
			*link = e->next;
			break;
		}
	}
	e->ptr = NULL;
}

static void tex_cache_reset( void )
{
	int i;

	memset( tex_entries, 0, sizeof( tex_entries ));
	for( i = 0; i < TEX_HASH; i++ )
		tex_hash[i] = -1;
	tex_head = tex_tail = 0;
	vram_cursor = vram_tex_start;
	memset( clut_owner, 0, sizeof( clut_owner ));
}

static u32 tex_alloc( const void *ptr, u32 size )
{
	tex_entry_t *e;
	u32 addr;
	int index;

	size = ( size + 8191 ) & ~8191u; /* page aligned: the GS swizzle stays inside it */
	if( vram_cursor + size > vram_tex_end )
	{
		/* the entries past the cursor are the oldest, drop them on wrap */
		while( tex_tail != tex_head && tex_entries[tex_tail].addr >= vram_cursor )
		{
			tex_unlink( tex_tail );
			tex_tail = ( tex_tail + 1 ) % TEX_ENTRIES;
		}
		vram_cursor = vram_tex_start;
	}

	addr = vram_cursor;

	while( tex_tail != tex_head )
	{
		e = &tex_entries[tex_tail];
		if( e->ptr && ( e->addr + e->size <= addr || e->addr >= addr + size ))
			break;
		tex_unlink( tex_tail );
		tex_tail = ( tex_tail + 1 ) % TEX_ENTRIES;
	}

	if(( tex_head + 1 ) % TEX_ENTRIES == tex_tail )
	{
		tex_unlink( tex_tail );
		tex_tail = ( tex_tail + 1 ) % TEX_ENTRIES;
	}

	index = tex_head;
	tex_head = ( tex_head + 1 ) % TEX_ENTRIES;

	e = &tex_entries[index];
	e->ptr = ptr;
	e->addr = addr;
	e->size = size;
	e->next = tex_hash[tex_hash_of( ptr )];
	tex_hash[tex_hash_of( ptr )] = index;

	vram_cursor = addr + size;
	return addr;
}

static tex_entry_t *tex_find( const void *ptr )
{
	int i;

	for( i = tex_hash[tex_hash_of( ptr )]; i >= 0; i = tex_entries[i].next )
		if( tex_entries[i].ptr == ptr )
			return &tex_entries[i];
	return NULL;
}

void sceKernelDcacheWritebackRange( const void *p, unsigned int size )
{
	tex_entry_t *e = tex_find( p );
	int i;

	(void)size;
	if( e )
		tex_unlink( e - tex_entries );
	if( p == g.tex_ptr )
		g.tex_dirty = 1;
	g.state_dirty = 1; /* this may also have been the bound palette */

	for( i = 0; i < CLUT_SLOTS; i++ )
		if( clut_owner[i].ptr == p )
			clut_owner[i].ptr = NULL;
}

void sceKernelDcacheWritebackAll( void )
{
}

/* Host to GS transfer over the GIF channel, after all that was queued before it. */
static u64 gif_setup[12] __attribute__(( aligned( 64 )));

int ps2_gs_trace; /* debug: set by the sprite loader around a suspect upload */

static void image_begin( u32 addr, int bw, int psm, int w, int h )
{
	u64 *p = (u64 *)((u32)gif_setup | 0x20000000); /* uncached */

	if( ps2_gs_trace ) printf( "[XASH-PS2] GS image_begin addr=%x bw=%d psm=%d %dx%d vif1=%08x d1=%08x d2=%08x\n",
		(unsigned)addr, bw, psm, w, h, (unsigned)*VIF1_STAT, (unsigned)*D1_CHCR, (unsigned)*D2_CHCR );
	buffer_kick();
	if( ps2_gs_trace ) printf( "[XASH-PS2] GS kicked vif1=%08x d1=%08x\n", (unsigned)*VIF1_STAT, (unsigned)*D1_CHCR );
	vif_idle_wait();
	if( ps2_gs_trace ) printf( "[XASH-PS2] GS vif idle\n" );

	p[0] = 4 | ((u64)1 << 15) | ((u64)1 << 60);
	p[1] = 0xE;
	p[2] = ((u64)( addr >> 8 ) << 32) | ((u64)bw << 48) | ((u64)psm << 56);
	p[3] = REG_BITBLTBUF;
	p[4] = 0;
	p[5] = REG_TRXPOS;
	p[6] = (u64)w | ((u64)h << 32);
	p[7] = REG_TRXREG;
	p[8] = 0;
	p[9] = REG_TRXDIR;
	dma_send( gif_setup, 5 );
	dma_wait();
}

static void image_send( const void *data, u32 bytes )
{
	u64 *p = (u64 *)((u32)gif_setup | 0x20000000);
	u32 qwc = ( bytes + 15 ) >> 4;

	p[0] = (u64)qwc | ((u64)1 << 15) | ((u64)2 << 58); /* IMAGE */
	p[1] = 0;
	dma_send( gif_setup, 1 );
	dma_wait();

	FlushCache( 0 );
	if( ps2_gs_trace ) printf( "[XASH-PS2] GS image_send data=%p bytes=%u qwc=%u\n", data, (unsigned)bytes, (unsigned)qwc );
	dma_send( data, qwc );
	dma_wait();
	if( ps2_gs_trace ) printf( "[XASH-PS2] GS image_send done\n" );
	ps2_gs_stats.upload_bytes += bytes;
}

static void image_end( void )
{
	gif_begin();
	write_ad_tag( 1 );
	write_ad( REG_TEXFLUSH, 0 );
	ps2_gs_stats.uploads++;
}

static void upload_clut( const u32 *palette, u32 addr, int variant )
{
	u32 *out = (u32 *)g.convert;
	int i;

	for( i = 0; i < 256; i++ )
	{
		/* CSM1 swaps index bits 3 and 4 */
		int swapped = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
		u32 c = palette[i];
		u32 a = c >> 24;

		if( variant )
		{
			/* a multiply blend reads the texture as light in its alpha */
			u32 lum = ((c & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + ((c >> 16) & 0xff) * 29) >> 8;
			a = variant == 2 ? lum : ( lum + 1 ) >> 1;
		}
		else a = ( a + 1 ) >> 1; /* the GS uses 0x80 as 1.0 */

		out[swapped] = (c & 0xffffff) | (a << 24);
	}

	image_begin( addr, 1, GS_CT32, 16, 16 );
	image_send( out, CLUT_BYTES );
	image_end();
}

/* Four palettes with the same hash can coexist. Lookup does not reserve a
 * slot: light_page also uses it to check whether the bound texture is safe. */
static inline u32 clut_slot( const void *palette, int variant )
{
	const u32 hash = ((u32)palette >> 4) ^ ((u32)palette >> 12) ^ ((u32)variant * 97);
	const int base = ( hash & ( CLUT_SLOTS / CLUT_WAYS - 1 )) * CLUT_WAYS;
	int i, empty = -1, oldest = -1;
	unsigned oldest_age = 0;

	for( i = base; i < base + CLUT_WAYS; i++ )
	{
		unsigned age;
		if( clut_owner[i].ptr == palette && clut_owner[i].variant == variant )
			return i;
		if( !clut_owner[i].ptr && empty < 0 ) empty = i;
		age = clut_clock - clut_owner[i].stamp;
		if( oldest < 0 || age > oldest_age )
		{
			oldest = i;
			oldest_age = age;
		}
	}
	return empty >= 0 ? empty : oldest;
}
static u32 ensure_clut( const void *palette, int variant )
{
	u32 slot = clut_slot( palette, variant );
	u32 addr = vram_clut + slot * CLUT_BYTES;

	if( clut_owner[slot].ptr != palette || clut_owner[slot].variant != variant )
	{
		PS2_PERF_BEGIN( UPLOAD );
		upload_clut( palette, addr, variant );
		clut_owner[slot].ptr = palette;
		clut_owner[slot].variant = variant;
		PS2_PERF_END( UPLOAD );
	}
	clut_owner[slot].stamp = ++clut_clock;
	return addr >> 8;
}

/* Sends the pixels, converted to what the GS reads when the format differs. */
static void upload_pixels( u32 addr )
{
	const int pixels = g.tex_w * g.tex_h;
	int done = 0;

	image_begin( addr, g.tbw, g.gs_psm, g.tex_w, g.tex_h );

	switch( g.tex_psm )
	{
	case GU_PSM_T8:
		image_send( g.tex_ptr, pixels );
		break;
	case GU_PSM_5551:
		image_send( g.tex_ptr, pixels * 2 );
		break;
	case GU_PSM_5650:
	{
		const u16 *src = g.tex_ptr;
		const int chunk = CONVERT_BYTES / 2;

		while( done < pixels )
		{
			u16 *out = (u16 *)g.convert;
			int n = pixels - done < chunk ? pixels - done : chunk, i;

			for( i = 0; i < n; i++ )
			{
				u32 c = src[done + i];
				out[i] = (c & 0x1f) | ((c >> 1) & 0x3e0) | ((c >> 1) & 0x7c00) | 0x8000;
			}
			image_send( out, n * 2 );
			done += n;
		}
		break;
	}
	case GU_PSM_4444:
	{
		const u16 *src = g.tex_ptr;
		const int chunk = CONVERT_BYTES / 4;

		while( done < pixels )
		{
			u32 *out = (u32 *)g.convert;
			int n = pixels - done < chunk ? pixels - done : chunk, i;

			for( i = 0; i < n; i++ )
			{
				u32 c = src[done + i];
				u32 a = (c >> 12) * 17;
				out[i] = ((c & 0xf) * 17) | (((c >> 4) & 0xf) * 17) << 8 | (((c >> 8) & 0xf) * 17) << 16 | (( a + 1 ) >> 1) << 24;
			}
			image_send( out, n * 4 );
			done += n;
		}
		break;
	}
	default: /* GU_PSM_8888 */
	{
		const u32 *src = g.tex_ptr;
		const int chunk = CONVERT_BYTES / 4;

		while( done < pixels )
		{
			u32 *out = (u32 *)g.convert;
			int n = pixels - done < chunk ? pixels - done : chunk, i;

			for( i = 0; i < n; i++ )
			{
				u32 c = src[done + i];
				out[i] = (c & 0xffffff) | (((( c >> 24 ) + 1 ) >> 1 ) << 24);
			}
			image_send( out, n * 4 );
			done += n;
		}
		break;
	}
	}

	image_end();
}

static int ilog2_up( int v )
{
	int n = 0;

	while(( 1 << n ) < v )
		n++;
	return n;
}

static void ensure_texture( void )
{
	tex_entry_t *e;

	g.tex_dirty = 0;
	if( !g.tex_ptr )
		return;

	switch( g.tex_psm )
	{
	case GU_PSM_T8:
		g.gs_psm = GS_T8;
		break;
	case GU_PSM_5551:
	case GU_PSM_5650:
		g.gs_psm = GS_CT16;
		break;
	default:
		g.gs_psm = GS_CT32;
		break;
	}

	g.tw = ilog2_up( g.tex_w );
	g.th = ilog2_up( g.tex_h );
	g.inv_w = 1.0f / (float)g.tex_w;
	g.inv_h = 1.0f / (float)g.tex_h;
	g.tbw = ( g.tex_w + 63 ) / 64;
	if( g.gs_psm == GS_T8 && ( g.tbw & 1 ))
		g.tbw++; /* the GS needs an even width for 8-bit data */

	e = tex_find( g.tex_ptr );
	if( !e )
	{
		PS2_PERF_BEGIN( UPLOAD );
		u32 addr = tex_alloc( g.tex_ptr, gsKit_texture_size( g.tbw * 64, g.tex_h, g.gs_psm ));
		upload_pixels( addr );
		e = tex_find( g.tex_ptr );
		PS2_PERF_END( UPLOAD );
	}
	g.tbp = e->addr >> 8;
}

/* ---------- state ---------- */

static inline int is_multiply( void )
{
	/* GU_SRC_COLOR and GU_DST_COLOR are both 0 */
	return g.bdst == GU_SRC_COLOR && ( g.bsrc == GU_DST_COLOR || ( g.bsrc == GU_FIX && !g.bfixs ));
}

static void commit_state( void )
{
	u64 values[CACHE_COUNT];
	u32 test;
	int blend = g.enabled & (1 << GU_BLEND);
	int multiply = blend && is_multiply();

	g.textured = ( g.enabled & (1 << GU_TEXTURE_2D)) && g.tex_ptr;

	memcpy( values, g.cache, sizeof( values ));

	if( g.textured )
	{
		u64 tex0;
		int tfx = g.tfx == GU_TFX_MODULATE ? 0 : 1; /* MODULATE or DECAL */
		int variant = 0;

		if( g.tex_dirty )
			ensure_texture();

		tex0 = (u64)g.tbp | ((u64)g.tbw << 14) | ((u64)g.gs_psm << 20) |
			((u64)g.tw << 26) | ((u64)g.th << 30) |
			((u64)( g.tcc == GU_TCC_RGBA ? 1 : 0 ) << 34) | ((u64)tfx << 35);

		if( g.gs_psm == GS_T8 && g.clut_ptr )
		{
			if( multiply )
				variant = g.bsrc == GU_DST_COLOR ? 2 : 1;
			tex0 |= ((u64)ensure_clut( g.clut_ptr, variant ) << 37) | ((u64)1 << 61);
		}

		values[CACHE_TEX0] = tex0;
		values[CACHE_TEX1] = ((u64)( g.fmag & 1 ) << 5) | ((u64)( g.fmin & 1 ) << 6);
		values[CACHE_CLAMP] = (u64)( g.wrapu == GU_CLAMP ? 1 : 0 ) | ((u64)( g.wrapv == GU_CLAMP ? 1 : 0 ) << 2);
	}

	if( blend )
	{
		int a = 0, b = 1, c = 0, d = 1; /* (Cs - Cd) * As + Cd */
		u32 fix = 0x80;

		if( multiply )
		{
			if( g.textured && g.gs_psm == GS_T8 )
			{
				a = 1; b = 2; c = 0; d = 2; /* Cd * As: GS alpha cannot multiply by RGB */
			}
		}
		else if( g.bdst == GU_FIX && g.bfixd )
		{
			/* additive; a fixed source factor of 1 means no source alpha */
			b = 2;
			if( g.bsrc == GU_FIX )
				c = 2;
		}

		values[CACHE_ALPHA] = (u64)a | ((u64)b << 2) | ((u64)c << 4) | ((u64)d << 6) | ((u64)fix << 32);
	}

	test = 0;
	if( g.enabled & (1 << GU_ALPHA_TEST))
	{
		/* PSP func numbers to GS ATST */
		static const u8 atst[8] = { 0, 1, 4, 7, 2, 3, 6, 5 };
		test |= 1 | ((u32)atst[g.afunc & 7] << 1) | ((u32)(( g.aref + 1 ) >> 1 ) << 4);
	}

	if( g.enabled & (1 << GU_DEPTH_TEST))
	{
		/* Larger Z is nearer on the GS. It has no EQUAL: a coplanar pass draws
		 * the same Z values, and nothing nearer can be in front of them. */
		u32 ztst = g.zfunc == GU_LESS ? 3 : ( g.zfunc == GU_ALWAYS ? 1 : 2 );
		test |= (1 << 16) | ( ztst << 17 );
		values[CACHE_ZBUF] = ( g.cache[CACHE_ZBUF] & 0xffffffffu ) | ((u64)( g.zmask ? 1 : 0 ) << 32);
	}
	else
	{
		test |= (1 << 16) | (1 << 17); /* ALWAYS: the GS forbids ZTE 0 */
		values[CACHE_ZBUF] = ( g.cache[CACHE_ZBUF] & 0xffffffffu ) | ((u64)1 << 32);
	}
	values[CACHE_TEST] = test;

	set_registers( values );
}

/* Texture modulation reads 0x80 as 1.0. A flat color writes RGB as is. */
static inline u32 convert_color( u32 c )
{
	if( g.textured )
		return (( c >> 1 ) & 0x7f7f7f7f) + ( c & 0x01010101 );
	return ( c & 0xffffff ) | ((( c >> 24 ) + 1 ) >> 1 ) << 24;
}

/* ---------- vertices ---------- */

static inline void project( const clipvert_t *v, projected_t *out )
{
	const float q = 1.0f / v->w;
	float z = g.znear + ( v->z * q * 0.5f + 0.5f ) * ( g.zfar - g.znear );
	int iz = 65535 - (int)z + g.zoffset;

	out->sx = g.vcx + v->x * q * g.vhw;
	out->sy = g.vcy - v->y * q * g.vhh;
	out->z = iz < 0 ? 0 : ( iz > 65535 ? 65535 : iz );
	out->q = q;
}

static inline u64 pack_xyz( float sx, float sy, u32 z )
{
	const int x = (int)( sx * 16.0f ) + g.gs->OffsetX;
	const int y = (int)( sy * 16.0f ) + g.gs->OffsetY;

	return (u64)(u32)( x & 0xffff ) | ((u64)(u32)( y & 0xffff ) << 16) | ((u64)z << 32);
}

static inline u64 float_bits( float f )
{
	union { float f; u32 u; } b;

	b.f = f;
	return b.u;
}

static inline void write_vertex( const clipvert_t *v, const projected_t *s )
{
	u64 *p = g.p;

	if( g.textured )
	{
		p[0] = float_bits( v->s * s->q ) | ( float_bits( v->t * s->q ) << 32 );
		p += 1;
	}
	p[0] = (u64)convert_color( v->c ) | ( float_bits( s->q ) << 32 );
	p[1] = pack_xyz( s->sx, s->sy, s->z );
	g.p = p + 2;
	g.batch_count++;
}

static inline int needs_clip( const clipvert_t *v )
{
	const float gx = GUARD_X * v->w, gy = GUARD_Y * v->w;

	return v->z + v->w < MIN_W || v->x > gx || v->x < -gx || v->y > gy || v->y < -gy;
}

typedef struct
{
	int stride, tex_off, color_off, vert_off;
	int tex_bits, vert_bits;
	int has_color;
	int plain_uv; /* no scale or offset to apply */
	u32 color; /* converted color when the vertices have none */
	const u8 *base;
	const u16 *indices;
} layout_t;

static void vu_write_params( void )
{
	const float *m = &g.mvp[0][0];
	const float range = g.zfar - g.znear;
	u32 *w;
	int i;

	write_vif( 0, 0, VIF_CODE( 0, 0, VIF_FLUSHE ), VIF_CODE( VU_PARAMS, VU_PARAMS_QW, VIF_UNPACK_V4_32 ));
	w = (u32 *)g.p;
	for( i = 0; i < 16; i++ )
		((float *)w)[i] = m[i];
	((float *)w)[16] = g.vhw;
	((float *)w)[17] = -g.vhh;
	((float *)w)[18] = -0.5f * range;
	((float *)w)[19] = 0.0f;
	((float *)w)[20] = g.vcx + (float)g.gs->OffsetX * 0.0625f;
	((float *)w)[21] = g.vcy + (float)g.gs->OffsetY * 0.0625f;
	((float *)w)[22] = 65535.0f - g.znear - 0.5f * range + (float)g.zoffset;
	((float *)w)[23] = 0.0f;
	((float *)w)[24] = 1.0f / GUARD_X;
	((float *)w)[25] = 1.0f / GUARD_Y;
	((float *)w)[26] = 1.0f;
	((float *)w)[27] = 1.0f;
	g.p += VU_PARAMS_QW * 2;
	g.vu_params_dirty = 0;
}

static void vu_open( u32 prim, int cull, int world )
{
	gif_close();
	vu_close();

	/* parameters, header, world packets and a whole batch: a stream QW carries at least one in VU1 */
	if( (u32)( (u8 *)g.end - (u8 *)g.p ) < ( VU_PARAMS_QW + VU_WORLD_QW + VU_INPUT_QW + 14 ) * 16u )
		buffer_kick();

	g.vu_start = g.p;
	write_vif( 0, 0, 0, VIF_CODE( 0x0101, 0, VIF_STCYCL ));
	if( g.vu_params_dirty )
		vu_write_params();

	write_vif( 0, 0, VIF_CODE( 0, 0, VIF_FLUSHE ), VIF_CODE( VU_HEADER, 2, VIF_UNPACK_V4_32 ));
	g.vu_header = g.p;
	g.p[0] = 0;
	g.p[1] = 0;
	/* PACKED: unlike REGLIST this honors PRE, so the PRIM travels in the tag */
	g.p[2] = ((u64)1 << 46) | ((u64)prim << 47) | ((u64)3 << 60);
	g.p[3] = 0x512; /* ST RGBAQ XYZ2 */
	g.p += 4;

	if( world )
	{
		/* second pass: PRMODE takes the attributes, so the same tags draw in context 2 */
		write_vif( 0, 0, 0, VIF_CODE( VU_WORLD, VU_WORLD_QW, VIF_UNPACK_V4_32 ));
		g.p[0] = 3 | ((u64)1 << 60); g.p[1] = 0xE;
		g.p[2] = 0; g.p[3] = REG_PRMODECONT;
		g.p[4] = PRIM_TME | PRIM_ABE | PRIM_CTXT; g.p[5] = REG_PRMODE;
		g.p[6] = g.lm_tex0 | ((u64)1 << 61); g.p[7] = REG_TEX0_2; /* CLD 1: the first pass left the texture's palette */
		/* closing: the texture's palette goes back in the CLUT buffer, as g.cache expects */
		g.p[8] = 2 | ((u64)1 << 60); g.p[9] = 0xE;
		g.p[10] = 1; g.p[11] = REG_PRMODECONT;
		g.p[12] = g.cache[CACHE_TEX0]; g.p[13] = REG_TEX0_1;
		g.p += VU_WORLD_QW * 2;
	}

	/* vertex attributes unpack one after the other, each to every third QW */
	write_vif( 0, 0, 0, VIF_CODE( 0x0103, 0, VIF_STCYCL ));
	g.vu_prims = 0;
	g.vu_used = 0;
	g.vu_prim = prim;
	g.vu_cull = cull;
	g.vu_world = world;
}

static inline u32 *vu_put_positions( const layout_t *l, u32 *w, int first, int count )
{
	int i;

	if( !l->indices && l->vert_bits == GU_VERTEX_32BITF )
	{
		const u8 *v = l->base + first * l->stride + l->vert_off;
		const int stride = l->stride;

		for( i = 0; i < count; i++, v += stride, w += 3 )
		{
			const u32 *p = (const u32 *)v;
			w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
		}
		return w;
	}

	for( i = first; i < first + count; i++ )
	{
		const u8 *v = l->base + ( l->indices ? l->indices[i] : i ) * l->stride + l->vert_off;

		if( l->vert_bits == GU_VERTEX_32BITF )
		{
			const u32 *p = (const u32 *)v;
			w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
		}
		else
		{
			const short *p = (const short *)v;
			((float *)w)[0] = p[0]; ((float *)w)[1] = p[1]; ((float *)w)[2] = p[2];
		}
		w += 3;
	}
	return w;
}

static inline u32 *vu_put_uvs( const layout_t *l, u32 *w, int first, int count )
{
	int i;

	if( !l->indices && g.textured && l->tex_bits == GU_TEXTURE_32BITF && l->plain_uv )
	{
		const u8 *v = l->base + first * l->stride + l->tex_off;
		const int stride = l->stride;

		for( i = 0; i < count; i++, v += stride, w += 2 )
		{
			const u32 *t = (const u32 *)v;
			w[0] = t[0];
			w[1] = t[1];
		}
		return w;
	}

	for( i = first; i < first + count; i++ )
	{
		const u8 *v = l->base + ( l->indices ? l->indices[i] : i ) * l->stride + l->tex_off;

		if( !g.textured || !l->tex_bits )
		{
			w[0] = w[1] = 0;
		}
		else if( l->tex_bits != GU_TEXTURE_32BITF )
		{
			const short *t = (const short *)v;
			((float *)w)[0] = t[0] * g.inv_w;
			((float *)w)[1] = t[1] * g.inv_h;
		}
		else if( l->plain_uv )
		{
			const u32 *t = (const u32 *)v;
			w[0] = t[0];
			w[1] = t[1];
		}
		else
		{
			const float *t = (const float *)v;
			((float *)w)[0] = t[0] * g.tsx + g.tox;
			((float *)w)[1] = t[1] * g.tsy + g.toy;
		}
		w += 2;
	}
	return w;
}

static inline u32 *vu_put_colors( const layout_t *l, u32 *w, int first, int count )
{
	int i;

	if( !l->has_color )
	{
		for( i = 0; i < count; i++ )
			*w++ = l->color;
	}
	else if( !l->indices && g.textured )
	{
		const u8 *v = l->base + first * l->stride + l->color_off;
		const int stride = l->stride;

		for( i = 0; i < count; i++, v += stride )
		{
			const u32 c = *(const u32 *)v;
			*w++ = (( c >> 1 ) & 0x7f7f7f7f) + ( c & 0x01010101 );
		}
	}
	else
	{
		for( i = first; i < first + count; i++ )
			*w++ = convert_color( *(const u32 *)( l->base + ( l->indices ? l->indices[i] : i ) * l->stride + l->color_off ));
	}
	return w;
}

/* Opens a primitive of VU1 and returns where its positions go. */
static inline u32 *vu_begin_primitive( int kind, u32 prim, int cull, int world, int verts, u32 *addr )
{
	const int need = 1 + verts * VU_VERTEX_QW;
	static const u8 gs_prim[3] = { 3, 5, 4 }; /* triangle, fan, strip */
	u32 *w;

	if( !g.vu_start || g.vu_prim != prim || g.vu_cull != cull || g.vu_world != world || g.vu_used + need > VU_INPUT_QW )
		vu_open( prim, cull, world );

	*addr = VU_INPUT + g.vu_used;
	w = (u32 *)g.p;

	/* VIF codes need only word alignment, so the attributes follow each other */
	*w++ = VIF_CODE( *addr, 1, VIF_UNPACK_V4_32 );
	w[0] = verts;
	w[1] = ( 1u << 14 ) | ((( prim & ~7u ) | gs_prim[kind] ) << 15 ) | ( 3u << 28 ); /* PRE, PRIM, NREG */
	w[2] = 0x512; /* ST RGBAQ XYZ2 */
	w[3] = kind; /* the GIF reads only NREG registers from here */
	w += 4;

	*w++ = VIF_CODE( *addr + 1, verts, VIF_UNPACK_V3_32 );
	return w;
}

static inline void vu_end_primitive( u32 *w, int verts )
{
	while( (u32)w & 15 )
		*w++ = VIF_CODE( 0, 0, VIF_NOP );

	g.p = (u64 *)w;
	g.vu_used += 1 + verts * VU_VERTEX_QW;
	g.vu_prims++;
	ps2_gs_stats.verts += verts;
}

/* One primitive for VU1: vertices first..first + count - 1, after vertex hub if it is not negative.
 * kind is 0 for a list, 1 for a fan and 2 for a strip. */
static void vu_primitive( const layout_t *l, int kind, u32 prim, int cull, int hub, int first, int count )
{
	const int verts = count + ( hub >= 0 );
	u32 addr;
	u32 *w = vu_begin_primitive( kind, prim, cull, 0, verts, &addr );

	if( hub >= 0 )
		w = vu_put_positions( l, w, hub, 1 );
	w = vu_put_positions( l, w, first, count );

	*w++ = VIF_CODE( addr + 2, verts, VIF_UNPACK_V2_32 );
	if( hub >= 0 )
		w = vu_put_uvs( l, w, hub, 1 );
	w = vu_put_uvs( l, w, first, count );

	*w++ = VIF_CODE( ( addr + 3 ) | VIF_UNPACK_USN, verts, VIF_UNPACK_V4_8 );
	if( hub >= 0 )
		w = vu_put_colors( l, w, hub, 1 );
	w = vu_put_colors( l, w, first, count );

	vu_end_primitive( w, verts );
}

static void update_mvp( void )
{
	float pv[4][4];
	int c, r;

	for( c = 0; c < 4; c++ )
		for( r = 0; r < 4; r++ )
			pv[c][r] = g.mat[GU_PROJECTION][0][r] * g.mat[GU_VIEW][c][0] +
				g.mat[GU_PROJECTION][1][r] * g.mat[GU_VIEW][c][1] +
				g.mat[GU_PROJECTION][2][r] * g.mat[GU_VIEW][c][2] +
				g.mat[GU_PROJECTION][3][r] * g.mat[GU_VIEW][c][3];

	for( c = 0; c < 4; c++ )
		for( r = 0; r < 4; r++ )
			g.mvp[c][r] = pv[0][r] * g.mat[GU_MODEL][c][0] + pv[1][r] * g.mat[GU_MODEL][c][1] +
				pv[2][r] * g.mat[GU_MODEL][c][2] + pv[3][r] * g.mat[GU_MODEL][c][3];

	g.mvp_dirty = 0;
	g.vu_params_dirty = 1;
}

static void layout_init( layout_t *l, int vtype, const void *indices, const void *vertices )
{
	static int cached_vtype = -1, cached_stride, cached_color_off, cached_vert_off;
	int offset = 0, align = 1;

	l->tex_bits = vtype & GU_TEXTURE_BITS;
	l->vert_bits = vtype & GU_VERTEX_BITS;
	l->has_color = ( vtype & GU_COLOR_BITS ) != 0;
	l->tex_off = l->color_off = 0;
	l->base = vertices;
	l->indices = ( vtype & GU_INDEX_BITS ) ? indices : NULL;
	l->plain_uv = g.tsx == 1.0f && g.tsy == 1.0f && g.tox == 0.0f && g.toy == 0.0f;
	if( !l->has_color )
	{
		l->color = convert_color( g.color );
	}

	if( vtype == cached_vtype )
	{
		l->stride = cached_stride;
		l->color_off = cached_color_off;
		l->vert_off = cached_vert_off;
		return;
	}

	if( l->tex_bits )
	{
		int size = l->tex_bits == GU_TEXTURE_32BITF ? 4 : 2;
		l->tex_off = 0;
		offset = size * 2;
		align = size;
	}
	if( l->has_color )
	{
		offset = ( offset + 3 ) & ~3;
		l->color_off = offset;
		offset += 4;
		align = 4;
	}
	if( vtype & GU_NORMAL_BITS )
	{
		offset = ( offset + 3 ) & ~3;
		offset += 12;
		align = 4;
	}
	{
		int size = l->vert_bits == GU_VERTEX_32BITF ? 4 : 2;
		offset = ( offset + size - 1 ) & ~( size - 1 );
		l->vert_off = offset;
		offset += size * 3;
		if( size > align )
			align = size;
	}
	l->stride = ( offset + align - 1 ) & ~( align - 1 );
	cached_vtype = vtype;
	cached_stride = l->stride;
	cached_color_off = l->color_off;
	cached_vert_off = l->vert_off;
}

static inline void fetch3d( const layout_t *l, int i, clipvert_t *out )
{
	const u8 *v = l->base + ( l->indices ? l->indices[i] : i ) * l->stride;
	const float *m = &g.mvp[0][0];
	float x, y, z;

	if( l->vert_bits == GU_VERTEX_32BITF )
	{
		const float *p = (const float *)( v + l->vert_off );
		x = p[0]; y = p[1]; z = p[2];
	}
	else
	{
		const short *p = (const short *)( v + l->vert_off );
		x = p[0]; y = p[1]; z = p[2];
	}

	out->x = m[0] * x + m[4] * y + m[8] * z + m[12];
	out->y = m[1] * x + m[5] * y + m[9] * z + m[13];
	out->z = m[2] * x + m[6] * y + m[10] * z + m[14];
	out->w = m[3] * x + m[7] * y + m[11] * z + m[15];

	if( g.textured && l->tex_bits )
	{
		if( l->tex_bits == GU_TEXTURE_32BITF )
		{
			const float *t = (const float *)( v + l->tex_off );
			out->s = t[0] * g.tsx + g.tox;
			out->t = t[1] * g.tsy + g.toy;
		}
		else
		{
			const short *t = (const short *)( v + l->tex_off );
			out->s = t[0] * g.inv_w;
			out->t = t[1] * g.inv_h;
		}
	}
	else out->s = out->t = 0.0f;

	out->c = l->has_color ? *(const u32 *)( v + l->color_off ) : g.color;
}

/* Lines and sprites are not clipped: one with a vertex behind the eye is dropped. */
static void draw_2_vertices_3d( const layout_t *l, int a, int b, u32 prim )
{
	clipvert_t va, vb;
	projected_t pa, pb;

	fetch3d( l, a, &va );
	fetch3d( l, b, &vb );

	if( needs_clip( &va ) || needs_clip( &vb ))
		return;

	project( &va, &pa );
	project( &vb, &pb );
	batch_begin( prim );
	write_vertex( &va, &pa );
	write_vertex( &vb, &pb );
}

static void draw_3d( int prim, int count, const layout_t *l, u32 gs_flags )
{
	const u32 tri = PRIM_TRIANGLE | gs_flags;
	const int cull = ( g.enabled & (1 << GU_CULL_FACE)) ? ( g.front == GU_CCW ? 1 : 2 ) : 0;
	clipvert_t v[3];
	int i, take;

	if( g.vu_params_dirty )
		vu_close();

	switch( prim )
	{
	case GU_TRIANGLES:
		count -= count % 3;
		ps2_gs_stats.tris += count / 3;
		for( i = 0; i < count; i += take )
		{
			take = count - i < VU_MAX_VERTS - VU_MAX_VERTS % 3 ? count - i : VU_MAX_VERTS - VU_MAX_VERTS % 3; /* whole triangles */
			vu_primitive( l, 0, tri, cull, -1, i, take );
		}
		break;
	case GU_TRIANGLE_FAN:
		if( count >= 3 )
			ps2_gs_stats.tris += count - 2;
		for( i = 1; i + 1 < count; i += take - 1 )
		{
			take = count - i < VU_MAX_VERTS - 1 ? count - i : VU_MAX_VERTS - 1;
			vu_primitive( l, 1, tri, cull, 0, i, take );
		}
		break;
	case GU_TRIANGLE_STRIP:
		if( count >= 3 )
			ps2_gs_stats.tris += count - 2;
		for( i = 0; i + 2 < count; i += take - 2 )
		{
			take = count - i < VU_MAX_VERTS ? count - i : VU_MAX_VERTS;
			vu_primitive( l, 2, tri, cull, -1, i, take );
		}
		break;
	case GU_LINES:
		for( i = 0; i + 1 < count; i += 2 )
			draw_2_vertices_3d( l, i, i + 1, PRIM_LINE | gs_flags );
		break;
	case GU_LINE_STRIP:
		for( i = 0; i + 1 < count; i++ )
			draw_2_vertices_3d( l, i, i + 1, PRIM_LINE | gs_flags );
		break;
	case GU_SPRITES:
		for( i = 0; i + 1 < count; i += 2 )
			draw_2_vertices_3d( l, i, i + 1, PRIM_SPRITE | gs_flags );
		break;
	case GU_POINTS:
		for( i = 0; i < count; i++ )
		{
			projected_t p;

			fetch3d( l, i, &v[0] );
			if( needs_clip( &v[0] ))
				continue;
			project( &v[0], &p );
			batch_begin( PRIM_POINT | gs_flags );
			write_vertex( &v[0], &p );
		}
		break;
	}
}

/* Screen coordinates, already past the viewport: only the logical scale applies. */
static void fetch2d( const layout_t *l, int i, clipvert_t *v, projected_t *s, float scale_x, float scale_y )
{
	const u8 *p = l->base + ( l->indices ? l->indices[i] : i ) * l->stride;
	float x, y, z;

	if( l->vert_bits == GU_VERTEX_32BITF )
	{
		const float *f = (const float *)( p + l->vert_off );
		x = f[0]; y = f[1]; z = f[2];
	}
	else
	{
		const short *f = (const short *)( p + l->vert_off );
		x = f[0]; y = f[1]; z = (unsigned short)f[2];
	}

	v->s = v->t = 0.0f;
	if( g.textured && l->tex_bits )
	{
		if( l->tex_bits == GU_TEXTURE_32BITF )
		{
			const float *t = (const float *)( p + l->tex_off );
			v->s = t[0] * g.inv_w;
			v->t = t[1] * g.inv_h;
		}
		else
		{
			const short *t = (const short *)( p + l->tex_off );
			v->s = t[0] * g.inv_w;
			v->t = t[1] * g.inv_h;
		}
	}
	v->c = l->has_color ? *(const u32 *)( p + l->color_off ) : g.color;
	s->sx = x * scale_x;
	s->sy = y * scale_y;
	s->z = 65535 - (u32)z;
	s->q = 1.0f;
}

static void draw_2d( int prim, int count, const layout_t *l, u32 gs_flags )
{
	const float scale_x = (float)g.gs->Width / LOGICAL_W, scale_y = (float)g.gs->Height / LOGICAL_H;
	clipvert_t v;
	projected_t s;
	int i;

	if( prim == GU_SPRITES )
	{
		for( i = 0; i < count; i++ )
		{
			if( !( i & 1 ))
				batch_begin( PRIM_SPRITE | gs_flags );
			fetch2d( l, i, &v, &s, scale_x, scale_y );
			write_vertex( &v, &s );
		}
	}
	else if( prim == GU_TRIANGLES )
	{
		for( i = 0; i < count; i++ )
		{
			if( !( i % 3 ))
				batch_begin( PRIM_TRIANGLE | gs_flags );
			fetch2d( l, i, &v, &s, scale_x, scale_y );
			write_vertex( &v, &s );
		}
	}
}

void sceGuDrawArray( int prim, int vtype, int count, const void *indices, const void *vertices )
{
	layout_t l;
	u32 gs_flags = 0;

	if( count <= 0 || !vertices )
		return;

	PS2_PERF_BEGIN( DRAW );

	if( g.state_dirty || g.tex_dirty )
	{
		commit_state();
		g.state_dirty = 0;
	}

	if( g.textured )
		gs_flags |= PRIM_TME;
	if( g.enabled & (1 << GU_BLEND))
		gs_flags |= PRIM_ABE;
	if( g.shade == GU_SMOOTH )
		gs_flags |= PRIM_IIP;

	layout_init( &l, vtype, indices, vertices );
	ps2_gs_stats.draws++;

	if( vtype & GU_TRANSFORM_2D )
	{
		draw_2d( prim, count, &l, gs_flags );
	}
	else
	{
		const int kind = l.has_color ? 2 : ( g.enabled & (1 << GU_BLEND)) && is_multiply() ? 1 : 0;
		const unsigned tris = ps2_gs_stats.tris, verts = ps2_gs_stats.verts, start = cpu_ticks();

		if( g.mvp_dirty )
			update_mvp();
		draw_3d( prim, count, &l, gs_flags );

		ps2_gs_stats.kind_cycles[kind] += cpu_ticks() - start;
		ps2_gs_stats.kind_draws[kind]++;
		ps2_gs_stats.kind_tris[kind] += ps2_gs_stats.tris - tris;
		ps2_gs_stats.kind_verts[kind] += ps2_gs_stats.verts - verts;
	}

	/* the vertices are in the packet now, so the last block can be reused */
	if(( const u8 * )vertices == g.arena_last )
		g.arena_cur = g.arena_last;

	PS2_PERF_END( DRAW );
}

/* The same VIF payload for the single-strip and whole-mesh entry points. */
static inline void studio_primitive( int kind, u32 flags, int cull, const short *cmds, int count, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha, const float (*chrome)[2] )
{
	const short *c;
	u32 addr, *w;
	int i;
	w = vu_begin_primitive( kind, flags, cull, 0, count, &addr );

	for( i = 0, c = cmds; i < count; i++, c += 4, w += 3 )
	{
		const u32 *p = (const u32 *)verts[c[0]];
		w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
	}

	*w++ = VIF_CODE( addr + 2, count, VIF_UNPACK_V2_32 );
	if( chrome )
	{
		for( i = 0, c = cmds; i < count; i++, c += 4, w += 2 )
		{
			((float *)w)[0] = chrome[c[1]][0] * s;
			((float *)w)[1] = chrome[c[1]][1] * t;
		}
	}
	else
	{
		for( i = 0, c = cmds; i < count; i++, c += 4, w += 2 )
		{
			((float *)w)[0] = c[2] * s;
			((float *)w)[1] = c[3] * t;
		}
	}
	*w++ = VIF_CODE( ( addr + 3 ) | VIF_UNPACK_USN, count, VIF_UNPACK_V4_8 );
	for( i = 0, c = cmds; i < count; i++, c += 4 )
	{
		const u32 color = rgb[c[1]] | alpha;
		*w++ = (( color >> 1 ) & 0x7f7f7f7f) + ( color & 0x01010101 );
	}

	vu_end_primitive( w, count );
}

/* A strip or fan of a studio model, straight from its tricmds (vertex, normal, s, t)
 * to the stream, without a copy in the arena or a layout per strip. Returns 0 for
 * anything it does not cover, and the caller then takes sceGuDrawArray. */
int ps2_gu_studio_prim( int prim, const short *cmds, int count, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha )
{
	const int kind = prim == GU_TRIANGLE_FAN ? 1 : 2;
	const unsigned start = cpu_ticks();
	u32 flags = PRIM_TRIANGLE | PRIM_TME;

	if( count < 3 || count > VU_MAX_VERTS || g.tsx != 1.0f || g.tsy != 1.0f || g.tox != 0.0f || g.toy != 0.0f )
		return 0;

	if( g.state_dirty || g.tex_dirty )
	{
		commit_state();
		g.state_dirty = 0;
	}

	if( !g.textured )
		return 0;

	if( g.enabled & (1 << GU_BLEND))
		flags |= PRIM_ABE;
	if( g.shade == GU_SMOOTH )
		flags |= PRIM_IIP;

	if( g.mvp_dirty )
		update_mvp();
	if( g.vu_params_dirty )
		vu_close();

	studio_primitive( kind, flags, ( g.enabled & (1 << GU_CULL_FACE)) ? ( g.front == GU_CCW ? 1 : 2 ) : 0, cmds, count, verts, s, t, rgb, alpha, NULL );

	{
		const unsigned cycles = cpu_ticks() - start;

		ps2_perf_cycles[PS2_PERF_DRAW] += cycles;
		ps2_gs_stats.kind_cycles[2] += cycles;
		ps2_gs_stats.kind_draws[2]++;
		ps2_gs_stats.kind_tris[2] += count - 2;
		ps2_gs_stats.kind_verts[2] += count;
		ps2_gs_stats.tris += count - 2;
		ps2_gs_stats.draws++;
	}
	return 1;
}

/* Prepare state and measure once per mesh, rather than once per tiny strip.
 * Validate the entire command list before emitting any geometry, so rejection
 * can safely use the existing caller path without duplicating earlier strips. */
static int studio_mesh( const short *cmds, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha, const float (*chrome)[2] )
{
	const unsigned start = cpu_ticks();
	const short *c = cmds;
	unsigned strips = 0, vertices = 0;
	u32 flags = PRIM_TRIANGLE | PRIM_TME;
	int count, cull;

	if( g.tsx != 1.0f || g.tsy != 1.0f || g.tox != 0.0f || g.toy != 0.0f )
		return 0;

	while(( count = *c++ ))
	{
		if( count < 0 ) count = -count;
		if( count < 3 || count > VU_MAX_VERTS )
			return 0;
		strips++;
		vertices += count;
		c += count * 4;
	}
	if( !strips ) return 1;

	if( g.state_dirty || g.tex_dirty )
	{
		commit_state();
		g.state_dirty = 0;
	}
	if( !g.textured ) return 0;
	if( g.enabled & (1 << GU_BLEND)) flags |= PRIM_ABE;
	if( g.shade == GU_SMOOTH ) flags |= PRIM_IIP;
	cull = ( g.enabled & (1 << GU_CULL_FACE)) ? ( g.front == GU_CCW ? 1 : 2 ) : 0;
	if( g.mvp_dirty ) update_mvp();
	if( g.vu_params_dirty ) vu_close();

	c = cmds;
	while(( count = *c++ ))
	{
		const int kind = count < 0 ? 1 : 2;
		if( count < 0 ) count = -count;
		studio_primitive( kind, flags, cull, c, count, verts, s, t, rgb, alpha, chrome );
		c += count * 4;
	}

	{
		const unsigned cycles = cpu_ticks() - start;
		ps2_perf_cycles[PS2_PERF_DRAW] += cycles;
		ps2_gs_stats.kind_cycles[2] += cycles;
		ps2_gs_stats.kind_draws[2] += strips;
		ps2_gs_stats.kind_tris[2] += vertices - 2 * strips;
		ps2_gs_stats.kind_verts[2] += vertices;
		ps2_gs_stats.tris += vertices - 2 * strips;
		ps2_gs_stats.draws += strips;
		PS2_STUDIO_ADD( STRIPS, strips );
		PS2_STUDIO_ADD( STRIPVERTS, vertices );
	}
	return 1;
}

int ps2_gu_studio_mesh( const short *cmds, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha )
{
	return studio_mesh( cmds, verts, s, t, rgb, alpha, NULL );
}

/* Chrome UVs are already calculated per normal, including the current view. */
int ps2_gu_studio_chrome_mesh( const short *cmds, const float (*verts)[3], const float (*chrome)[2], float s, float t, const unsigned int *rgb, unsigned int alpha )
{
	return studio_mesh( cmds, verts, s, t, rgb, alpha, chrome );
}
/* TEX0 of a lightmap page in GS memory, uploaded if missing. 0 when its palette
 * would take the CLUT slot of the bound texture: the second pass reloads both. */
static u64 light_page( const void *pixels, const void *palette, int w, int h )
{
	const u32 tbw = (( w + 63 ) / 64 + 1 ) & ~1u; /* even for 8-bit data */
	tex_entry_t *e;
	u32 cbp;

	if( g.clut_ptr && clut_slot( palette, 1 ) == clut_slot( g.clut_ptr, 0 ))
		return 0;

	e = tex_find( pixels );
	if( !e )
	{
		PS2_PERF_BEGIN( UPLOAD );
		u32 addr = tex_alloc( pixels, gsKit_texture_size( tbw * 64, h, GS_T8 ));
		image_begin( addr, tbw, GS_T8, w, h );
		image_send( pixels, w * h );
		image_end();
		e = tex_find( pixels );
		PS2_PERF_END( UPLOAD );
	}
	cbp = ensure_clut( palette, 1 ); /* light in alpha, as the multiply blend reads it */

	return (u64)( e->addr >> 8 ) | ((u64)tbw << 14) | ((u64)GS_T8 << 20) |
		((u64)ilog2_up( w ) << 26) | ((u64)ilog2_up( h ) << 30) | ((u64)1 << 34) | ((u64)cbp << 37);
}

/* Context 2 draws the lightmap over what context 1 drew: same Z, Cd * As. */
static void world_context( int nearest )
{
	batch_close();
	if( g.end - g.p < PACKET_ROOM / 8 )
		buffer_kick();
	gif_begin();

	write_ad_tag( 8 );
	write_ad( REG_FRAME_2, g.frame_base );
	write_ad( REG_ZBUF_2, ( g.cache[CACHE_ZBUF] & 0xffffffffu ) | ((u64)1 << 32));
	write_ad( REG_XYOFFSET_2, (u64)g.gs->OffsetX | ((u64)g.gs->OffsetY << 32));
	write_ad( REG_SCISSOR_2, g.cache[CACHE_SCISSOR] );
	write_ad( REG_TEST_2, (1 << 16) | (2 << 17)); /* GEQUAL */
	write_ad( REG_ALPHA_2, 1 | (2 << 2) | (0 << 4) | (2 << 6));
	write_ad( REG_TEX1_2, nearest ? 0 : (( 1 << 5 ) | ( 1 << 6 )));
	write_ad( REG_CLAMP_2, 0 );

	g.ctx2_dirty = 0;
	g.lm_nearest = nearest;
}

/* A lightmapped world polygon as one VU1 fan, drawn twice from one copy of its
 * vertices: the bound texture in context 1, then its lightmap page in context 2.
 * verts are s t x y z per vertex, luv the lightmap s t. Returns 0 for anything it
 * does not cover, and the caller then takes the texture pass plus R_BlendLightmaps. */
int ps2_gu_world_fan( const float *verts, const float (*luv)[2], int count, const void *lm_pixels, const void *lm_palette, int lm_w, int lm_h, int lm_nearest )
{
	const unsigned start = cpu_ticks();
	const unsigned uploads = ps2_gs_stats.uploads;
	u32 flags = PRIM_TRIANGLE | PRIM_TME;
	u32 addr, color, *w;
	u64 tex0;
	int i, cull;

	if( count < 3 || count > VU_MAX_VERTS || g.tsx != 1.0f || g.tsy != 1.0f || g.tox != 0.0f || g.toy != 0.0f )
		return 0;

	if( g.state_dirty || g.tex_dirty )
	{
		commit_state();
		g.state_dirty = 0;
	}

	if( !g.textured || ( g.enabled & (1 << GU_BLEND)))
		return 0;

	tex0 = light_page( lm_pixels, lm_palette, lm_w, lm_h );
	if( !tex0 )
		return 0;
	if( ps2_gs_stats.uploads != uploads && !tex_find( g.tex_ptr ))
	{
		/* shortcut: assumes sending the texture again does not push the page out; GS memory holds both many times over */
		g.tex_dirty = 1;
		commit_state();
	}

	if( g.ctx2_dirty || g.lm_nearest != lm_nearest )
		world_context( lm_nearest );

	cull = ( g.enabled & (1 << GU_CULL_FACE)) ? ( g.front == GU_CCW ? 1 : 2 ) : 0;
	if( g.shade == GU_SMOOTH )
		flags |= PRIM_IIP;
	if( g.mvp_dirty )
		update_mvp();
	if( g.vu_params_dirty )
		vu_close();

	if( tex0 != g.lm_tex0 )
	{
		const int same_clut = !(( tex0 ^ g.lm_tex0 ) >> 37 );

		g.lm_tex0 = tex0;
		if( same_clut && g.vu_start && g.vu_world && g.vu_prim == flags && g.vu_cull == cull &&
			g.vu_used + 2 + 1 + count * VU_VERTEX_QW <= VU_INPUT_QW )
		{
			/* the page changes inside the batch; CLD 0 keeps the palette already loaded */
			const u32 at = VU_INPUT + g.vu_used;

			w = (u32 *)g.p;
			*w++ = VIF_CODE( at, 1, VIF_UNPACK_V4_32 );
			w[0] = 1; w[1] = 1u << 28; w[2] = 0xE; w[3] = VU_KIND_STATE; /* A+D, NLOOP 1 */
			w += 4;
			*w++ = VIF_CODE( at + 1, 1, VIF_UNPACK_V4_32 );
			w[0] = (u32)tex0; w[1] = (u32)( tex0 >> 32 ); w[2] = REG_TEX0_2; w[3] = 0;
			w += 4;
			while( (u32)w & 15 )
				*w++ = VIF_CODE( 0, 0, VIF_NOP );
			g.p = (u64 *)w;
			g.vu_used += 2;
			g.vu_prims++;
		}
		else vu_close();
	}

	w = vu_begin_primitive( 1, flags, cull, 1, count, &addr );

	for( i = 0; i < count; i++, w += 3 )
	{
		const u32 *p = (const u32 *)( verts + i * 5 + 2 );
		w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
	}

	/* texture s t in xy, lightmap s t in zw */
	*w++ = VIF_CODE( addr + 2, count, VIF_UNPACK_V4_32 );
	for( i = 0; i < count; i++, w += 4 )
	{
		const u32 *t = (const u32 *)( verts + i * 5 );
		const u32 *l = (const u32 *)luv[i];
		w[0] = t[0]; w[1] = t[1]; w[2] = l[0]; w[3] = l[1];
	}

	*w++ = VIF_CODE( ( addr + 3 ) | VIF_UNPACK_USN, count, VIF_UNPACK_V4_8 );
	color = convert_color( g.color );
	for( i = 0; i < count; i++ )
		*w++ = color;

	vu_end_primitive( w, count );

	{
		const unsigned cycles = cpu_ticks() - start;

		ps2_perf_cycles[PS2_PERF_DRAW] += cycles;
		ps2_gs_stats.kind_cycles[0] += cycles;
		ps2_gs_stats.kind_draws[0]++;
		ps2_gs_stats.kind_tris[0] += count - 2;
		ps2_gs_stats.kind_verts[0] += count;
		ps2_gs_stats.tris += count - 2;
		ps2_gs_stats.draws++;
	}
	return 1;
}

/* ---------- memory for vertices ---------- */

void *sceGuGetMemory( int size )
{
	u8 *p = g.arena_cur;

	size = ( size + 15 ) & ~15;
	if( p + size > g.arena + ARENA_BYTES )
	{
		printf( "[XASH-PS2] GS vertex arena full, %i bytes wanted\n", size );
		p = g.arena; /* older vertices were already sent */
	}
	g.arena_last = p;
	g.arena_cur = p + size;
	return p;
}

void *extGuBeginPacket( unsigned int *maxsize )
{
	if( maxsize )
		*maxsize = (unsigned int)( g.arena + ARENA_BYTES - g.arena_cur );
	g.arena_last = g.arena_cur;
	return g.arena_cur;
}

void extGuEndPacket( void *eaddr )
{
	g.arena_cur = (u8 *)(((u32)eaddr + 15) & ~15u);
}

void extGuStart( int cid, void *list, int size )
{
	(void)size;
	sceGuStart( cid, list );
}

/* ---------- frame ---------- */

static void frame_registers( void )
{
	const u32 fb = g.gs->ScreenBuffer[g.gs->ActiveBuffer & 1];

	g.frame_base = (u64)( fb >> 13 ) | ((u64)( g.gs->Width >> 6 ) << 16) | ((u64)g.gs->PSM << 24);
	g.cache[CACHE_FRAME] = g.frame_base;
	g.cache[CACHE_ZBUF] = (u64)( g.gs->ZBuffer >> 13 ) | ((u64)( g.gs->PSMZ & 0xf ) << 24);
	g.cache[CACHE_TEST] = (1 << 16) | (1 << 17);
	g.cache[CACHE_ALPHA] = 0;
	/* not sent here: an impossible value makes the next textured draw send them */
	g.cache[CACHE_TEX1] = ~(u64)0;
	g.cache[CACHE_CLAMP] = ~(u64)0;
	g.cache[CACHE_TEX0] = ~(u64)0;
	g.cache[CACHE_SCISSOR] = (u64)( g.gs->Width - 1 ) << 16 | (u64)( g.gs->Height - 1 ) << 48;
	g.ctx2_dirty = 1;

	gif_begin();
	write_ad_tag( 12 );
	write_ad( REG_FRAME_1, g.cache[CACHE_FRAME] );
	write_ad( REG_ZBUF_1, g.cache[CACHE_ZBUF] );
	write_ad( REG_XYOFFSET_1, (u64)g.gs->OffsetX | ((u64)g.gs->OffsetY << 32) );
	write_ad( REG_SCISSOR_1, g.cache[CACHE_SCISSOR] );
	write_ad( REG_TEST_1, g.cache[CACHE_TEST] );
	write_ad( REG_ALPHA_1, 0 );
	write_ad( REG_PRMODECONT, 1 );
	write_ad( REG_TEXA, (u64)0x80 << 32 ); /* CT16 alpha bit 1 means 1.0 */
	write_ad( REG_DTHE, 1 );
	write_ad( REG_COLCLAMP, 1 );
	write_ad( REG_PABE, 0 );
	write_ad( REG_FBA_1, 0 );
}

/* Runs before sceGuStart, on its own: reset, program, clip planes, VU1 state. */
static void vu_load_program( void )
{
	static const float planes[5][4] =
	{
		{ 0.0f, 0.0f, 1.0f, 1.0f },		/* near: z + w */
		{ -1.0f, 0.0f, 0.0f, GUARD_X },
		{ 1.0f, 0.0f, 0.0f, GUARD_X },
		{ 0.0f, -1.0f, 0.0f, GUARD_Y },
		{ 0.0f, 1.0f, 0.0f, GUARD_Y },
	};
	const u32 *code = &VU1Gs_CodeStart;
	const u32 instructions = (u32)( &VU1Gs_CodeEnd - &VU1Gs_CodeStart ) / 2;
	u32 done = 0;
	int i;

	__asm__ volatile( "ctc2 %0, $28\n\tsync.p" :: "r"( 0x0200 )); /* FBRST: reset VU1 */
	*VIF1_FBRST = 1;

	buffer_begin();
	write_vif( 0, 0, VIF_CODE( 0x0101, 0, VIF_STCYCL ), 0 ); /* CL = WL = 1 */

	while( done < instructions )
	{
		u32 n = instructions - done > 256 ? 256 : instructions - done;
		u32 *w;

		write_vif( 0, 0, 0, 0 ); /* the MPG data must start on 8 bytes, see below */
		g.p -= 2;
		w = (u32 *)g.p;
		w[0] = 0;
		w[1] = VIF_CODE( done, n == 256 ? 0 : n, VIF_MPG );
		memcpy( w + 2, code + done * 2, n * 8 );
		g.p += 1 + n;
		if(( (u32)g.p & 15 ) != 0 )
		{
			*g.p++ = 0; /* two VIF NOPs to end the quadword */
		}
		done += n;
	}

	write_vif( 0, 0, 0, VIF_CODE( VU_PLANES, 5, VIF_UNPACK_V4_32 ));
	memcpy( g.p, planes, sizeof( planes ));
	g.p += 10;
	write_vif( 0, 0, 0, VIF_CODE( VU_MSCAL_INIT, 0, VIF_MSCAL ));
	write_vif( 0, 0, 0, VIF_CODE( 0, 0, VIF_FLUSHE ));

	vif_dma_send( g.buf[g.buf_index], (u32)((u8 *)g.p - (u8 *)g.start) >> 4 );
	vif_idle_wait();
	printf( "[XASH-PS2] STAGE=VU1_READY instructions=%u\n", instructions );
}

void sceGuInit( void )
{
	int i;

	g.gs = PS2_GS_Global();
	g.buf[0] = memalign( 64, PACKET_BYTES );
	g.buf[1] = memalign( 64, PACKET_BYTES );
	g.arena = memalign( 64, ARENA_BYTES );
	g.convert = memalign( 64, CONVERT_BYTES );
	if( !g.buf[0] || !g.buf[1] || !g.arena || !g.convert )
	{
		printf( "[XASH-PS2] GS: no memory for packets\n" );
		return;
	}
	FlushCache( 0 ); /* no dirty line may land on the uncached writes */
	g.arena_cur = g.arena_last = g.arena;

	vram_clut = ( g.gs->CurrentPointer + 8191 ) & ~8191u;
	vram_tex_start = vram_clut + CLUT_SLOTS * CLUT_BYTES;
	vram_tex_end = 4 * 1024 * 1024;
	tex_cache_reset();

	for( i = 0; i < 4; i++ )
	{
		memset( g.mat[i], 0, sizeof( g.mat[i] ));
		g.mat[i][0][0] = g.mat[i][1][1] = g.mat[i][2][2] = g.mat[i][3][3] = 1.0f;
	}
	g.mvp_dirty = 1;
	g.tsx = g.tsy = 1.0f;
	g.zfar = 65535.0f;
	g.color = 0xffffffff;
	g.vhw = LOGICAL_W * 0.5f;
	g.vhh = LOGICAL_H * 0.5f;

	vu_load_program();

	printf( "[XASH-PS2] STAGE=GS_RENDER_READY clut=%u tex=%u free=%u\n", vram_clut, vram_tex_start, vram_tex_end - vram_tex_start );
}

static int flip_handler;

void sceGuTerm( void )
{
	flip_wait();
	if( flip_handler >= 0 )
		gsKit_remove_vsync_handler( flip_handler );
	flip_handler = -1;
	vif_idle_wait();
	dma_wait();
	free( g.buf[0] );
	free( g.buf[1] );
	free( g.arena );
	free( g.convert );
	memset( g.buf, 0, sizeof( g.buf ));
}

/* Deferred flip: the vblank interrupt shows the finished buffer, so the EE runs the next
 * frame's server and client work instead of waiting for vblank in sceGuSwapBuffers.
 * The first DMA of the next frame waits if that vblank has not come yet, because it
 * draws into the buffer still on screen. Same registers as gsKit_sync_flip; gs_gu.c
 * sends FRAME itself. */
static volatile u64 flip_dispfb;
static int flip_handler = -1;

static s32 flip_vblank( s32 cause )
{
	(void)cause;
	if( flip_pending )
	{
		*GS_DISPFB2 = flip_dispfb;
		flip_pending = 0;
	}
	ExitHandler();
	return -1;
}

static void flip_wait( void )
{
	const unsigned int start = cpu_ticks();

	PS2_PERF_BEGIN( FLIP );
	while( flip_pending )
	{
		/* no vblank in 100 ms: show it now rather than hang */
		if( cpu_ticks() - start > 29491200u )
		{
			*GS_DISPFB2 = flip_dispfb;
			flip_pending = 0;
		}
	}
	PS2_PERF_END( FLIP );
}

void sceGuStart( int cid, void *list )
{
	(void)cid; (void)list;
	g.arena_cur = g.arena_last = g.arena;
	g.tag = NULL;
	g.direct = NULL;
	g.vu_start = NULL;
	buffer_begin();
	frame_registers();
	g.tex_dirty = 1;
	g.state_dirty = 1;
}

int sceGuFinish( void )
{
	buffer_kick();
	return 0;
}

int sceGuSync( int mode, int what )
{
	PS2_PERF_BEGIN( SUBMIT );
	(void)mode; (void)what;
	vif_idle_wait();
	dma_wait();
	PS2_PERF_END( SUBMIT );
	return 0;
}

void *sceGuSwapBuffers( void )
{
	GSGLOBAL *gs = g.gs;

	if( flip_handler < 0 )
		flip_handler = gsKit_add_vsync_handler( flip_vblank );
	flip_wait();
	flip_dispfb = (u64)( gs->ScreenBuffer[gs->ActiveBuffer & 1] >> 13 )
		| ((u64)( gs->Width >> 6 ) << 9) | ((u64)gs->PSM << 15);
	gs->ActiveBuffer ^= 1;
	flip_pending = 1;
	return NULL;
}

int sceGuDisplay( int state ) { (void)state; return 0; }
int sceGuCheckList( void ) { return 0; }
void sceGuDrawBuffer( int psm, void *fbp, int fbw ) { (void)psm; (void)fbp; (void)fbw; }
void sceGuDispBuffer( int w, int h, void *p, int b ) { (void)w; (void)h; (void)p; (void)b; }
void sceGuDepthBuffer( void *zbp, int zbw ) { (void)zbp; (void)zbw; }
void guSwapBuffersBehaviour( int behaviour ) { (void)behaviour; }
int sceDisplayWaitVblankStart( void ) { return 0; }
void sceGuClearStencil( unsigned int stencil ) { (void)stencil; }
void sceGuClearDepth( unsigned int depth ) { (void)depth; }
void sceGuStencilFunc( int f, int r, int m ) { (void)f; (void)r; (void)m; }
void sceGuStencilOp( int f, int z, int p ) { (void)f; (void)z; (void)p; }
void sceGuPixelMask( unsigned int mask ) { (void)mask; }
void sceGuFog( float n, float f, unsigned int c ) { (void)n; (void)f; (void)c; }
void sceGuTexMode( int tpsm, int maxmips, int a2, int swizzle )
{
	(void)maxmips; (void)a2; (void)swizzle;
	if( g.tex_psm != tpsm )
		g.tex_dirty = 1;
	g.tex_psm = tpsm;
}
void sceGuTexLevelMode( unsigned int mode, float bias ) { (void)mode; (void)bias; }
void sceGuTexSlope( float slope ) { (void)slope; }
void sceGuClutMode( unsigned int c, unsigned int s, unsigned int m, unsigned int a ) { (void)c; (void)s; (void)m; (void)a; }

SceInt64 sceKernelGetSystemTimeWide( void )
{
	return (SceInt64)( cpu_ticks() / 295u ); /* wraps in 14.5 s; only spans are used */
}

void sceGuOffset( unsigned int x, unsigned int y )
{
	g.ofx = (float)x;
	g.ofy = (float)y;
}

void sceGuViewport( int cx, int cy, int width, int height )
{
	const float scale_x = (float)g.gs->Width / LOGICAL_W, scale_y = (float)g.gs->Height / LOGICAL_H;

	g.vcx = ( (float)cx - g.ofx ) * scale_x;
	g.vcy = ( (float)cy - g.ofy ) * scale_y;
	g.vhw = (float)width * 0.5f * scale_x;
	g.vhh = (float)height * 0.5f * scale_y;
	g.vu_params_dirty = 1;
}

void sceGuScissor( int x0, int y0, int x1, int y1 )
{
	const float scale_x = (float)g.gs->Width / LOGICAL_W, scale_y = (float)g.gs->Height / LOGICAL_H;
	u64 values[CACHE_COUNT];
	int sx0 = (int)( x0 * scale_x ), sy0 = (int)( y0 * scale_y );
	int sx1 = (int)( x1 * scale_x ) - 1, sy1 = (int)( y1 * scale_y ) - 1;

	if( sx1 < sx0 ) sx1 = sx0;
	if( sy1 < sy0 ) sy1 = sy0;

	memcpy( values, g.cache, sizeof( values ));
	values[CACHE_SCISSOR] = (u64)sx0 | ((u64)sx1 << 16) | ((u64)sy0 << 32) | ((u64)sy1 << 48);
	if( g.p )
		set_registers( values );
	g.ctx2_dirty = 1;
}

#define GU_SET( field, value ) do { if( g.field != ( value )) { g.field = ( value ); g.state_dirty = 1; } } while( 0 )

void sceGuEnable( int state ) { GU_SET( enabled, g.enabled | ( 1u << state )); }
void sceGuDisable( int state ) { GU_SET( enabled, g.enabled & ~( 1u << state )); }
int sceGuGetStatus( int state ) { return ( g.enabled >> state ) & 1; }
void sceGuColor( unsigned int color ) { g.color = color; }
void sceGuClearColor( unsigned int color ) { g.clear_color = color; }
void sceGuDepthFunc( int function ) { GU_SET( zfunc, function ); }
void sceGuDepthMask( int mask ) { GU_SET( zmask, mask ); }
void sceGuDepthOffset( unsigned int offset ) { g.zoffset = -(int)offset; g.vu_params_dirty = 1; }
void sceGuDepthRange( int near, int far ) { g.znear = (float)near; g.zfar = (float)far; g.vu_params_dirty = 1; }
void sceGuAlphaFunc( int func, int value, int mask ) { (void)mask; GU_SET( afunc, func ); GU_SET( aref, value ); }
void sceGuFrontFace( int order ) { g.front = order; }
void sceGuShadeModel( int mode ) { g.shade = mode; }
void sceGuTexFunc( int tfx, int tcc ) { GU_SET( tfx, tfx ); GU_SET( tcc, tcc ); }
void sceGuTexFilter( int min, int mag ) { GU_SET( fmin, min ); GU_SET( fmag, mag ); }
void sceGuTexWrap( int u, int v ) { GU_SET( wrapu, u ); GU_SET( wrapv, v ); }
void sceGuTexScale( float u, float v ) { g.tsx = u; g.tsy = v; }
void sceGuTexOffset( float u, float v ) { g.tox = u; g.toy = v; }

void sceGuBlendFunc( int op, int src, int dest, unsigned int srcfix, unsigned int destfix )
{
	(void)op;
	GU_SET( bsrc, src );
	GU_SET( bdst, dest );
	GU_SET( bfixs, srcfix );
	GU_SET( bfixd, destfix );
}

void sceGuTexImage( int mipmap, int width, int height, int tbw, const void *tbp )
{
	(void)tbw;
	if( mipmap )
		return; /* base level only: ref_gu builds no mipmaps on PS2 */
	if( g.tex_ptr != tbp || g.tex_w != width || g.tex_h != height )
		g.tex_dirty = 1;
	g.tex_ptr = tbp;
	g.tex_w = width;
	g.tex_h = height;
}

void sceGuClutLoad( int num_blocks, const void *cbp )
{
	(void)num_blocks;
	GU_SET( clut_ptr, cbp );
}

void sceGuClear( int flags )
{
	u64 values[CACHE_COUNT];
	const u32 color = g.clear_color;
	u32 w, h;

	if( !( flags & ( GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT )))
		return;

	w = g.gs->Width;
	h = g.gs->Height;

	memcpy( values, g.cache, sizeof( values ));
	values[CACHE_TEST] = (1 << 16) | (1 << 17);
	values[CACHE_ZBUF] = ( g.cache[CACHE_ZBUF] & 0xffffffffu ) | ((u64)(( flags & GU_DEPTH_BUFFER_BIT ) ? 0 : 1 ) << 32);
	values[CACHE_FRAME] = g.frame_base | ((u64)(( flags & GU_COLOR_BUFFER_BIT ) ? 0 : 0xffffffffu ) << 32);
	set_registers( values );

	g.textured = 0;
	batch_begin( PRIM_SPRITE );
	g.p[0] = color & 0xffffff;
	g.p[1] = pack_xyz( 0.0f, 0.0f, 0 );
	g.p[2] = color & 0xffffff;
	g.p[3] = pack_xyz( (float)w, (float)h, 0 );
	g.p += 4;
	g.batch_count += 2;

	values[CACHE_FRAME] = g.frame_base;
	set_registers( values );
	g.state_dirty = 1; /* TEST and ZBUF no longer match the state */
}

/* ---------- matrices ---------- */

void sceGumMatrixMode( int mode ) { g.mode = mode; }

void sceGumLoadIdentity( void )
{
	float (*m)[4] = g.mat[g.mode];

	memset( m, 0, sizeof( g.mat[0] ));
	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
	g.mvp_dirty = 1;
}

void sceGumLoadMatrix( const ScePspFMatrix4 *m )
{
	memcpy( g.mat[g.mode], m, sizeof( g.mat[0] ));
	g.mvp_dirty = 1;
}

void sceGumTranslate( const ScePspFVector3 *v )
{
	float (*m)[4] = g.mat[g.mode];
	int r;

	for( r = 0; r < 4; r++ )
		m[3][r] += m[0][r] * v->x + m[1][r] * v->y + m[2][r] * v->z;
	g.mvp_dirty = 1;
}

void sceGumScale( const ScePspFVector3 *v )
{
	float (*m)[4] = g.mat[g.mode];
	int r;

	for( r = 0; r < 4; r++ )
	{
		m[0][r] *= v->x;
		m[1][r] *= v->y;
		m[2][r] *= v->z;
	}
	g.mvp_dirty = 1;
}

void sceGumUpdateMatrix( void )
{
}
