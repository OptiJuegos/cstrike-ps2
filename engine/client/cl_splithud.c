/*
cl_splithud.c - what player 2 sees besides the world: HUD, text menus, weapon

Player 2 is a fake client, so the user messages the server sends it never
reach the client library. The engine keeps the latest values of the standard
Half-Life messages addressed to player 2 (Health and Battery come from the
edict) and draws player 2's HUD with the game's own HUD sprites, named by
the split_hud_* cvars, or as plain text when split_hud_sprites is empty or a
sprite is missing.

Old style text menus (ShowMenu, "#Name" resolved through titles.txt) are
drawn in player 2's view. While one is open, keys use the split_menubind
table instead of split_bind, with these engine commands:
  split_menuup, split_menudown    move the cursor over the selectable items
  split_menuselect                send "menuselect <n>" for the item
  split_menuclose                 send "menuselect 10" when 0 is valid, then hide
*/

#include "common.h"
#include "client.h"
#include "server.h"
#include "cl_split.h"
#include "mod_local.h"
#include "studio.h"

#define SPLIT_MAX_WEAPONS	32
#define SPLIT_MAX_AMMO		32
#define SPLIT_MENU_TEXT		1024
#define SPLIT_MENU_ITEMS	10

CVAR_DEFINE_AUTO( _split_active, "0", 0, "1 while the screen is split: the client library hides its own crosshair" );
static CVAR_DEFINE_AUTO( split_hud_health, "", 0, "splitscreen player 2 HUD: text before the health" );
static CVAR_DEFINE_AUTO( split_hud_armor, "", 0, "splitscreen player 2 HUD: text before the armor" );
static CVAR_DEFINE_AUTO( split_hud_money, "", 0, "splitscreen player 2 HUD: text before the money" );
static CVAR_DEFINE_AUTO( split_hud_ammo, "/", 0, "splitscreen player 2 HUD: text between clip and reserve ammo" );
static CVAR_DEFINE_AUTO( split_hud_sprites, "", 0, "player 2 HUD: sprite list file (hud.txt); empty for the text HUD" );
static CVAR_DEFINE_AUTO( split_hud_digits, "", 0, "player 2 HUD: sprite name of the digits without the 0..9 at the end" );
static CVAR_DEFINE_AUTO( split_hud_healthicon, "", 0, "player 2 HUD: health icon sprite name" );
static CVAR_DEFINE_AUTO( split_hud_armoricon, "", 0, "player 2 HUD: armor icon sprite name" );
static CVAR_DEFINE_AUTO( split_hud_moneyicon, "", 0, "player 2 HUD: money icon sprite name" );
static CVAR_DEFINE_AUTO( split_hud_divider, "", 0, "player 2 HUD: sprite name between clip and reserve ammo" );
static CVAR_DEFINE_AUTO( split_hud_ammoicon, "", 0, "player 2 HUD: ammo icon sprite name in each weapon's sprites/<weapon>.txt" );
static CVAR_DEFINE_AUTO( split_hud_res, "0", 0, "player 2 HUD: sprite resolution in the list (320, 640); 0 picks by screen width" );
static CVAR_DEFINE_AUTO( split_hud_color, "255 160 0", 0, "player 2 HUD: sprite color, r g b" );

typedef struct
{
	HSPRITE	hspr;	// 0: missing
	wrect_t	rc;
} split_sprite_t;

// per map: WeaponList comes once at map start, sprites load with the map
static struct
{
	int	ammotype[SPLIT_MAX_WEAPONS];	// primary ammo id per weapon id
	char	name[SPLIT_MAX_WEAPONS][32];	// weapon_*, names its sprites/<name>.txt
} weapons;

// resolved on the first draw after player 2 joins
static struct
{
	qboolean	resolved;
	qboolean	ok;	// every sprite of the fixed set exists
	split_sprite_t	digits[10], health, armor, money, divider;
	split_sprite_t	ammoicon[SPLIT_MAX_WEAPONS];
	byte	ammoicon_state[SPLIT_MAX_WEAPONS];	// 0 unknown, 1 found, 2 missing
} sprites;

