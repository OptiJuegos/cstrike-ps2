/*
gl_image.c - texture uploading and processing
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "r_local.h"

#if XASH_PS2
/* Original filesystem and renderer are statically linked on PS2. */
extern file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly );
extern fs_offset_t FS_Read( file_t *file, void *buffer, size_t buffersize );
extern fs_offset_t FS_FileLength( file_t *file );
extern int FS_Close( file_t *file );
#endif

#define TEXTURES_HASH_SIZE	(MAX_TEXTURES >> 2)

static image_t		r_images[MAX_TEXTURES];
static image_t*	r_imagesHashTable[TEXTURES_HASH_SIZE];
static uint		r_numImages;

#if XASH_PS2
/* Original 640hud*.spr images are 256x256 8-bit indexed. The full-size
 * 16-bit color and alpha copies cost 256 KiB per atlas even though the HUD
 * uses one sprite at a time. Preserve every original pixel index and palette
 * in the texture's own allocation, materializing into one shared pair of
 * scratch images only for the synchronous software drawing operation. */
typedef struct ps2_hudindexed_s
{
	byte indices[256 * 256];
	pixel_t color[256];
	pixel_t alpha[256];
} ps2_hudindexed_t;

static pixel_t ps2_hudPixels[256 * 256];
static pixel_t ps2_hudAlphaPixels[256 * 256];
static image_t *ps2_hudMaterialized;

void R_PS2PrepareIndexedHud( image_t *tex )
{
	ps2_hudindexed_t *hud;
	uint i;

	if( !tex || !tex->ps2HudIndexed || tex == ps2_hudMaterialized ) return;
	hud = (ps2_hudindexed_t *)tex->ps2HudIndexed;
	if( ps2_hudMaterialized )
	{
		ps2_hudMaterialized->pixels[0] = NULL;
		ps2_hudMaterialized->alpha_pixels = NULL;
	}
	for( i = 0; i < 256 * 256; i++ )
	{
		byte index = hud->indices[i];
		ps2_hudPixels[i] = hud->color[index];
		if( FBitSet( tex->flags, TF_HAS_ALPHA ))
			ps2_hudAlphaPixels[i] = hud->alpha[index];
	}
	tex->pixels[0] = ps2_hudPixels;
	tex->alpha_pixels = FBitSet( tex->flags, TF_HAS_ALPHA ) ? ps2_hudAlphaPixels : NULL;
	ps2_hudMaterialized = tex;
}
#endif

#define IsLightMap( tex )	( FBitSet(( tex )->flags, TF_ATLAS_PAGE ))
/*
=================
R_GetTexture

acess to array elem
=================
*/
image_t *R_GetTexture( unsigned int texnum )
{
	ASSERT( texnum >= 0 && texnum < MAX_TEXTURES );
	return &r_images[texnum];
}

/*
=================
GL_Bind
=================
*/
void GAME_EXPORT GL_Bind( int tmu, unsigned int texnum )
{
	image_t	*image;

	extern void	(*d_pdrawspans)(void *);
	extern void R_PolysetFillSpans8 ( void * );
	extern void R_PolysetDrawSpansConstant8_33( void *pspanpackage);
	extern void R_PolysetDrawSpansTextureBlended( void *pspanpackage);
	extern void R_PolysetDrawSpansBlended( void *pspanpackage);
	extern void R_PolysetDrawSpansAdditive( void *pspanpackage);
	extern void R_PolysetDrawSpansGlow( void *pspanpackage);

	image = &r_images[texnum];
	#if XASH_PS2
	R_PS2PrepareIndexedHud( image );
	#endif
	//vid.rendermode = kRenderNormal;

	if( vid.rendermode == kRenderNormal )
	{
		r_affinetridesc.pskin = image->pixels[0];
		d_pdrawspans = R_PolysetFillSpans8 ;
	}
	else if( vid.rendermode == kRenderTransAdd)
	{
		r_affinetridesc.pskin = image->pixels[0];
		d_pdrawspans = R_PolysetDrawSpansAdditive;
	}
	else if( vid.rendermode == kRenderGlow )
	{
		r_affinetridesc.pskin = image->pixels[0];
		d_pdrawspans = R_PolysetDrawSpansGlow;
	}
	else if( image->alpha_pixels )
	{
		r_affinetridesc.pskin = image->alpha_pixels;
		d_pdrawspans = R_PolysetDrawSpansTextureBlended;
	}
	else
	{
		r_affinetridesc.pskin = image->pixels[0];
		d_pdrawspans = R_PolysetDrawSpansBlended;
	}

	r_affinetridesc.skinwidth = image->width;
	r_affinetridesc.skinheight = image->height;
}

/*
=================
GL_ApplyTextureParams
=================
*/
void GL_ApplyTextureParams( image_t *tex )
{

	Assert( tex != NULL );
}

/*
=================
GL_UpdateTextureParams
=================
*/
static void GL_UpdateTextureParams( int iTexture )
{
	image_t	*tex = &r_images[iTexture];

	Assert( tex != NULL );

	if( !tex->pixels) return; // free slot

	GL_Bind( XASH_TEXTURE0, iTexture );
}

/*
=================
R_SetTextureParameters
=================
*/
void R_SetTextureParameters( void )
{
	int	i;

	// change all the existing mipmapped texture objects
	for( i = 0; i < r_numImages; i++ )
		GL_UpdateTextureParams( i );
}


/*
==================
GL_CalcImageSize
==================
*/
static size_t GL_CalcImageSize( pixformat_t format, int width, int height, int depth )
{
	size_t	size = 0;

	// check the depth error
	depth = Q_max( 1, depth );

	switch( format )
	{
	case PF_RGB_24:
	case PF_BGR_24:
		size = width * height * depth * 3;
		break;
	case PF_BGRA_32:
	case PF_RGBA_32:
		size = width * height * depth * 4;
		break;
	case PF_DXT1:
		size = (((width + 3) >> 2) * ((height + 3) >> 2) * 8) * depth;
		break;
	case PF_DXT3:
	case PF_DXT5:
	case PF_ATI2:
		size = (((width + 3) >> 2) * ((height + 3) >> 2) * 16) * depth;
		break;
	}

	return size;
}

/*
==================
GL_CalcTextureSize
==================
*/
static size_t GL_CalcTextureSize( int width, int height, int depth )
{
	return width * height * 2;
}

static int GL_CalcMipmapCount( image_t *tex, qboolean haveBuffer )
{
	int	width, height;
	int	mipcount;

	Assert( tex != NULL );

	if( !haveBuffer )
		return 1;

	// generate mip-levels by user request
	if( FBitSet( tex->flags, TF_NOMIPMAP ))
		return 1;
		
	// mip-maps can't exceeds 4
	for( mipcount = 0; mipcount < 4; mipcount++ )
	{
		width = Q_max( 1, ( tex->width >> mipcount ));
		height = Q_max( 1, ( tex->height >> mipcount ));
		if( width == 1 && height == 1 )
			break;
	}

	return mipcount + 1;
}

