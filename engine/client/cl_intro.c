/*
cl_intro.c - startup logos shown before the main menu

cl_intro lists the images, separated by spaces. Each one fades in from
black, holds, and fades out (cl_intro_fade and cl_intro_hold, in seconds).
Any key skips the whole intro. Empty cl_intro: no intro.

Images are cut into power-of-two tiles of at most 256 texels, because
renderers that cap texture size (PS2: 256) would blur a full-screen image.
*/

#include "common.h"
#include "client.h"

#define INTRO_MAX_TILE	256
#define INTRO_MAX_TILES	64	// 2048x2048 at most

typedef struct
{
	int	texnum;
	int	x, y, w, h;	// texels in the source image
} intro_tile_t;

static convar_t	*cl_intro;
static convar_t	*cl_intro_fade;
static convar_t	*cl_intro_hold;

static struct
{
	qboolean		finished;
	const char	*next;	// rest of cl_intro->string
	double		start;	// realtime of the current image's first frame
	int		width, height;
	int		numTiles;
	intro_tile_t	tiles[INTRO_MAX_TILES];
} intro;

static int CL_IntroPiece( int remaining )
{
	int	size = INTRO_MAX_TILE;

	while( size > remaining )
		size >>= 1;
	return size;
}

static void CL_IntroFreeImage( void )
{
	int	i;

	for( i = 0; i < intro.numTiles; i++ )
		ref.dllFuncs.GL_FreeTexture( intro.tiles[i].texnum );
	intro.numTiles = 0;
}

static qboolean CL_IntroLoadImage( const char *name )
{
	rgbdata_t		*pic, tile;
	intro_tile_t	*t;
	byte		*buffer;
	int		x, y, row;

	pic = FS_LoadImage( name, NULL, 0 );
	if( !pic ) return false;

	// shortcut: only 32 bit images (every PNG and TGA loads as RGBA), convert others to upgrade
	if( pic->type != PF_RGBA_32 )
	{
		Con_Printf( S_WARN "cl_intro: %s is not RGBA\n", name );
		FS_FreeImage( pic );
		return false;
	}

	buffer = Mem_Malloc( cls.mempool, INTRO_MAX_TILE * INTRO_MAX_TILE * 4 );
	intro.width = pic->width;
	intro.height = pic->height;

	for( y = 0; y < pic->height; y += t->h )
	{
		for( x = 0; x < pic->width && intro.numTiles < INTRO_MAX_TILES; x += t->w )
		{
			t = &intro.tiles[intro.numTiles];
			t->x = x;
			t->y = y;
			t->w = CL_IntroPiece( pic->width - x );
			t->h = CL_IntroPiece( pic->height - y );

			for( row = 0; row < t->h; row++ )
				memcpy( buffer + row * t->w * 4, pic->buffer + (( y + row ) * pic->width + x ) * 4, t->w * 4 );

			tile = *pic;
			tile.width = t->w;
			tile.height = t->h;
			tile.buffer = buffer;
			tile.size = t->w * t->h * 4;
			tile.palette = NULL;
			tile.numMips = 1;
			t->texnum = ref.dllFuncs.GL_LoadTextureFromBuffer( va( "#intro%d", intro.numTiles ), &tile, TF_IMAGE, false );
			intro.numTiles++;
		}
		if( intro.numTiles == INTRO_MAX_TILES )
			break;
	}

	Mem_Free( buffer );
	FS_FreeImage( pic );
	return intro.numTiles > 0;
}

// load the next listed image; false when the list is done
static qboolean CL_IntroNextImage( void )
{
	string	name;
	int	len;

	CL_IntroFreeImage();
	while( *intro.next )
	{
		while( *intro.next == ' ' ) intro.next++;
		for( len = 0; intro.next[len] && intro.next[len] != ' '; len++ );
		if( !len ) break;

		Q_strncpy( name, intro.next, Q_min( len + 1, (int)sizeof( name )));
		intro.next += len;
		intro.start = 0.0;
		if( CL_IntroLoadImage( name ))
			return true;
		Con_Printf( S_WARN "cl_intro: can't load %s\n", name );
	}
	return false;
}

static void CL_IntroFinish( void )
{
	CL_IntroFreeImage();
	intro.finished = true;
}

/*
=============
CL_IntroDraw

draws the current logo; false once the intro is over (the menu draws then)
=============
*/
qboolean CL_IntroDraw( void )
{
	float	fade, hold, t, alpha, sx, sy;
	int	i;

	if( intro.finished )
		return false;

	if( !intro.next )
	{
		intro.next = cl_intro->string;
		if( !CL_IntroNextImage( ))
		{
			CL_IntroFinish();
			return false;
		}
	}

	// the clock starts on the first drawn frame, after the image loaded
	if( intro.start == 0.0 )
		intro.start = host.realtime;

	fade = Q_max( cl_intro_fade->value, 0.0f );
	hold = Q_max( cl_intro_hold->value, 0.0f );
	t = host.realtime - intro.start;

	if( t >= fade * 2 + hold )
	{
		if( !CL_IntroNextImage( ))
		{
			CL_IntroFinish();
			return false;
		}
		intro.start = host.realtime;
		t = 0.0f;
	}

	if( t < fade ) alpha = 1.0f - t / fade;
	else if( t > fade + hold ) alpha = ( t - fade - hold ) / fade;
	else alpha = 0.0f;

	sx = (float)refState.width / intro.width;
	sy = (float)refState.height / intro.height;

	ref.dllFuncs.FillRGBABlend( 0, 0, refState.width, refState.height, 0, 0, 0, 255 );
	ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
	for( i = 0; i < intro.numTiles; i++ )
	{
		const intro_tile_t *tile = &intro.tiles[i];
		ref.dllFuncs.R_DrawStretchPic( tile->x * sx, tile->y * sy, tile->w * sx, tile->h * sy, 0, 0, 1, 1, tile->texnum );
	}
	if( alpha > 0.0f )
		ref.dllFuncs.FillRGBABlend( 0, 0, refState.width, refState.height, 0, 0, 0, (int)( bound( 0.0f, alpha, 1.0f ) * 255 ));

	return true;
}

/*
=============
CL_IntroKeyEvent

a key press skips the intro; true when the key was used for that
=============
*/
qboolean CL_IntroKeyEvent( int key, int down )
{
	if( intro.finished || !intro.next )
		return false;
	if( down )
		CL_IntroFinish();
	return true;
}

void CL_IntroInit( void )
{
	cl_intro = Cvar_Get( "cl_intro", "", 0, "startup logo images shown before the menu, separated by spaces" );
	cl_intro_fade = Cvar_Get( "cl_intro_fade", "1", 0, "startup logo fade in and fade out time, in seconds" );
	cl_intro_hold = Cvar_Get( "cl_intro_hold", "2", 0, "startup logo time at full brightness, in seconds" );
}