static struct
{
	int	money;			// -1 until the server sends it
	int	weapon;			// active weapon id, 0 none
	int	clip;			// -1 for weapons without a clip
	int	ammo[SPLIT_MAX_AMMO];

	char	menu[SPLIT_MENU_TEXT];
	qboolean	menu_open;
	qboolean	menu_partial;	// more parts on the way
	int	menu_keys;		// bit n-1 for "menuselect n"
	double	menu_end;		// 0: no timeout
	int	menu_items[SPLIT_MENU_ITEMS];	// slot numbers in screen order
	int	menu_numitems, menu_cursor;

	int	viewmodel;		// model index
	int	weaponanim;
	int	weaponbody;
	float	weaponanim_time;
	qboolean	weaponanim_sent;	// svc_weaponanim seen: the edict copy can't restart a repeat
	cl_entity_t	viewent;	// kept between frames: muzzle flash, attachments
} hud;

void CL_SplitHudReset( void )
{
	memset( &hud, 0, sizeof( hud ));
	memset( &sprites, 0, sizeof( sprites ));
	hud.money = -1;
}

void CL_SplitMenuHide( void )
{
	hud.menu_open = hud.menu_partial = false;
}

qboolean CL_SplitMenuOpen( void )
{
	if( hud.menu_open && hud.menu_end && host.realtime > hud.menu_end )
		hud.menu_open = false;
	return hud.menu_open;
}

static void CL_SplitMenuParse( void )
{
	client_textmessage_t *title;
	const char *line;

	// "#Buy" is a titles.txt entry
	if( hud.menu[0] == '#' && ( title = CL_TextMessageGet( hud.menu + 1 )) != NULL )
		Q_strncpy( hud.menu, title->pMessage, sizeof( hud.menu ));

	hud.menu_numitems = hud.menu_cursor = 0;
	for( line = hud.menu; *line; )
	{
		const char *p = line;
		int slot;

		while( *p == '\\' && p[1] ) // color codes: \y \w \r \d \R
			p += 2;
		if( *p >= '0' && *p <= '9' && p[1] == '.' && hud.menu_numitems < SPLIT_MENU_ITEMS )
		{
			slot = ( *p == '0' ) ? 10 : *p - '0';
			if( FBitSet( hud.menu_keys, BIT( slot - 1 )))
				hud.menu_items[hud.menu_numitems++] = slot;
		}
		line = Q_strchr( line, '\n' );
		if( !line ) break;
		line++;
	}
}

void CL_SplitUserMessage( const char *name, const byte *data, int size )
{
	sizebuf_t	msg;
	// data points into sv.multicast at any byte offset; the bit reader loads
	// whole uints, which faults on a real PS2 EE when misaligned
	static uint aligned[MAX_USERMSG_LENGTH / sizeof( uint )];

	size = bound( 0, size, (int)sizeof( aligned ));
	memcpy( aligned, data, size );
	MSG_StartReading( &msg, (byte *)aligned, size, 0, -1 );

	if( !Q_strcmp( name, "svc_weaponanim" ))
	{
		// sent on every shot, also when the animation repeats
		hud.weaponanim = MSG_ReadByte( &msg );
		hud.weaponbody = MSG_ReadByte( &msg );
		hud.weaponanim_time = cl.time;
		hud.weaponanim_sent = true;
	}
	else if( !Q_strcmp( name, "Money" ))
	{
		hud.money = MSG_ReadLong( &msg );
	}
	else if( !Q_strcmp( name, "CurWeapon" ))
	{
		int state = MSG_ReadByte( &msg );
		int id = MSG_ReadChar( &msg );
		int clip = MSG_ReadChar( &msg );

		if( id <= 0 )
			hud.weapon = 0;
		else if( state && id < SPLIT_MAX_WEAPONS )
		{
			hud.weapon = id;
			hud.clip = clip;
		}
	}
	else if( !Q_strcmp( name, "AmmoX" ))
	{
		int type = MSG_ReadByte( &msg );
		int count = MSG_ReadByte( &msg );

		if( type < SPLIT_MAX_AMMO )
			hud.ammo[type] = count;
	}
	else if( !Q_strcmp( name, "WeaponList" ))
	{
		char name[32];
		int ammo1, id;

		Q_strncpy( name, MSG_ReadString( &msg ), sizeof( name ));
		ammo1 = MSG_ReadChar( &msg );
		MSG_ReadByte( &msg );	// max ammo 1
		MSG_ReadChar( &msg );	// ammo 2
		MSG_ReadByte( &msg );	// max ammo 2
		MSG_ReadChar( &msg );	// slot
		MSG_ReadChar( &msg );	// position
		id = MSG_ReadChar( &msg );
		if( id > 0 && id < SPLIT_MAX_WEAPONS )
		{
			weapons.ammotype[id] = ammo1;
			Q_strncpy( weapons.name[id], name, sizeof( weapons.name[id] ));
		}
	}
	else if( !Q_strcmp( name, "ShowMenu" ))
	{
		int keys = MSG_ReadShort( &msg );
		int time = MSG_ReadChar( &msg );
		qboolean more = MSG_ReadByte( &msg );

		if( !hud.menu_partial )
			hud.menu[0] = '\0';
		Q_strncat( hud.menu, MSG_ReadString( &msg ), sizeof( hud.menu ));
		hud.menu_partial = more;
		if( more )
			return;

		hud.menu_keys = keys;
		hud.menu_open = keys != 0;
		hud.menu_end = ( time > 0 ) ? host.realtime + time : 0.0;
		CL_SplitMenuParse();
	}
}