/*
================
GL_SetTextureDimensions
================
*/
static void GL_SetTextureDimensions( image_t *tex, int width, int height, int depth )
{
	int	maxTextureSize = 1024;
	int	maxDepthSize = 1;

	Assert( tex != NULL );

	#if XASH_PS2
	// ReGameDLL precaches every weapon's Studio skin at map startup. With a
	// 32 MiB EE budget, a maximum of 64 px per Studio skin is the low-memory
	// startup preset. All geometry, original MDLs, WADs and menu art remain
	// unchanged; only the renderer's in-memory skin cache is downsampled.
	if( !Q_strnicmp( tex->name, "#models/", 8 ))
	{
		// Only the view weapon and people are seen up close; dropped weapons,
		// weapons in other hands, gibs and props are a few pixels on screen.
		if( !Q_strnicmp( tex->name, "#models/v_", 10 ) ||
			!Q_strnicmp( tex->name, "#models/player", 14 ) ||
			!Q_strnicmp( tex->name, "#models/hostage", 15 ))
			maxTextureSize = 64;
		else maxTextureSize = 32;
	}
	// Particle/effect sprites are numerous (hundreds of separate frames).
	// The long wall-impact/smoke animations have a smaller in-memory cache
	// while preserving every original frame and its transparency layer.
	else if( !Q_strnicmp( tex->name, "#sprites/wall_puff", 18 ) ||
		!Q_strnicmp( tex->name, "#sprites/black_smoke", 20 )) maxTextureSize = 16;
	else if( !Q_strnicmp( tex->name, "#sprites/", 9 )) maxTextureSize = 24;
	// BSP/WAD textures retain their original source assets and mip-chain;
	// 64px software copies are the PS2 low-memory preset. At 480x272 this
	// frees EE RAM for full-resolution 256x256 HUD sprites without dropping
	// map faces, lightmaps, materials, or models.
	else if( tex->type == it_wall ) maxTextureSize = 64;
	// The 800x600 menu backdrop stays in memory during the game; at 480 px wide
	// each 256 px tile covers about 154 screen pixels.
	else if( !Q_strnicmp( tex->name, "resource/background/", 20 )) maxTextureSize = 128;
#endif


	// store original sizes
	tex->srcWidth = width;
	tex->srcHeight = height;

	if( width > maxTextureSize || height > maxTextureSize || depth > maxDepthSize )
	{
		while( width > maxTextureSize || height > maxTextureSize )
		{
			width >>= 1;
			height >>= 1;
		}
	}

	// set the texture dimensions
	tex->width = Q_max( 1, width );
	tex->height = Q_max( 1, height );
	tex->depth = Q_max( 1, depth );
}

/*
===============
GL_SetTextureTarget
===============
*/
static void GL_SetTextureTarget( image_t *tex, rgbdata_t *pic )
{
	Assert( pic != NULL );
	Assert( tex != NULL );

	// correct depth size
	pic->depth = Q_max( 1, pic->depth );
	tex->numMips = 0; // begin counting

	// correct mip count
	pic->numMips = Q_max( 1, pic->numMips );
}

/*
===============
GL_SetTextureFormat
===============
*/
static void GL_SetTextureFormat( image_t *tex, pixformat_t format, int channelMask )
{
	qboolean	haveColor = ( channelMask & IMAGE_HAS_COLOR );
	qboolean	haveAlpha = ( channelMask & IMAGE_HAS_ALPHA );

	Assert( tex != NULL );
	//tex->transparent = !!( channelMask & IMAGE_HAS_ALPHA );
}

#if XASH_PS2
/* ReGameDLL precaches all Studio skins, even for weapons not currently held.
 * ImageLib's regular route inflates each indexed skin to a 256 KiB RGBA
 * buffer; ref_soft immediately converts that into its 16-bit pixel format.
 * Convert the indexed image directly instead, keeping the original palette
 * and a single mip level. Embedded indexed sprite images also benefit, but
 * are never resolution limited. Ordinary WAD/world/menu textures still use
 * their existing processing and mipmaps. */
