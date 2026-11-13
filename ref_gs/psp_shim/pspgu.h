/* The part of the PSP GU API that ref_gu uses, implemented on the PS2 GS by
 * ref_gs/gs_gu.c. ref_gu compiles against these headers without pspsdk. */
#ifndef PS2_PSPGU_SHIM_H
#define PS2_PSPGU_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef int SceUID;
typedef long long SceInt64;
typedef unsigned int SceSize;

typedef struct { float x, y, z; } ScePspFVector3;
typedef struct { float x, y, z, w; } ScePspFVector4;
typedef struct { ScePspFVector4 x, y, z, w; } ScePspFMatrix4;

/* vertex format, same bit layout as the PSP */
#define GU_TEXTURE_8BIT     (1 << 0)
#define GU_TEXTURE_16BIT    (2 << 0)
#define GU_TEXTURE_32BITF   (3 << 0)
#define GU_TEXTURE_BITS     (3 << 0)
#define GU_COLOR_5650       (4 << 2)
#define GU_COLOR_5551       (5 << 2)
#define GU_COLOR_4444       (6 << 2)
#define GU_COLOR_8888       (7 << 2)
#define GU_COLOR_BITS       (7 << 2)
#define GU_NORMAL_8BIT      (1 << 5)
#define GU_NORMAL_16BIT     (2 << 5)
#define GU_NORMAL_32BITF    (3 << 5)
#define GU_NORMAL_BITS      (3 << 5)
#define GU_VERTEX_8BIT      (1 << 7)
#define GU_VERTEX_16BIT     (2 << 7)
#define GU_VERTEX_32BITF    (3 << 7)
#define GU_VERTEX_BITS      (3 << 7)
#define GU_INDEX_8BIT       (1 << 11)
#define GU_INDEX_16BIT      (2 << 11)
#define GU_INDEX_BITS       (3 << 11)
#define GU_TRANSFORM_3D     (0 << 23)
#define GU_TRANSFORM_2D     (1 << 23)

#define GU_POINTS           0
#define GU_LINES            1
#define GU_LINE_STRIP       2
#define GU_TRIANGLES        3
#define GU_TRIANGLE_STRIP   4
#define GU_TRIANGLE_FAN     5
#define GU_SPRITES          6

#define GU_ALPHA_TEST       0
#define GU_DEPTH_TEST       1
#define GU_SCISSOR_TEST     2
#define GU_STENCIL_TEST     3
#define GU_BLEND            4
#define GU_CULL_FACE        5
#define GU_DITHER           6
#define GU_FOG              7
#define GU_CLIP_PLANES      8
#define GU_TEXTURE_2D       9
#define GU_LIGHTING         10

#define GU_NEVER            0
#define GU_ALWAYS           1
#define GU_EQUAL            2
#define GU_NOTEQUAL         3
#define GU_LESS             4
#define GU_LEQUAL           5
#define GU_GREATER          6
#define GU_GEQUAL           7

#define GU_COLOR_BUFFER_BIT   1
#define GU_STENCIL_BUFFER_BIT 2
#define GU_DEPTH_BUFFER_BIT   4

#define GU_CW               0
#define GU_CCW              1
#define GU_FLAT             0
#define GU_SMOOTH           1
#define GU_FALSE            0
#define GU_TRUE             1

#define GU_TFX_MODULATE     0
#define GU_TFX_DECAL        1
#define GU_TFX_BLEND        2
#define GU_TFX_REPLACE      3
#define GU_TFX_ADD          4
#define GU_TCC_RGB          0
#define GU_TCC_RGBA         1

#define GU_ADD              0
#define GU_SRC_COLOR        0
#define GU_ONE_MINUS_SRC_COLOR 1
#define GU_SRC_ALPHA        2
#define GU_ONE_MINUS_SRC_ALPHA 3
#define GU_DST_COLOR        0
#define GU_ONE_MINUS_DST_COLOR 1
#define GU_DST_ALPHA        4
#define GU_ONE_MINUS_DST_ALPHA 5
#define GU_FIX              10

#define GU_NEAREST          0
#define GU_LINEAR           1
#define GU_NEAREST_MIPMAP_NEAREST 4
#define GU_LINEAR_MIPMAP_NEAREST  5
#define GU_NEAREST_MIPMAP_LINEAR  6
#define GU_LINEAR_MIPMAP_LINEAR   7

#define GU_REPEAT           0
#define GU_CLAMP            1

#define GU_PSM_5650         0
#define GU_PSM_5551         1
#define GU_PSM_4444         2
#define GU_PSM_8888         3
#define GU_PSM_T4           4
#define GU_PSM_T8           5
#define GU_PSM_T16          6
#define GU_PSM_T32          7
#define GU_PSM_DXT1         8
#define GU_PSM_DXT3         9
#define GU_PSM_DXT5         10

#define GU_PROJECTION       0
#define GU_VIEW             1
#define GU_TEXTURE          2
#define GU_MODEL            3

#define GU_DIRECT           0
#define GU_SYNC_FINISH      0
#define GU_SYNC_WAIT        0