// true when one of the space separated words in list matches: as a prefix, or anywhere
/*
=================
CL_SplitEventBegin

The game's client events animate the weapon, flash the muzzle and eject
shells only for the local player. While one of player 2's events runs, the
client library sees player 2 as the local player and its view model as the
view model. Returns false for other events.
=================
*/
static struct
{
	int		playernum;
	float		weaponstarttime;
	int		weaponsequence;
	vec3_t		viewheight;
	cl_entity_t	viewent;
} split_ev;

qboolean CL_SplitEventBegin( int entindex )
{
	edict_t *ed = CL_SplitEdict();

	if( !ed || entindex != NUM_FOR_EDICT( ed ))
		return false;

	split_ev.playernum = cl.playernum;
	split_ev.weaponstarttime = cl.local.weaponstarttime;
	split_ev.weaponsequence = cl.local.weaponsequence;
	VectorCopy( cl.viewheight, split_ev.viewheight );
	split_ev.viewent = clgame.viewent;

	cl.playernum = entindex - 1;
	cl.local.weaponstarttime = -1.0f; // CL_WeaponAnim sets 0
	VectorCopy( ed->v.view_ofs, cl.viewheight );
	clgame.viewent = hud.viewent;
	return true;
}

void CL_SplitEventEnd( void )
{
	if( cl.local.weaponstarttime == 0.0f )
	{
		hud.weaponanim = cl.local.weaponsequence;
		hud.weaponbody = clgame.viewent.curstate.body;
		hud.weaponanim_time = cl.time;
		hud.weaponanim_sent = true; // the edict copy would undo it
	}
	hud.viewent = clgame.viewent; // keeps EF_MUZZLEFLASH for the next draw

	cl.playernum = split_ev.playernum;
	cl.local.weaponstarttime = split_ev.weaponstarttime;
	cl.local.weaponsequence = split_ev.weaponsequence;
	VectorCopy( split_ev.viewheight, cl.viewheight );
	clgame.viewent = split_ev.viewent;
}

static void CL_SplitMenuMove( int step )
{
	if( !CL_SplitMenuOpen() || !hud.menu_numitems )
		return;
	hud.menu_cursor = ( hud.menu_cursor + step + hud.menu_numitems ) % hud.menu_numitems;
}

static void CL_SplitMenuUp_f( void ) { CL_SplitMenuMove( -1 ); }
static void CL_SplitMenuDown_f( void ) { CL_SplitMenuMove( 1 ); }

static void CL_SplitMenuSelect_f( void )
{
	if( !CL_SplitMenuOpen() || !hud.menu_numitems )
		return;
	hud.menu_open = false; // the server sends the next menu, if any
	CL_SplitClientCommand( va( "menuselect %d", hud.menu_items[hud.menu_cursor] ));
}