static qboolean GL_UploadPS2StudioIndexed( image_t *tex, const rgbdata_t *pic )
{
	const byte *indices = pic->buffer;
	const byte *palette = pic->palette;
	uint x, y, w = tex->width, h = tex->height;
	const uint srcw = pic->width, srch = pic->height;
	static uint converted = 0;

	if( !indices || !palette || !srcw || !srch || !w || !h ) return false;
	if( !Q_strnicmp( tex->name, "#HUD/sprites/640hud", 19 ) &&
		srcw == 256 && srch == 256 && w == 256 && h == 256 )
	{
		ps2_hudindexed_t *hud = (ps2_hudindexed_t *)Mem_Calloc( r_temppool, sizeof( *hud ));
		uint i;
		memcpy( hud->indices, indices, sizeof( hud->indices ));
		for( i = 0; i < 256; i++ )
		{
			const byte *rgba = palette + i * 4;
			uint r = rgba[0] * BIT(5) / 256;
			uint g = rgba[1] * BIT(6) / 256;
			uint b = rgba[2] * BIT(5) / 256;
			uint major = (((r >> 2) & MASK(3)) << 5) |
				(((g >> 3) & MASK(3)) << 2) | ((b >> 3) & MASK(2));
			uint minor = MOVE_BIT(r,1,5) | MOVE_BIT(r,0,2) |
				MOVE_BIT(g,2,7) | MOVE_BIT(g,1,4) | MOVE_BIT(g,0,1) |
				MOVE_BIT(b,2,6) | MOVE_BIT(b,1,3) | MOVE_BIT(b,0,0);
			uint alpha = (rgba[3] * 8 / 256) << (16 - 3);
			pixel_t color = (major << 8) | (minor & 0xff);
			hud->alpha[i] = (color >> 3) | alpha;
			if( FBitSet( tex->flags, TF_HAS_ALPHA ) && !sw_noalphabrushes->value &&
				rgba[3] < 128 && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
				color = TRANSPARENT_COLOR;
			hud->color[i] = color;
		}
		tex->ps2HudIndexed = hud;
		tex->numMips = 1;
		tex->size += sizeof( *hud );
		converted++;
		printf( "[XASH-PS2] STAGE=HUD_INDEXED_COMPACT name=%s bytes=%u\n",
			tex->name, (unsigned)sizeof( *hud ));
		return true;
	}
	tex->pixels[0] = (pixel_t *)Mem_Calloc( r_temppool, w * h * sizeof(pixel_t) );
	if( FBitSet( tex->flags, TF_HAS_ALPHA ))
		tex->alpha_pixels = (pixel_t *)Mem_Calloc( r_temppool, w * h * sizeof(pixel_t) );

	for( y = 0; y < h; y++ )
	{
		for( x = 0; x < w; x++ )
		{
			uint r = 0, g = 0, b = 0, a = 0, j, count;
			uint major, minor, index = y * w + x;
			/* Match GL_ResampleTexture's 2x2 box filter for scaled skins;
			 * when not scaled, preserve every original palette index. */
			count = (srcw == w && srch == h) ? 1 : 4;
			for( j = 0; j < count; j++ )
			{
				uint sx, sy, palIndex;
				const byte *rgba;
				if( count == 1 )
				{
					sx = x;
					sy = y;
				}
				else
				{
					/* The original filter samples at 1/4 and 3/4 texel
					 * offsets horizontally and vertically. */
					sx = (uint)(((uint64_t)(4 * x + (j & 1 ? 3 : 1)) * srcw) / (4 * w));
					sy = (uint)(((uint64_t)(4 * y + (j & 2 ? 3 : 1)) * srch) / (4 * h));
					if( sx >= srcw ) sx = srcw - 1;
					if( sy >= srch ) sy = srch - 1;
				}
				palIndex = indices[sy * srcw + sx];
				rgba = palette + palIndex * 4;
				r += rgba[0]; g += rgba[1]; b += rgba[2]; a += rgba[3];
			}
			r = (r / count) * BIT(5) / 256;
			g = (g / count) * BIT(6) / 256;
			b = (b / count) * BIT(5) / 256;
			major = (((r >> 2) & MASK(3)) << 5) |
				(((g >> 3) & MASK(3)) << 2) | ((b >> 3) & MASK(2));
			minor = MOVE_BIT(r,1,5) | MOVE_BIT(r,0,2) |
				MOVE_BIT(g,2,7) | MOVE_BIT(g,1,4) | MOVE_BIT(g,0,1) |
				MOVE_BIT(b,2,6) | MOVE_BIT(b,1,3) | MOVE_BIT(b,0,0);
			tex->pixels[0][index] = (major << 8) | (minor & 0xFF);
			if( tex->alpha_pixels )
			{
				uint alpha = ((a / count) * 8 / 256) << (16 - 3);
				tex->alpha_pixels[index] = (tex->pixels[0][index] >> 3) | alpha;
				if( !sw_noalphabrushes->value && (a / count) < 128 &&
					FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
					tex->pixels[0][index] = TRANSPARENT_COLOR;
			}
		}
	}
	tex->numMips = 1;
	tex->size += GL_CalcTextureSize( w, h, tex->depth );
	converted++;
	if( converted == 1 || (converted % 25) == 0 )
		printf( "[XASH-PS2] STAGE=STUDIO_INDEXED_DIRECT count=%u last=%s src=%ux%u dst=%ux%u\n",
			converted, tex->name, srcw, srch, w, h );
	return true;
}
#endif

/*
=================
GL_ResampleTexture

Assume input buffer is RGBA
=================
*/
byte *GL_ResampleTexture( const byte *source, int inWidth, int inHeight, int outWidth, int outHeight, qboolean isNormalMap )
{
	uint		frac, fracStep;
	uint		*in = (uint *)source;
	uint		p1[0x1000], p2[0x1000];
	byte		*pix1, *pix2, *pix3, *pix4;
	uint		*out, *inRow1, *inRow2;
	static byte	*scaledImage = NULL;	// pointer to a scaled image
	vec3_t		normal;
	int		i, x, y;

	if( !source ) return NULL;

	scaledImage = Mem_Realloc( r_temppool, scaledImage, outWidth * outHeight * 4 );
	fracStep = inWidth * 0x10000 / outWidth;
	out = (uint *)scaledImage;

	frac = fracStep >> 2;
	for( i = 0; i < outWidth; i++ )
	{
		p1[i] = 4 * (frac >> 16);
		frac += fracStep;
	}

	frac = (fracStep >> 2) * 3;
	for( i = 0; i < outWidth; i++ )
	{
		p2[i] = 4 * (frac >> 16);
		frac += fracStep;
	}

	if( isNormalMap )
	{
		for( y = 0; y < outHeight; y++, out += outWidth )
		{
			inRow1 = in + inWidth * (int)(((float)y + 0.25f) * inHeight / outHeight);
			inRow2 = in + inWidth * (int)(((float)y + 0.75f) * inHeight / outHeight);

			for( x = 0; x < outWidth; x++ )
			{
				pix1 = (byte *)inRow1 + p1[x];
				pix2 = (byte *)inRow1 + p2[x];
				pix3 = (byte *)inRow2 + p1[x];
				pix4 = (byte *)inRow2 + p2[x];

				normal[0] = MAKE_SIGNED( pix1[0] ) + MAKE_SIGNED( pix2[0] ) + MAKE_SIGNED( pix3[0] ) + MAKE_SIGNED( pix4[0] );
				normal[1] = MAKE_SIGNED( pix1[1] ) + MAKE_SIGNED( pix2[1] ) + MAKE_SIGNED( pix3[1] ) + MAKE_SIGNED( pix4[1] );
				normal[2] = MAKE_SIGNED( pix1[2] ) + MAKE_SIGNED( pix2[2] ) + MAKE_SIGNED( pix3[2] ) + MAKE_SIGNED( pix4[2] );

				if( !VectorNormalizeLength( normal ))
					VectorSet( normal, 0.5f, 0.5f, 1.0f );

				((byte *)(out+x))[0] = 128 + (byte)(127.0f * normal[0]);
				((byte *)(out+x))[1] = 128 + (byte)(127.0f * normal[1]);
				((byte *)(out+x))[2] = 128 + (byte)(127.0f * normal[2]);
				((byte *)(out+x))[3] = 255;
			}
		}
	}
	else
	{
		for( y = 0; y < outHeight; y++, out += outWidth )
		{
			inRow1 = in + inWidth * (int)(((float)y + 0.25f) * inHeight / outHeight);
			inRow2 = in + inWidth * (int)(((float)y + 0.75f) * inHeight / outHeight);

			for( x = 0; x < outWidth; x++ )
			{
				pix1 = (byte *)inRow1 + p1[x];
				pix2 = (byte *)inRow1 + p2[x];
				pix3 = (byte *)inRow2 + p1[x];
				pix4 = (byte *)inRow2 + p2[x];

				((byte *)(out+x))[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0]) >> 2;
				((byte *)(out+x))[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1]) >> 2;
				((byte *)(out+x))[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2]) >> 2;
				((byte *)(out+x))[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3]) >> 2;
			}
		}
	}

	return scaledImage;
}

/*
=================
GL_BoxFilter3x3

box filter 3x3
=================
*/
void GL_BoxFilter3x3( byte *out, const byte *in, int w, int h, int x, int y )
{
	int		r = 0, g = 0, b = 0, a = 0;
	int		count = 0, acount = 0;
	int		i, j, u, v;
	const byte	*pixel;

	for( i = 0; i < 3; i++ )
	{
		u = ( i - 1 ) + x;

		for( j = 0; j < 3; j++ )
		{
			v = ( j - 1 ) + y;

			if( u >= 0 && u < w && v >= 0 && v < h )
			{
				pixel = &in[( u + v * w ) * 4];

				if( pixel[3] != 0 )
				{
					r += pixel[0];
					g += pixel[1];
					b += pixel[2];
					a += pixel[3];
					acount++;
				}
			}
		}
	}

	if(  acount == 0 )
		acount = 1;

	out[0] = r / acount;
	out[1] = g / acount;
	out[2] = b / acount;
//	out[3] = (int)( SimpleSpline( ( a / 12.0f ) / 255.0f ) * 255 );
}

/*
=================
GL_ApplyFilter

Apply box-filter to 1-bit alpha
=================
*/
byte *GL_ApplyFilter( const byte *source, int width, int height )
{
	byte	*in = (byte *)source;
	byte	*out = (byte *)source;
	int	i;

	if( ENGINE_GET_PARM( PARM_QUAKE_COMPATIBLE ) )
		return in;

	for( i = 0; source && i < width * height; i++, in += 4 )
	{
		if( in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0 )
			GL_BoxFilter3x3( in, source, width, height, i % width, i / width );
	}

	return out;
}