#define GU_KEEP             0
#define GU_ZERO             1
#define GU_REPLACE          2
#define GU_INVERT           3
#define GU_INCR             4
#define GU_DECR             5

#define GU_ABGR(a,b,g,r)    (((a) << 24) | ((b) << 16) | ((g) << 8) | (r))
#define GU_RGBA(r,g,b,a)    GU_ABGR((a),(b),(g),(r))
#define GU_COLOR(r,g,b,a)   GU_RGBA((unsigned int)((r) * 255.0f),(unsigned int)((g) * 255.0f),(unsigned int)((b) * 255.0f),(unsigned int)((a) * 255.0f))

void sceGuInit( void );
void sceGuTerm( void );
void sceGuStart( int cid, void *list );
int sceGuFinish( void );
int sceGuSync( int mode, int what );
void *sceGuSwapBuffers( void );
int sceGuDisplay( int state );
int sceGuCheckList( void );
void *sceGuGetMemory( int size );
void sceGuDrawBuffer( int psm, void *fbp, int fbw );
void sceGuDispBuffer( int width, int height, void *dispbp, int dispbw );
void sceGuDepthBuffer( void *zbp, int zbw );
void sceGuOffset( unsigned int x, unsigned int y );
void sceGuViewport( int cx, int cy, int width, int height );
void sceGuScissor( int x0, int y0, int x1, int y1 );
void sceGuEnable( int state );
void sceGuDisable( int state );
int sceGuGetStatus( int state );
void sceGuClear( int flags );
void sceGuClearColor( unsigned int color );
void sceGuClearDepth( unsigned int depth );
void sceGuClearStencil( unsigned int stencil );
void sceGuColor( unsigned int color );
void sceGuDepthFunc( int function );
void sceGuDepthMask( int mask );
void sceGuDepthOffset( unsigned int offset );
void sceGuDepthRange( int near, int far );
void sceGuAlphaFunc( int func, int value, int mask );
void sceGuBlendFunc( int op, int src, int dest, unsigned int srcfix, unsigned int destfix );
void sceGuStencilFunc( int func, int ref, int mask );
void sceGuStencilOp( int fail, int zfail, int zpass );
void sceGuPixelMask( unsigned int mask );
void sceGuFog( float near, float far, unsigned int color );
void sceGuFrontFace( int order );
void sceGuShadeModel( int mode );
void sceGuTexFunc( int tfx, int tcc );
void sceGuTexFilter( int min, int mag );
void sceGuTexWrap( int u, int v );
void sceGuTexScale( float u, float v );
void sceGuTexOffset( float u, float v );
void sceGuTexMode( int tpsm, int maxmips, int a2, int swizzle );
void sceGuTexImage( int mipmap, int width, int height, int tbw, const void *tbp );
void sceGuTexLevelMode( unsigned int mode, float bias );
void sceGuTexSlope( float slope );
void sceGuClutMode( unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int a3 );
void sceGuClutLoad( int num_blocks, const void *cbp );
void sceGuDrawArray( int prim, int vtype, int count, const void *indices, const void *vertices );

void sceGumMatrixMode( int mode );
void sceGumLoadIdentity( void );
void sceGumLoadMatrix( const ScePspFMatrix4 *m );
void sceGumTranslate( const ScePspFVector3 *v );
void sceGumScale( const ScePspFVector3 *v );
void sceGumUpdateMatrix( void );

void guSwapBuffersBehaviour( int behaviour );
#define PSP_DISPLAY_SETBUF_IMMEDIATE 0
#define PSP_DISPLAY_SETBUF_NEXTFRAME 1

/* A texture changed in EE RAM: drop its copy in GS memory. */
void sceKernelDcacheWritebackRange( const void *p, unsigned int size );
void sceKernelDcacheWritebackAll( void );
SceInt64 sceKernelGetSystemTimeWide( void );
int sceDisplayWaitVblankStart( void );

int ps2_gu_studio_prim( int prim, const short *cmds, int count, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha );
/* A whole compatible mesh; zero selects the existing per-strip path. */
int ps2_gu_studio_mesh( const short *cmds, const float (*verts)[3], float s, float t, const unsigned int *rgb, unsigned int alpha );
/* Chrome uses floating-point UV coordinates indexed by each command's normal. */
int ps2_gu_studio_chrome_mesh( const short *cmds, const float (*verts)[3], const float (*chrome)[2], float s, float t, const unsigned int *rgb, unsigned int alpha );
/* World polygon with its lightmap in one VU1 batch; 0 means the caller draws both passes. */
int ps2_gu_world_fan( const float *verts, const float (*luv)[2], int count, const void *lm_pixels, const void *lm_palette, int lm_w, int lm_h, int lm_nearest );

/* Frame counters printed by engine/platform/ps2/perf_ps2.c. */
typedef struct
{
	unsigned tris, verts, draws, uploads, upload_bytes;
	/* per kind of 3D draw: world, lightmap pass, colored vertices (models, effects) */
	unsigned kind_tris[3], kind_verts[3], kind_draws[3];
	unsigned long long kind_cycles[3];
} ps2_gs_stats_t;
extern ps2_gs_stats_t ps2_gs_stats;

#endif /* PS2_PSPGU_SHIM_H */