static void CL_SplitMenuClose_f( void )
{
	if( !CL_SplitMenuOpen( ))
		return;
	hud.menu_open = false;
	if( FBitSet( hud.menu_keys, BIT( 9 )))
		CL_SplitClientCommand( "menuselect 10" );
}

void CL_SplitHudInit( void )
{
	CL_SplitHudReset();
	Cvar_RegisterVariable( &_split_active );
	Cvar_RegisterVariable( &split_hud_health );
	Cvar_RegisterVariable( &split_hud_armor );
	Cvar_RegisterVariable( &split_hud_money );
	Cvar_RegisterVariable( &split_hud_ammo );
	Cvar_RegisterVariable( &split_hud_sprites );
	Cvar_RegisterVariable( &split_hud_digits );
	Cvar_RegisterVariable( &split_hud_healthicon );
	Cvar_RegisterVariable( &split_hud_armoricon );
	Cvar_RegisterVariable( &split_hud_moneyicon );
	Cvar_RegisterVariable( &split_hud_divider );
	Cvar_RegisterVariable( &split_hud_ammoicon );
	Cvar_RegisterVariable( &split_hud_color );
	Cvar_RegisterVariable( &split_hud_res );
	Cmd_AddCommand( "split_menuup", CL_SplitMenuUp_f, "splitscreen player 2: previous menu item" );
	Cmd_AddCommand( "split_menudown", CL_SplitMenuDown_f, "splitscreen player 2: next menu item" );
	Cmd_AddCommand( "split_menuselect", CL_SplitMenuSelect_f, "splitscreen player 2: pick the menu item" );
	Cmd_AddCommand( "split_menuclose", CL_SplitMenuClose_f, "splitscreen player 2: close the menu" );
}

static void CL_SplitDrawMenu( int x, int y, int right, int bottom )
{
	static const rgba_t colors[] =
	{
		{ 255, 255, 255, 255 },	// \w
		{ 255, 210, 64, 255 },	// \y
		{ 255, 64, 64, 255 },	// \r
		{ 128, 128, 128, 255 },	// \d
	};
	char line[256];
	const char *text = hud.menu;
	int split;	// start of the right-aligned part (after \R), -1 none
	int height, item = 0;

	Con_DrawStringLen( "M", NULL, &height );

	// squeeze the lines when the menu is taller than the view
	{
		const char *p;
		int lines = 1;

		for( p = hud.menu; *p; p++ )
			if( *p == '\n' && p[1] && p[1] != '\n' )
				lines++;
		height = Q_min( height, ( bottom - y ) / lines );
	}

	while( *text )
	{
		const byte *color = colors[0];
		int len = 0, slot = -1;

		split = -1;

		// one line, color codes applied, the cursor item marked
		for( ; *text && *text != '\n'; text++ )
		{
			if( *text == '\\' && text[1] )
			{
				text++;
				if( *text == 'R' )	// rest of the line right-aligned (prices)
				{
					if( len < sizeof( line ) - 1 )
						line[len++] = '\0';
					split = len;
					continue;
				}
				switch( *text )
				{
				case 'y': color = colors[1]; break;
				case 'r': color = colors[2]; break;
				case 'd': color = colors[3]; break;
				default: color = colors[0]; break;
				}
				continue;
			}
			if( len == 0 && *text >= '0' && *text <= '9' && text[1] == '.' )
				slot = ( *text == '0' ) ? 10 : *text - '0';
			if( len < sizeof( line ) - 1 )
				line[len++] = *text;
		}
		line[len] = '\0';
		if( *text ) text++;
		if( split >= 0 && !line[0] && !line[split] )
			len = 0; // only the marker

		if( slot > 0 && item < hud.menu_numitems && hud.menu_items[item] == slot )
		{
			if( item == hud.menu_cursor )
				Con_DrawString( x - 12, y, ">", colors[1] );
			item++;
		}
		while( len > 0 && line[len - 1] == ' ' )
			line[--len] = '\0';
		if( !len )
			continue; // blank lines don't fit in half a screen
		Con_DrawString( x, y, line, color );
		if( split >= 0 && line[split] )
		{
			int width;

			Con_DrawStringLen( line + split, &width, NULL );
			Con_DrawString( right - width, y, line + split, color );
		}
		y += height;
	}
}