/*
=================
GL_BuildMipMap

Operates in place, quartering the size of the texture
=================
*/
static void GL_BuildMipMap( byte *in, int srcWidth, int srcHeight, int srcDepth, int flags )
{
	byte	*out = in;
	int	instride = ALIGN( srcWidth * 4, 1 );
	int	mipWidth, mipHeight, outpadding;
	int	row, x, y, z;
	vec3_t	normal;

	if( !in ) return;

	mipWidth = Q_max( 1, ( srcWidth >> 1 ));
	mipHeight = Q_max( 1, ( srcHeight >> 1 ));
	outpadding = ALIGN( mipWidth * 4, 1 ) - mipWidth * 4;
	row = srcWidth << 2;

	if( FBitSet( flags, TF_ALPHACONTRAST ))
	{
		memset( in, mipWidth, mipWidth * mipHeight * 4 );
		return;
	}

	// move through all layers
	for( z = 0; z < srcDepth; z++ )
	{
		if( FBitSet( flags, TF_NORMALMAP ))
		{
			for( y = 0; y < mipHeight; y++, in += instride * 2, out += outpadding )
			{
				byte *next = ((( y << 1 ) + 1 ) < srcHeight ) ? ( in + instride ) : in;
				for( x = 0, row = 0; x < mipWidth; x++, row += 8, out += 4 )
				{
					if((( x << 1 ) + 1 ) < srcWidth )
					{
						normal[0] = MAKE_SIGNED( in[row+0] ) + MAKE_SIGNED( in[row+4] )
						+ MAKE_SIGNED( next[row+0] ) + MAKE_SIGNED( next[row+4] );
						normal[1] = MAKE_SIGNED( in[row+1] ) + MAKE_SIGNED( in[row+5] )
						+ MAKE_SIGNED( next[row+1] ) + MAKE_SIGNED( next[row+5] );
						normal[2] = MAKE_SIGNED( in[row+2] ) + MAKE_SIGNED( in[row+6] )
						+ MAKE_SIGNED( next[row+2] ) + MAKE_SIGNED( next[row+6] );
					}
					else
					{
						normal[0] = MAKE_SIGNED( in[row+0] ) + MAKE_SIGNED( next[row+0] );
						normal[1] = MAKE_SIGNED( in[row+1] ) + MAKE_SIGNED( next[row+1] );
						normal[2] = MAKE_SIGNED( in[row+2] ) + MAKE_SIGNED( next[row+2] );
					}

					if( !VectorNormalizeLength( normal ))
						VectorSet( normal, 0.5f, 0.5f, 1.0f );

					out[0] = 128 + (byte)(127.0f * normal[0]);
					out[1] = 128 + (byte)(127.0f * normal[1]);
					out[2] = 128 + (byte)(127.0f * normal[2]);
					out[3] = 255;
				}
			}
		}
		else
		{
			for( y = 0; y < mipHeight; y++, in += instride * 2, out += outpadding )
			{
				byte *next = ((( y << 1 ) + 1 ) < srcHeight ) ? ( in + instride ) : in;
				for( x = 0, row = 0; x < mipWidth; x++, row += 8, out += 4 )
				{
					if((( x << 1 ) + 1 ) < srcWidth )
					{
						out[0] = (in[row+0] + in[row+4] + next[row+0] + next[row+4]) >> 2;
						out[1] = (in[row+1] + in[row+5] + next[row+1] + next[row+5]) >> 2;
						out[2] = (in[row+2] + in[row+6] + next[row+2] + next[row+6]) >> 2;
						out[3] = (in[row+3] + in[row+7] + next[row+3] + next[row+7]) >> 2;
					}
					else
					{
						out[0] = (in[row+0] + next[row+0]) >> 1;
						out[1] = (in[row+1] + next[row+1]) >> 1;
						out[2] = (in[row+2] + next[row+2]) >> 1;
						out[3] = (in[row+3] + next[row+3]) >> 1;
					}
				}
			}
		}
	}
}

/*
===============
GL_UploadTexture

upload texture into video memory
===============
*/
static qboolean GL_UploadTexture( image_t *tex, rgbdata_t *pic )
{
	byte		*buf, *data;
	size_t		texsize, size;
	uint		width, height;
	uint		i, j, numSides;
	uint		offset = 0;
	qboolean		normalMap = false;
	const byte	*bufend;
	int mipCount;

	tex->fogParams[0] = pic->fogParams[0];
	tex->fogParams[1] = pic->fogParams[1];
	tex->fogParams[2] = pic->fogParams[2];
	tex->fogParams[3] = pic->fogParams[3];
	GL_SetTextureDimensions( tex, pic->width, pic->height, pic->depth );
	GL_SetTextureFormat( tex, pic->type, pic->flags );

	//gEngfuncs.Con_Printf("%s %d %d\n", tex->name, tex->width, tex->height );

	Assert( pic != NULL );
	Assert( tex != NULL );

	if( !pic->buffer )
		return true;
	#if XASH_PS2
	if( tex->name[0] == '#' && pic->type == PF_INDEXED_32 && pic->palette )
		return GL_UploadPS2StudioIndexed( tex, pic );
	#endif

	buf = pic->buffer;

	mipCount = 4;//GL_CalcMipmapCount( tex, ( buf != NULL ));
	#if XASH_PS2
	/* HUD and UI requests with TF_NOMIPMAP do not need four pixel copies. */
	if( FBitSet( tex->flags, TF_NOMIPMAP )) mipCount = 1;
	// ReGameDLL precaches every CS weapon skin. Studio mesh rasterization
	// samples pixels[0] exclusively; unlike BSP surfaces it never uses the
	// other three mip levels. Do not allocate these unused copies on PS2.
	// Studio/sprite textures can use one level; world/WAD textures can
	// also start with '#map:' and still need mips for software surfaces.
	if( tex->name[0] == '#' && tex->type != it_wall ) mipCount = 1;
	#endif

	// NOTE: only single uncompressed textures can be resamples, no mips, no layers, no sides
	if((( pic->width != tex->width ) || ( pic->height != tex->height )))
		data = GL_ResampleTexture( buf, pic->width, pic->height, tex->width, tex->height, normalMap );
	else data = buf;

	//if( !ImageDXT( pic->type ) && !FBitSet( tex->flags, TF_NOMIPMAP ) && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
	//	data = GL_ApplyFilter( data, tex->width, tex->height );

	// mips will be auto-generated if desired
	for( j = 0; j < mipCount; j++ )
	{
	int x, y;
		width = Q_max( 1, ( tex->width >> j ));
		height = Q_max( 1, ( tex->height >> j ));
		texsize = GL_CalcTextureSize( width, height, tex->depth );
		size = GL_CalcImageSize( pic->type, width, height, tex->depth );
		//GL_TextureImageRAW( tex, i, j, width, height, tex->depth, pic->type, data );
		// increase size to workaround triangle renderer bugs
		// it seems to assume memory readable. maybe it was pointed to WAD?
		//tex->pixels[j] = (byte*)Mem_Calloc( r_temppool, width * height * sizeof(pixel_t) + 1024 ) + 512;
		tex->pixels[j] = (pixel_t*)Mem_Calloc( r_temppool, width * height * sizeof(pixel_t) );


		//memset( (byte*)tex->pixels[j] - 512, 0xFF, 512 );
		//memset( (byte*)tex->pixels[j] + width * height * sizeof(pixel_t), 0xFF, 512 );

		if( j == 0 &&  tex->flags & TF_HAS_ALPHA )
			tex->alpha_pixels = (pixel_t*)Mem_Calloc( r_temppool, width * height * sizeof(pixel_t) );
	
		for(i = 0; i < height * width; i++ )
		{
				unsigned int r, g, b, major, minor;
		#if 0
				r = data[i * 4 + 0] * MASK(5-1) / 255;
				g = data[i * 4 + 1] * MASK(6-1) / 255;
				b = data[i * 4 + 2] * MASK(5-1) / 255;
		#else
				// seems to look better
				r = data[i * 4 + 0] * BIT(5) / 256;
				g = data[i * 4 + 1] * BIT(6) / 256;
				b = data[i * 4 + 2] * BIT(5) / 256;
		#endif
				// 565 to 332
				major = (((r >> 2) & MASK(3)) << 5) |( (( (g >> 3) & MASK(3)) << 2 )  )| (((b >> 3) & MASK(2)));

				// save minor GBRGBRGB
				minor = MOVE_BIT(r,1,5) | MOVE_BIT(r,0,2) | MOVE_BIT(g,2,7) | MOVE_BIT(g,1,4) | MOVE_BIT(g,0,1) | MOVE_BIT(b,2,6)| MOVE_BIT(b,1,3)|MOVE_BIT(b,0,0);

				tex->pixels[j][i] = major << 8 | (minor & 0xFF);
				if( j == 0 && tex->alpha_pixels )
				{
					unsigned int alpha = (data[i * 4 + 3] * 8 / 256) << (16 - 3);
					tex->alpha_pixels[i] = (tex->pixels[j][i] >> 3) | alpha;
					if( !sw_noalphabrushes->value && data[i * 4 + 3] < 128 && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ) )
						tex->pixels[j][i] = TRANSPARENT_COLOR; //0000 0011 0100 1001;
				}

		}

		if( mipCount > 1 )
			GL_BuildMipMap( data, width, height, tex->depth, tex->flags );

		tex->size += texsize;
		tex->numMips++;

		//GL_CheckTexImageError( tex );
	}

#if 0


	GL_SetTextureTarget( tex, pic ); // must be first

	// make sure what target is correct
	if( tex->target == GL_NONE )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "GL_UploadTexture: %s is not supported by your hardware\n", tex->name );
		return false;
	}

	GL_SetTextureDimensions( tex, pic->width, pic->height, pic->depth );
	GL_SetTextureFormat( tex, pic->type, pic->flags );

	tex->fogParams[0] = pic->fogParams[0];
	tex->fogParams[1] = pic->fogParams[1];
	tex->fogParams[2] = pic->fogParams[2];
	tex->fogParams[3] = pic->fogParams[3];

	if(( pic->width * pic->height ) & 3 )
	{
		// will be resampled, just tell me for debug targets
		gEngfuncs.Con_Reportf( "GL_UploadTexture: %s s&3 [%d x %d]\n", tex->name, pic->width, pic->height );
	}

	buf = pic->buffer;
	bufend = pic->buffer + pic->size; // total image size include all the layers, cube sides, mipmaps
	offset = GL_CalcImageSize( pic->type, pic->width, pic->height, pic->depth );
	texsize = GL_CalcTextureSize( tex->format, tex->width, tex->height, tex->depth );
	normalMap = FBitSet( tex->flags, TF_NORMALMAP ) ? true : false;
	numSides = FBitSet( pic->flags, IMAGE_CUBEMAP ) ? 6 : 1;

	// uploading texture into video memory, change the binding
	glState.currentTextures[glState.activeTMU] = tex->texnum;
	pglBindTexture( tex->target, tex->texnum );

	for( i = 0; i < numSides; i++ )
	{
		// track the buffer bounds
		if( buf != NULL && buf >= bufend )
			gEngfuncs.Host_Error( "GL_UploadTexture: %s image buffer overflow\n", tex->name );

		if( ImageDXT( pic->type ))
		{
			for( j = 0; j < Q_max( 1, pic->numMips ); j++ )
			{
				width = Q_max( 1, ( tex->width >> j ));
				height = Q_max( 1, ( tex->height >> j ));
				texsize = GL_CalcTextureSize( tex->format, width, height, tex->depth );
				size = GL_CalcImageSize( pic->type, width, height, tex->depth );
				GL_TextureImageDXT( tex, i, j, width, height, tex->depth, size, buf );
				tex->size += texsize;
				buf += size; // move pointer
				tex->numMips++;

				GL_CheckTexImageError( tex );
			}
		}
		else if( Q_max( 1, pic->numMips ) > 1 )	// not-compressed DDS
		{
			for( j = 0; j < Q_max( 1, pic->numMips ); j++ )
			{
				width = Q_max( 1, ( tex->width >> j ));
				height = Q_max( 1, ( tex->height >> j ));
				texsize = GL_CalcTextureSize( tex->format, width, height, tex->depth );
				size = GL_CalcImageSize( pic->type, width, height, tex->depth );
				GL_TextureImageRAW( tex, i, j, width, height, tex->depth, pic->type, buf );
				tex->size += texsize;
				buf += size; // move pointer
				tex->numMips++;

				GL_CheckTexImageError( tex );

			}
		}
		else // RGBA32
		{
			int mipCount = GL_CalcMipmapCount( tex, ( buf != NULL ));

			// NOTE: only single uncompressed textures can be resamples, no mips, no layers, no sides
			if(( tex->depth == 1 ) && ( pic->width != tex->width ) || ( pic->height != tex->height ))
				data = GL_ResampleTexture( buf, pic->width, pic->height, tex->width, tex->height, normalMap );
			else data = buf;

			if( !ImageDXT( pic->type ) && !FBitSet( tex->flags, TF_NOMIPMAP ) && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
				data = GL_ApplyFilter( data, tex->width, tex->height );

			// mips will be auto-generated if desired
			for( j = 0; j < mipCount; j++ )
			{
				width = Q_max( 1, ( tex->width >> j ));
				height = Q_max( 1, ( tex->height >> j ));
				texsize = GL_CalcTextureSize( tex->format, width, height, tex->depth );
				size = GL_CalcImageSize( pic->type, width, height, tex->depth );
				GL_TextureImageRAW( tex, i, j, width, height, tex->depth, pic->type, data );
				if( mipCount > 1 )
					GL_BuildMipMap( data, width, height, tex->depth, tex->flags );
				tex->size += texsize;
				tex->numMips++;

				GL_CheckTexImageError( tex );
			}

			// move to next side
			if( numSides > 1 && ( buf != NULL ))
				buf += GL_CalcImageSize( pic->type, pic->width, pic->height, 1 );
		}
	}

	SetBits( tex->flags, TF_IMG_UPLOADED ); // done
	tex->numMips /= numSides;

	return true;
#endif
	return true;
}

/*
===============
GL_ProcessImage

do specified actions on pixels
===============
*/
static void GL_ProcessImage( image_t *tex, rgbdata_t *pic )
{
	uint	img_flags = 0; 

	// force upload texture as RGB or RGBA (detail textures requires this)
	if( tex->flags & TF_FORCE_COLOR ) pic->flags |= IMAGE_HAS_COLOR;
	if( pic->flags & IMAGE_HAS_ALPHA ) tex->flags |= TF_HAS_ALPHA;

	if( ImageDXT( pic->type ))
	{
		if( !pic->numMips )
			tex->flags |= TF_NOMIPMAP; // disable mipmapping by user request

		// clear all the unsupported flags
		tex->flags &= ~TF_KEEP_SOURCE;
	}
	else
	{
		// copy flag about luma pixels
		if( pic->flags & IMAGE_HAS_LUMA )
			tex->flags |= TF_HAS_LUMA;

		if( pic->flags & IMAGE_QUAKEPAL )
			tex->flags |= TF_QUAKEPAL;

		// create luma texture from quake texture
		if( tex->flags & TF_MAKELUMA )
		{
			img_flags |= IMAGE_MAKE_LUMA;
			tex->flags &= ~TF_MAKELUMA;
		}

		if( tex->flags & TF_ALLOW_EMBOSS )
		{
			img_flags |= IMAGE_EMBOSS;
			tex->flags &= ~TF_ALLOW_EMBOSS;
		}

		if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
			tex->original = gEngfuncs.FS_CopyImage( pic ); // because current pic will be expanded to rgba

		// we need to expand image into RGBA buffer
		if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
		{
			#if XASH_PS2
			if( !(tex->name[0] == '#' && pic->type == PF_INDEXED_32 && pic->palette &&
				!FBitSet( tex->flags, TF_MAKELUMA|TF_ALLOW_EMBOSS )))
			#endif
			img_flags |= IMAGE_FORCE_RGBA;
		}

		// processing image before uploading (force to rgba, make luma etc)
		if( pic->buffer ) gEngfuncs.Image_Process( &pic, 0, 0, img_flags, gl_emboss_scale->value );

		if( FBitSet( tex->flags, TF_LUMINANCE ))
			ClearBits( pic->flags, IMAGE_HAS_COLOR );
	}
}