// one sprite of a list file (hud.txt or a weapon's .txt) at the HUD's resolution
static qboolean CL_SplitFindSprite( const char *list, const char *name, split_sprite_t *out )
{
	const cl_enginefunc_t *eng = CL_EngineFuncs();
	int res = split_hud_res.value > 0 ? (int)split_hud_res.value : (( refState.width >= 640 ) ? 640 : 320 );
	client_sprite_t *list_sprites;
	char listname[MAX_QPATH];
	int count, i;

	out->hspr = 0;
	if( !name[0] || !list[0] )
		return false;

	Q_strncpy( listname, list, sizeof( listname ));
	if(( list_sprites = eng->pfnSPR_GetList( listname, &count )) == NULL )
		return false;

	for( i = 0; i < count; i++ )
	{
		if( list_sprites[i].iRes == res && !Q_stricmp( list_sprites[i].szName, name ))
		{
			out->hspr = eng->pfnSPR_Load( va( "sprites/%s.spr", list_sprites[i].szSprite ));
			out->rc = list_sprites[i].rc;
			return out->hspr != 0;
		}
	}
	return false;
}

static qboolean CL_SplitResolveSprites( void )
{
	const char *list = split_hud_sprites.string;
	int i;

	if( sprites.resolved )
		return sprites.ok;

	sprites.resolved = true;
	sprites.ok = list[0] != '\0';
	for( i = 0; i < 10 && sprites.ok; i++ )
		sprites.ok = CL_SplitFindSprite( list, va( "%s%d", split_hud_digits.string, i ), &sprites.digits[i] );
	sprites.ok = sprites.ok
		&& CL_SplitFindSprite( list, split_hud_healthicon.string, &sprites.health )
		&& CL_SplitFindSprite( list, split_hud_armoricon.string, &sprites.armor )
		&& CL_SplitFindSprite( list, split_hud_moneyicon.string, &sprites.money )
		&& CL_SplitFindSprite( list, split_hud_divider.string, &sprites.divider );
	if( !sprites.ok && list[0] )
		Con_Printf( "split_hud_sprites: a sprite is missing in %s, using the text HUD\n", list );
	return sprites.ok;
}

static const split_sprite_t *CL_SplitAmmoIcon( int weapon )
{
	if( weapon <= 0 || weapon >= SPLIT_MAX_WEAPONS || !weapons.name[weapon][0] )
		return NULL;
	if( !sprites.ammoicon_state[weapon] )
	{
		qboolean found = CL_SplitFindSprite( va( "sprites/%s.txt", weapons.name[weapon] ),
			split_hud_ammoicon.string, &sprites.ammoicon[weapon] );
		sprites.ammoicon_state[weapon] = found ? 1 : 2;
	}
	return ( sprites.ammoicon_state[weapon] == 1 ) ? &sprites.ammoicon[weapon] : NULL;
}

#define SPRITE_W( s )	(( s )->rc.right - ( s )->rc.left )
#define SPRITE_H( s )	(( s )->rc.bottom - ( s )->rc.top )

static int CL_SplitDrawSprite( const split_sprite_t *sprite, int x, int y, const int color[3] )
{
	const cl_enginefunc_t *eng = CL_EngineFuncs();

	eng->pfnSPR_Set( sprite->hspr, color[0], color[1], color[2] );
	eng->pfnSPR_DrawAdditive( 0, x, y, &sprite->rc );
	return x + SPRITE_W( sprite );
}

static int CL_SplitNumberWidth( int value )
{
	char digits[16];
	int i, width = 0;

	Q_snprintf( digits, sizeof( digits ), "%d", Q_max( value, 0 ));
	for( i = 0; digits[i]; i++ )
		width += SPRITE_W( &sprites.digits[digits[i] - '0'] );
	return width;
}