/*
================
GL_CheckTexName
================
*/
qboolean GL_CheckTexName( const char *name )
{
	if( !COM_CheckString( name ) )
		return false;

	// because multi-layered textures can exceed name string
	if( Q_strlen( name ) >= sizeof( r_images->name ))
	{
		gEngfuncs.Con_Printf( S_ERROR "LoadTexture: too long name %s (%d)\n", name, Q_strlen( name ));
		return false;
	}

	return true;
}

/*
================
GL_TextureForName
================
*/
static image_t *GL_TextureForName( const char *name )
{
	image_t	*tex;
	uint		hash;

	// find the texture in array
	hash = COM_HashKey( name, TEXTURES_HASH_SIZE );

	for( tex = r_imagesHashTable[hash]; tex != NULL; tex = tex->nextHash )
	{
		if( !Q_stricmp( tex->name, name ))
			return tex;
	}

	return NULL;
}

/*
================
GL_AllocTexture
================
*/
static image_t *GL_AllocTexture( const char *name, texFlags_t flags )
{
	image_t	*tex;
	uint		i;

	// find a free texture_t slot
	for( i = 0, tex = r_images; i < r_numImages; i++, tex++ )
		if( !tex->name[0] ) break;

	if( i == r_numImages )
	{
		if( r_numImages == MAX_TEXTURES )
			gEngfuncs.Host_Error( "GL_AllocTexture: MAX_TEXTURES limit exceeds\n" );
		r_numImages++;
	}

	tex = &r_images[i];

	// copy initial params
	Q_strncpy( tex->name, name, sizeof( tex->name ));

	//tex->texnum = i; // texnum is used for fast acess into gl_textures array too
	tex->flags = flags;
#if XASH_PS2
	// The BSP loader uniquely marks real world/WAD textures with
	// TF_ALLOW_EMBOSS. Keep that classification after GL_ProcessImage
	// consumes the flag, so PS2 can bound *only* cached world pixels.
	// Sky, HUD, menus and model textures keep their existing policies.
	if( FBitSet( flags, TF_ALLOW_EMBOSS ))
		tex->type = it_wall;
#endif

	// add to hash table
	tex->hashValue = COM_HashKey( name, TEXTURES_HASH_SIZE );
	tex->nextHash = r_imagesHashTable[tex->hashValue];
	r_imagesHashTable[tex->hashValue] = tex;

	return tex;
}

/*
================
GL_DeleteTexture
================
*/
static void GL_DeleteTexture( image_t *tex )
{
	image_t	**prev;
	image_t	*cur;
	int  i;

	ASSERT( tex != NULL );

	// Failed uploads can have a live hash entry without pixel storage.
	if( !tex->name[0]) return;

	// debug
	if( !tex->name[0] )
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_DeleteTexture: trying to free unnamed texture\n");
		return;
	}

	// remove from hash table
	prev = &r_imagesHashTable[tex->hashValue];

	while( 1 )
	{
		cur = *prev;
		if( !cur ) break;

		if( cur == tex )
		{
			*prev = cur->nextHash;
			break;
		}
		prev = &cur->nextHash;
	}

	// release source
	if( tex->original )
		gEngfuncs.FS_FreeImage( tex->original );
	#if XASH_PS2
	if( tex->ps2HudIndexed )
	{
		if( ps2_hudMaterialized == tex ) ps2_hudMaterialized = NULL;
		Mem_Free( tex->ps2HudIndexed );
		tex->ps2HudIndexed = NULL;
		tex->pixels[0] = NULL; // common renderer scratch, not a heap allocation
		tex->alpha_pixels = NULL;
	}
	#endif

	for( i = 0; i < 4; i++ )
		if( tex->pixels[i]) Mem_Free(tex->pixels[i]);
	if( tex->alpha_pixels ) Mem_Free(tex->alpha_pixels);

	memset( tex, 0, sizeof( *tex ));
}

/*
================
GL_UpdateTexSize

recalc image room
================
*/
void GAME_EXPORT GL_UpdateTexSize( int texnum, int width, int height, int depth )
{
	int		i, j, texsize;
	int		numSides;
	image_t	*tex;

	if( texnum <= 0 || texnum >= MAX_TEXTURES )
		return;

	tex = &r_images[texnum];
	numSides = FBitSet( tex->flags, TF_CUBEMAP ) ? 6 : 1;
	GL_SetTextureDimensions( tex, width, height, depth );
	tex->size = 0; // recompute now

	for( i = 0; i < numSides; i++ )
	{
		for( j = 0; j < Q_max( 1, tex->numMips ); j++ )
		{
			width = Q_max( 1, ( tex->width >> j ));
			height = Q_max( 1, ( tex->height >> j ));
			texsize = GL_CalcTextureSize( width, height, tex->depth );
			tex->size += texsize;
		}
	}
}

/*
================
GL_LoadTexture
================
*/
#if XASH_PS2
/*
 * Keep original uncompressed BGRA32 HUD overlays, but decode a row at a
 * time. FS_LoadImage normally holds both a 256 KiB file and 256 KiB RGBA
 * image while converting them to the software renderer's 16-bit format.
 * After the world and Studio models load, those temporary copies can OOM.
 * Limit this path to the verified original scope arc: unrelated image
 * types and assets continue through the ordinary ImageLib path.
 */
static int GL_LoadPS2StreamedTGA( const char *name, texFlags_t flags )
{
	file_t *file;
	image_t *tex;
	byte header[18], row[256 * 4];
	uint x, y;
	qboolean topOrigin;

	if( Q_stricmp( name, "sprites/scope_arc_nw.tga" ) ||
		!FBitSet( flags, TF_NOMIPMAP ) || FBitSet( flags, TF_NOFLIP_TGA ))
		return -1;

	file = FS_Open( name, "rb", false );
	if( !file ) return -1;
	if( FS_Read( file, header, sizeof( header )) != sizeof( header ) ||
		header[0] != 0 || header[1] != 0 || header[2] != 2 ||
		header[12] != 0 || header[13] != 1 ||
		header[14] != 0 || header[15] != 1 ||
		header[16] != 32 || (header[17] & 0x0f) != 8 ||
		(header[17] & 0x10) != 0 || FS_FileLength( file ) < 18 + 256 * 256 * 4 )
	{
		FS_Close( file );
		return -1;
	}

	tex = GL_AllocTexture( name, flags | TF_HAS_ALPHA );
	tex->type = it_pic;
	GL_SetTextureDimensions( tex, 256, 256, 1 );
	tex->numMips = 1;
	tex->pixels[0] = (pixel_t *)Mem_Calloc( r_temppool, 256 * 256 * sizeof( pixel_t ));
	tex->alpha_pixels = (pixel_t *)Mem_Calloc( r_temppool, 256 * 256 * sizeof( pixel_t ));
	tex->size = 256 * 256 * sizeof( pixel_t );
	topOrigin = (header[17] & 0x20) != 0;

	for( y = 0; y < 256; y++ )
	{
		uint dstrow = topOrigin ? y : (255 - y);
		if( FS_Read( file, row, sizeof( row )) != sizeof( row ))
		{
			FS_Close( file );
			GL_DeleteTexture( tex );
			return 0;
		}
		for( x = 0; x < 256; x++ )
		{
			const byte *bgra = &row[x * 4];
			uint r = bgra[2] * BIT(5) / 256;
			uint g = bgra[1] * BIT(6) / 256;
			uint b = bgra[0] * BIT(5) / 256;
			uint major = (((r >> 2) & MASK(3)) << 5) |
				(((g >> 3) & MASK(3)) << 2) | ((b >> 3) & MASK(2));
			uint minor = MOVE_BIT(r, 1, 5) | MOVE_BIT(r, 0, 2) |
				MOVE_BIT(g, 2, 7) | MOVE_BIT(g, 1, 4) | MOVE_BIT(g, 0, 1) |
				MOVE_BIT(b, 2, 6) | MOVE_BIT(b, 1, 3) | MOVE_BIT(b, 0, 0);
			uint idx = dstrow * 256 + x;
			uint alpha = (bgra[3] * 8 / 256) << (16 - 3);
			tex->pixels[0][idx] = (major << 8) | (minor & 0xff);
			tex->alpha_pixels[idx] = (tex->pixels[0][idx] >> 3) | alpha;
		}
	}
	FS_Close( file );
	SetBits( tex->flags, TF_IMG_UPLOADED );
	GL_ApplyTextureParams( tex );
	printf( "[XASH-PS2] STAGE=TGA_STREAMED_DIRECT name=%s size=256x256\n", name );
	return (int)(tex - r_images);
}
#endif

int GAME_EXPORT GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	image_t	*tex;
	rgbdata_t		*pic;
	uint		picFlags = 0;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )))
		return (tex - r_images);

#if XASH_PS2
	if( !buf && !size )
	{
		int streamed = GL_LoadPS2StreamedTGA( name, flags );
		if( streamed >= 0 ) return streamed;
	}
#endif

	if( FBitSet( flags, TF_NOFLIP_TGA ))
		SetBits( picFlags, IL_DONTFLIP_TGA );

	if( FBitSet( flags, TF_KEEP_SOURCE ) && !FBitSet( flags, TF_EXPAND_SOURCE ))
		SetBits( picFlags, IL_KEEP_8BIT );	
		#if XASH_PS2
		// Preserve the index+palette representation of Studio skins while
		// decoding. The direct upload converts them straight to 16-bit pixels.
		if( name[0] == '#' && !FBitSet( flags, TF_MAKELUMA|TF_ALLOW_EMBOSS ))
			SetBits( picFlags, IL_KEEP_8BIT );
		#endif

	// set some image flags
	gEngfuncs.Image_SetForceFlags( picFlags );

	pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic ) return 0; // couldn't loading image

	// allocate the new one
	tex = GL_AllocTexture( name, flags );
	GL_ProcessImage( tex, pic );

	if( !GL_UploadTexture( tex, pic ))
	{
		GL_DeleteTexture( tex ); // unlink hash and release partial allocations
		gEngfuncs.FS_FreeImage( pic ); // release source texture
		return 0;
	}

	GL_ApplyTextureParams( tex ); // update texture filter, wrap etc
	gEngfuncs.FS_FreeImage( pic ); // release source texture

	// NOTE: always return texnum as index in array or engine will stop work !!!
	return tex - r_images;
}

/*
================
GL_LoadTextureArray
================
*/
int GAME_EXPORT GL_LoadTextureArray( const char **names, int flags )
{
	return 0;
}

/*
================
GL_LoadTextureFromBuffer
================
*/
int GAME_EXPORT GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	image_t	*tex;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )) && !update )
		return (tex - r_images);

	// couldn't loading image
	if( !pic ) return 0;

	if( update )
	{
		if( tex == NULL )
			gEngfuncs.Host_Error( "GL_LoadTextureFromBuffer: couldn't find texture %s for update\n", name );
		SetBits( tex->flags, flags );
	}
	else
	{
		// allocate the new one
		tex = GL_AllocTexture( name, flags );
	}

	GL_ProcessImage( tex, pic );
	if( !GL_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( image_t ));
		return 0;
	}

	GL_ApplyTextureParams( tex ); // update texture filter, wrap etc
	return (tex - r_images);
}

/*
================
GL_CreateTexture

creates texture from buffer
================
*/
int GAME_EXPORT GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	int	datasize = 1;
	rgbdata_t	r_empty;

	if( FBitSet( flags, TF_ARB_16BIT ))
		datasize = 2;
	else if( FBitSet( flags, TF_ARB_FLOAT ))
		datasize = 4;

	memset( &r_empty, 0, sizeof( r_empty ));
	r_empty.width = width;
	r_empty.height = height;
	r_empty.type = PF_RGBA_32;
	r_empty.size = r_empty.width * r_empty.height * datasize * 4;
	r_empty.buffer = (byte *)buffer;

	// clear invalid combinations
	ClearBits( flags, TF_TEXTURE_3D );

	// if image not luminance and not alphacontrast it will have color
	if( !FBitSet( flags, TF_LUMINANCE ) && !FBitSet( flags, TF_ALPHACONTRAST ))
		SetBits( r_empty.flags, IMAGE_HAS_COLOR );

	if( FBitSet( flags, TF_HAS_ALPHA ))
		SetBits( r_empty.flags, IMAGE_HAS_ALPHA );

	if( FBitSet( flags, TF_CUBEMAP ))
	{
		return 0;
	}

	return GL_LoadTextureInternal( name, &r_empty, flags );
}

/*
================
GL_CreateTextureArray

creates texture array from buffer
================
*/
int GAME_EXPORT GL_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags )
{
	return 0;
}

/*
================
GL_FindTexture
================
*/
int GAME_EXPORT GL_FindTexture( const char *name )
{
	image_t	*tex;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )))
		return (tex - r_images);

	return 0;
}

/*
================
GL_FreeTexture
================
*/
void GAME_EXPORT GL_FreeTexture( unsigned int texnum )
{
	// number 0 it's already freed
	if( texnum <= 0 )
		return;

	GL_DeleteTexture( &r_images[texnum] );
}

/*
================
GL_ProcessTexture
================
*/
void GAME_EXPORT GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	image_t	*image;
	rgbdata_t		*pic;
	int		flags = 0;

	if( texnum <= 0 || texnum >= MAX_TEXTURES )
		return; // missed image
	image = &r_images[texnum];

	// select mode
	if( gamma != -1.0f )
	{
		flags = IMAGE_LIGHTGAMMA;
	}
	else if( topColor != -1 && bottomColor != -1 )
	{
		flags = IMAGE_REMAP;
	}
	else
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: bad operation for %s\n", image->name );
		return;
	}

	if( !image->original )
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: no input data for %s\n", image->name );
		return;
	}

	if( ImageDXT( image->original->type ))
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: can't process compressed texture %s\n", image->name );
		return;
	}

	// all the operations makes over the image copy not an original
	pic = gEngfuncs.FS_CopyImage( image->original );
	gEngfuncs.Image_Process( &pic, topColor, bottomColor, flags, 0.0f );

	GL_UploadTexture( image, pic );
	GL_ApplyTextureParams( image ); // update texture filter, wrap etc

	gEngfuncs.FS_FreeImage( pic );
}