static int CL_SplitDrawNumber( int x, int y, int value, const int color[3] )
{
	char digits[16];
	int i;

	Q_snprintf( digits, sizeof( digits ), "%d", Q_max( value, 0 ));
	for( i = 0; digits[i]; i++ )
		x = CL_SplitDrawSprite( &sprites.digits[digits[i] - '0'], x, y, color );
	return x;
}

// money: icon and digits, right edge at right
static void CL_SplitDrawMoney( int right, int y, const int color[3] )
{
	int x = right - SPRITE_W( &sprites.money ) - CL_SplitNumberWidth( hud.money );

	x = CL_SplitDrawSprite( &sprites.money, x, y, color );
	CL_SplitDrawNumber( x, y, hud.money, color );
}

// the layout of the game's HUD: health and armor bottom left, ammo bottom right, money above it
// sprite calls take HUD units (clgame.scrInfo) and SPR_AdjustSize scales them
// to the screen; layout comes in screen pixels, so convert when hud_scale != 1
static void CL_SplitToHudUnits( int *x, int *y, int *width, int *height )
{
	float sx = clgame.scrInfo.iWidth / (float)refState.width;
	float sy = clgame.scrInfo.iHeight / (float)CL_HUD_HEIGHT;

	*x *= sx; *width *= sx;
	*y *= sy; *height *= sy;
}

static void CL_SplitDrawSpriteHud( edict_t *ed, int x, int y, int width, int height )
{
	int color[3] = { 255, 160, 0 };
	int numh, base, right, cx;

	CL_SplitToHudUnits( &x, &y, &width, &height );
	numh = SPRITE_H( &sprites.digits[0] );
	base = y + height - numh - numh / 2;
	right = x + width - 16;

	sscanf( split_hud_color.string, "%d %d %d", &color[0], &color[1], &color[2] );

	cx = CL_SplitDrawSprite( &sprites.health, x + 10, base, color ) + 4;
	CL_SplitDrawNumber( cx, base, (int)ed->v.health, color );

	cx = CL_SplitDrawSprite( &sprites.armor, x + width / 4, base, color ) + 4;
	CL_SplitDrawNumber( cx, base, (int)ed->v.armorvalue, color );

	if( hud.money >= 0 )
		CL_SplitDrawMoney( right, base - numh - numh / 2, color );

	if( hud.weapon > 0 && ed->v.health > 0 )
	{
		const split_sprite_t *icon = CL_SplitAmmoIcon( hud.weapon );
		int type = weapons.ammotype[hud.weapon];
		int reserve = ( type > 0 ) ? hud.ammo[type & ( SPLIT_MAX_AMMO - 1 )] : -1;
		int w = 0;

		if( icon ) w += SPRITE_W( icon ) + 4;
		if( reserve >= 0 ) w += CL_SplitNumberWidth( reserve );
		if( hud.clip >= 0 ) w += CL_SplitNumberWidth( hud.clip ) + SPRITE_W( &sprites.divider ) + 16;

		cx = right - w;
		if( hud.clip >= 0 )
		{
			cx = CL_SplitDrawNumber( cx, base, hud.clip, color ) + 8;
			cx = CL_SplitDrawSprite( &sprites.divider, cx, base - ( SPRITE_H( &sprites.divider ) - numh ) / 2, color ) + 8;
		}
		if( reserve >= 0 )
			cx = CL_SplitDrawNumber( cx, base, reserve, color );
		if( icon )
			CL_SplitDrawSprite( icon, cx + 4, base, color );
	}
}