/*
==============================================================================

INTERNAL TEXTURES

==============================================================================
*/
/*
==================
GL_FakeImage
==================
*/
static rgbdata_t *GL_FakeImage( int width, int height, int depth, int flags )
{
	static byte	data2D[1024]; // 16x16x4
	static rgbdata_t	r_image;

	// also use this for bad textures, but without alpha
	r_image.width = Q_max( 1, width );
	r_image.height = Q_max( 1, height );
	r_image.depth = Q_max( 1, depth );
	r_image.flags = flags;
	r_image.type = PF_RGBA_32;
	r_image.size = r_image.width * r_image.height * r_image.depth * 4;
	r_image.buffer = (r_image.size > sizeof( data2D )) ? NULL : data2D;
	r_image.palette = NULL;
	r_image.numMips = 1;
	r_image.encode = 0;

	if( FBitSet( r_image.flags, IMAGE_CUBEMAP ))
		r_image.size *= 6;
	memset( data2D, 0xFF, sizeof( data2D ));

	return &r_image;
}

/*
==================
R_InitDlightTexture
==================
*/
void R_InitDlightTexture( void )
{
	rgbdata_t	r_image;

	if( tr.dlightTexture != 0 )
		return; // already initialized

	memset( &r_image, 0, sizeof( r_image ));
	r_image.width = BLOCK_SIZE; 
	r_image.height = BLOCK_SIZE;
	r_image.flags = IMAGE_HAS_COLOR;
	r_image.type = PF_RGBA_32;
	r_image.size = r_image.width * r_image.height * 4;

	tr.dlightTexture = GL_LoadTextureInternal( "*dlight", &r_image, TF_NOMIPMAP|TF_CLAMP|TF_ATLAS_PAGE );
}

/*
==================
GL_CreateInternalTextures
==================
*/
static void GL_CreateInternalTextures( void )
{
	int	dx2, dy, d;
	int	x, y;
	rgbdata_t	*pic;

	// emo-texture from quake1
	pic = GL_FakeImage( 16, 16, 1, IMAGE_HAS_COLOR );

	for( y = 0; y < 16; y++ )
	{
		for( x = 0; x < 16; x++ )
		{
			if(( y < 8 ) ^ ( x < 8 ))
				((uint *)pic->buffer)[y*16+x] = 0xFFFF00FF;
			else ((uint *)pic->buffer)[y*16+x] = 0xFF000000;
		}
	}

	tr.defaultTexture = GL_LoadTextureInternal( REF_DEFAULT_TEXTURE, pic, TF_COLORMAP );

	// particle texture from quake1
	pic = GL_FakeImage( 16, 16, 1, IMAGE_HAS_COLOR|IMAGE_HAS_ALPHA );

	for( x = 0; x < 16; x++ )
	{
		dx2 = x - 8;
		dx2 = dx2 * dx2;

		for( y = 0; y < 16; y++ )
		{
			dy = y - 8;
			d = 255 - 35 * sqrt( dx2 + dy * dy );
			pic->buffer[( y * 16 + x ) * 4 + 3] = bound( 0, d, 255 );
		}
	}

	tr.particleTexture = GL_LoadTextureInternal( "*particle", pic, TF_CLAMP );

	// white texture
	pic = GL_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFFFFFFFF;
	tr.whiteTexture = GL_LoadTextureInternal( REF_WHITE_TEXTURE, pic, TF_COLORMAP );

	// gray texture
	pic = GL_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFF7F7F7F;
	tr.grayTexture = GL_LoadTextureInternal( REF_GRAY_TEXTURE, pic, TF_COLORMAP );

	// black texture
	pic = GL_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFF000000;
	tr.blackTexture = GL_LoadTextureInternal( REF_BLACK_TEXTURE, pic, TF_COLORMAP );

	// cinematic dummy
	pic = GL_FakeImage( 640, 100, 1, IMAGE_HAS_COLOR );
	tr.cinTexture = GL_LoadTextureInternal( "*cintexture", pic, TF_NOMIPMAP|TF_CLAMP );
}

/*
===============
R_TextureList_f
===============
*/
void R_TextureList_f( void )
{
	image_t	*image;
	int		i, texCount, bytes = 0;

	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( " -id-   -w-  -h-     -size- -fmt- -type- -data-  -encode- -wrap- -depth- -name--------\n" );

	for( i = texCount = 0, image = r_images; i < r_numImages; i++, image++ )
	{
		int	size;

		if( !image->name[0] ) continue;

		// the alpha layer is a separate allocation that tex->size does not count
		size = image->size;
		if( image->alpha_pixels )
			size += image->width * image->height * sizeof( pixel_t );

		bytes += size;
		texCount++;

		gEngfuncs.Con_Printf( "%4i: ", i );
		gEngfuncs.Con_Printf( "%4i %4i ", image->width, image->height );
		gEngfuncs.Con_Printf( "%12d ", size );

		if( image->flags & TF_NORMALMAP )
			gEngfuncs.Con_Printf( "normal  " );
		else gEngfuncs.Con_Printf( "diffuse " );

		if( image->flags & TF_CLAMP )
			gEngfuncs.Con_Printf( "clamp  " );
		else if( image->flags & TF_BORDER )
			gEngfuncs.Con_Printf( "border " );
		else gEngfuncs.Con_Printf( "repeat " );
		gEngfuncs.Con_Printf( "   %d  ", image->depth );
		gEngfuncs.Con_Printf( "  %s\n", image->name );
	}

	gEngfuncs.Con_Printf( "---------------------------------------------------------\n" );
	gEngfuncs.Con_Printf( "%i total textures\n", texCount );
	gEngfuncs.Con_Printf( "%s total memory used\n", Q_memprint( bytes ));
	gEngfuncs.Con_Printf( "\n" );
}

/*
===============
R_InitImages
===============
*/
void R_InitImages( void )
{
	memset( r_images, 0, sizeof( r_images ));
	memset( r_imagesHashTable, 0, sizeof( r_imagesHashTable ));
	r_numImages = 0;

	// create unused 0-entry
	Q_strncpy( r_images->name, "*unused*", sizeof( r_images->name ));
	r_images->hashValue = COM_HashKey( r_images->name, TEXTURES_HASH_SIZE );
	r_images->nextHash = r_imagesHashTable[r_images->hashValue];
	r_imagesHashTable[r_images->hashValue] = r_images;
	r_numImages = 1;

	// validate cvars
	R_SetTextureParameters();
	GL_CreateInternalTextures();

	gEngfuncs.Cmd_AddCommand( "texturelist", R_TextureList_f, "display loaded textures list" );
}

/*
===============
R_ShutdownImages
===============
*/
void R_ShutdownImages( void )
{
	image_t	*tex;
	int		i;

	gEngfuncs.Cmd_RemoveCommand( "texturelist" );

	for( i = 0, tex = r_images; i < r_numImages; i++, tex++ )
		GL_DeleteTexture( tex );

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	memset( r_imagesHashTable, 0, sizeof( r_imagesHashTable ));
	memset( r_images, 0, sizeof( r_images ));
	r_numImages = 0;
}