void CL_SplitDrawHud( int x, int y, int width, int height )
{
	static const rgba_t color = { 255, 180, 32, 230 };
	edict_t *ed = CL_SplitEdict();
	qboolean use_sprites;
	char text[64];
	int textwidth, textheight;

	if( !ed )
		return;

	use_sprites = CL_SplitResolveSprites();
	Con_DrawStringLen( "M", NULL, &textheight );

	// a menu fills the view: keep only the money, top right
	if( CL_SplitMenuOpen( ))
	{
		CL_SplitDrawMenu( x + 20, y + 4, x + Q_min( width - 20, 420 ), y + height - 4 );
		if( hud.money >= 0 && use_sprites )
		{
			int color[3] = { 255, 160, 0 };
			int hx = x, hy = y, hw = width, hh = height;

			sscanf( split_hud_color.string, "%d %d %d", &color[0], &color[1], &color[2] );
			CL_SplitToHudUnits( &hx, &hy, &hw, &hh );
			CL_SplitDrawMoney( hx + hw - 16, hy + 4, color );
		}
		else if( hud.money >= 0 )
		{
			Q_snprintf( text, sizeof( text ), "%s%d", split_hud_money.string, hud.money );
			Con_DrawStringLen( text, &textwidth, NULL );
			Con_DrawString( x + width - textwidth - 8, y + 4, text, color );
		}
		return;
	}

	if( use_sprites )
	{
		CL_SplitDrawSpriteHud( ed, x, y, width, height );
		return;
	}

	y += height - textheight - 4;

	Q_snprintf( text, sizeof( text ), "%s%d   %s%d", split_hud_health.string, Q_max( 0, (int)ed->v.health ),
		split_hud_armor.string, (int)ed->v.armorvalue );
	Con_DrawString( x + 8, y, text, color );

	if( hud.money >= 0 )
	{
		Q_snprintf( text, sizeof( text ), "%s%d", split_hud_money.string, hud.money );
		Con_DrawStringLen( text, &textwidth, NULL );
		Con_DrawString( x + ( width - textwidth ) / 2, y, text, color );
	}

	if( hud.weapon > 0 && ed->v.health > 0 )
	{
		int reserve = hud.ammo[weapons.ammotype[hud.weapon] & ( SPLIT_MAX_AMMO - 1 )];

		if( hud.clip >= 0 )
			Q_snprintf( text, sizeof( text ), "%d %s %d", hud.clip, split_hud_ammo.string, reserve );
		else if( weapons.ammotype[hud.weapon] > 0 )
			Q_snprintf( text, sizeof( text ), "%d", reserve );
		else text[0] = '\0';
		Con_DrawStringLen( text, &textwidth, NULL );
		Con_DrawString( x + width - textwidth - 8, y, text, color );
	}
}

qboolean CL_SplitViewModel( cl_entity_t *out, const vec3_t origin, const vec3_t angles )
{
	edict_t *ed = CL_SplitEdict();
	cl_entity_t *view;
	int index;

	if( !ed || ed->v.health <= 0 || ed->v.iuser1 || !ed->v.viewmodel )
		return false;

	index = SV_ModelIndex( STRING( ed->v.viewmodel ));
	if( !CL_ModelHandle( index ))
		return false;

	// svc_weaponanim restarts every shot; without it, follow the edict
	if( index != hud.viewmodel || ( !hud.weaponanim_sent && ed->v.weaponanim != hud.weaponanim ))
	{
		hud.viewmodel = index;
		hud.weaponanim = ed->v.weaponanim;
		hud.weaponanim_time = cl.time;
	}

	// hud.viewent keeps the muzzle flash an event set and the attachments of the last draw
	view = &hud.viewent;
	view->index = view->curstate.number = NUM_FOR_EDICT( ed );
	view->model = CL_ModelHandle( index );
	view->curstate.modelindex = index;
	view->curstate.sequence = hud.weaponanim;
	view->curstate.body = hud.weaponbody;
	view->curstate.animtime = hud.weaponanim_time;
	view->curstate.frame = 0.0f;
	view->curstate.framerate = 1.0f;
	view->curstate.rendermode = kRenderNormal;
	view->curstate.renderamt = 255;
	VectorCopy( origin, view->origin );
	VectorCopy( angles, view->angles );
	view->angles[PITCH] = -angles[PITCH]; // as V_CalcGunAngle in the client library: studio models negate it again
	VectorCopy( origin, view->curstate.origin );
	VectorCopy( view->angles, view->curstate.angles );
	*out = *view;
	return true;
}

// after player 2's draw: the ref cleared the muzzle flash and set the attachments
void CL_SplitViewModelDone( const cl_entity_t *view )
{
	hud.viewent = *view;
}
