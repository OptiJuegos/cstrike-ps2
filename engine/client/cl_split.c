/*
cl_split.c - local splitscreen: player 2 on the second gamepad

Player 2 is a fake client on the local server, like a bot, but its usercmds
come from the second gamepad. The game library sees an ordinary player.

The platform layer reports the second pad through CL_SplitKeyEvent and
CL_SplitAxisEvent, with the same key numbers and axis order as the first
pad. Everything game-specific lives in splitscreen.cfg:

  split_bind <key> "<actions>"   actions separated by ';'
      +attack +attack2 +jump +duck +reload +use +speed +score
                                 held while the key is down
      +joyslow                   held: slower look (scale: joy_slow)
      a | b | c                  each press runs the next step, then wraps
      split_join, split_leave    engine commands, run on press
      anything else              client command sent as player 2, on press
  split_menubind <key> "<actions>"   same, used while a text menu is open
  split_joincmds "<commands>"    client commands sent right after joining
  split_userinfo "<key value>"   userinfo keys set for player 2 after joining
  split_name                     player 2's name
HUD, text menus and player 2's weapon are in cl_splithud.c.
*/

#include "common.h"
#include "client.h"
#include "server.h"
#include "library.h"
#include "const.h"
#include "cl_split.h"

#define SPLIT_CONFIG		"splitscreen.cfg"
#define SPLIT_MAX_BINDS		20	// one per pad key
#define SPLIT_MAX_ACTIONS	384	// fits every primary and pistol name (triangle)
#define SPLIT_RUN_SPEED		400.0f
#define SPLIT_JOIN_TIMEOUT	2.0	// seconds to wait for the first menu before split_joincmds
#define SPLIT_WALK_SCALE	0.52f	// +speed, same ratio as cl_movespeedkey
#define SPLIT_SLOW		BIT( 30 )	// +joyslow; above every IN_* bit, never sent

static CVAR_DEFINE_AUTO( split_name, "Player 2", FCVAR_ARCHIVE, "splitscreen player 2 name" );
static CVAR_DEFINE_AUTO( split_joincmds, "", 0, "client commands player 2 sends after joining, separated by ';'" );
static CVAR_DEFINE_AUTO( split_userinfo, "", 0, "userinfo \"key value\" pairs set for player 2 after joining" );
static CVAR_DEFINE_AUTO( split_yawspeed, "200", FCVAR_ARCHIVE, "player 2 turn speed, degrees per second at full stick" );
static CVAR_DEFINE_AUTO( split_pitchspeed, "140", FCVAR_ARCHIVE, "player 2 look up/down speed, degrees per second at full stick" );
static CVAR_DEFINE_AUTO( split_deadzone, "0.2", FCVAR_ARCHIVE, "player 2 stick deadzone, 0..1" );

typedef struct
{
	int		key;
	int		flags;	// IN_* bits held while down
	qboolean	down;
	int		step;	// next '|' step of a cycling binding
	char		actions[SPLIT_MAX_ACTIONS];	// pressed actions, ';' separated
} split_bind_t;

static const struct
{
	const char	*name;
	int		flag;
} split_buttons[] =
{
	{ "+attack", IN_ATTACK }, { "+attack2", IN_ATTACK2 }, { "+jump", IN_JUMP },
	{ "+duck", IN_DUCK }, { "+reload", IN_RELOAD }, { "+use", IN_USE },
	{ "+speed", IN_RUN }, { "+score", IN_SCORE }, { "+joyslow", SPLIT_SLOW },
};
#define SPLIT_NUM_BUTTONS	( sizeof( split_buttons ) / sizeof( split_buttons[0] ))

// [0] in game, [1] while a text menu is open
static split_bind_t	split_binds[2][SPLIT_MAX_BINDS];
static int		split_numbinds[2];
static int		split_held;	// IN_* bits of all keys down
static float		split_axes[4];	// left x, left y, right y, right x; -1..1
static int		split_slot = -1, split_userid;
static vec3_t		split_angles;
static void		(*split_aimassist)( int player, float *angles, float fov, float step );
static double		split_joindeadline;	// split_joincmds still to send; 0 when sent

static sv_client_t *CL_SplitClient( void )
{
	sv_client_t *cl;

	if( split_slot < 0 )
		return NULL;

	cl = &svs.clients[split_slot];
	if( sv.state != ss_active || cl->state != cs_spawned || cl->userid != split_userid
		|| !FBitSet( cl->flags, FCL_FAKECLIENT ))
	{
		split_slot = -1; // map change, kick or server shutdown
		return NULL;
	}
	return cl;
}

// next ';' separated action of *text, trimmed; false at the end
static qboolean CL_SplitNextAction( const char **text, char *out, size_t size )
{
	const char *start = *text, *end;
	size_t len;

	while( *start == ';' || *start == ' ' )
		start++;
	if( !*start )
		return false;

	end = Q_strchr( start, ';' );
	if( !end )
		end = start + Q_strlen( start );
	*text = end;

	while( end > start && end[-1] == ' ' )
		end--;
	len = Q_min( (size_t)( end - start ), size - 1 );
	memcpy( out, start, len );
	out[len] = '\0';
	return true;
}

// runs actions as player 2; split_* commands go to the engine console
static void CL_SplitRunActions( const char *actions )
{
	sv_client_t *cl = CL_SplitClient();
	char command[SPLIT_MAX_ACTIONS];

	while( CL_SplitNextAction( &actions, command, sizeof( command )))
	{
		if( !Q_strncmp( command, "split_", 6 ))
		{
			Cbuf_AddText( command );
			Cbuf_AddText( "\n" );
		}
		else if( cl )
			SV_ExecuteClientCommand( cl, command );
	}
}

void CL_SplitClientCommand( const char *command )
{
	sv_client_t *cl = CL_SplitClient();
	char buffer[SPLIT_MAX_ACTIONS];

	if( !cl )
		return;
	Q_strncpy( buffer, command, sizeof( buffer ));
	SV_ExecuteClientCommand( cl, buffer );
}

static void CL_SplitApplyUserinfo( sv_client_t *cl )
{
	char key[MAX_INFO_STRING], value[MAX_INFO_STRING];
	char *text = split_userinfo.string;

	// COM_ParseFile has no size limit: a token can't be longer than the whole string
	if( Q_strlen( text ) >= sizeof( key ))
	{
		Con_Printf( "split_userinfo: too long\n" );
		return;
	}

	while(( text = COM_ParseFile( text, key )) != NULL
		&& ( text = COM_ParseFile( text, value )) != NULL )
		Info_SetValueForKey( cl->userinfo, key, value, sizeof( cl->userinfo ));
	SV_UserinfoChanged( cl );
}

static void CL_SplitJoin_f( void )
{
	sv_client_t *cl;
	edict_t *ed;

	if( CL_SplitClient( ))
		return; // already in
	if( sv.state != ss_active || sv.background || svs.maxclients < 2 )
	{
		Con_Printf( "split_join: needs a local game with maxplayers 2 or more\n" );
		return;
	}

	ed = SV_FakeConnect( split_name.string );
	if( !ed )
	{
		Con_Printf( "split_join: the server is full\n" );
		return;
	}

	// player 2 before ClientPutInServer: it sends WeaponList and the first HUD messages
	cl = SV_ClientFromEdict( ed, true );
	split_slot = cl - svs.clients;
	split_userid = cl->userid;
	VectorClear( split_angles );
	split_aimassist = (void *)COM_GetProcAddress( svgame.hInstance, "Server_SplitAimAssist" );
	CL_SplitHudReset();
	svgame.dllFuncs.pfnClientPutInServer( ed );
	CL_SplitApplyUserinfo( cl );
	// split_joincmds wait for the game's first menu, as a bot answers it:
	// sent earlier, the game's join sequence still opens the team menu afterwards
	split_joindeadline = host.realtime + SPLIT_JOIN_TIMEOUT;
}

static void CL_SplitLeave_f( void )
{
	sv_client_t *cl = CL_SplitClient();

	if( cl )
		SV_DropClient( cl, false );
	split_slot = -1;
}

static void CL_SplitBind_f( void )
{
	int table = !Q_strcmp( Cmd_Argv( 0 ), "split_menubind" );
	split_bind_t *binds = split_binds[table], *b = NULL;
	char token[SPLIT_MAX_ACTIONS];
	const char *actions;
	int key;
	size_t i;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "%s <key> [\"actions\"]\n", Cmd_Argv( 0 ));
		return;
	}

	key = Key_StringToKeynum( Cmd_Argv( 1 ));
	if( key < 0 )
	{
		Con_Printf( "split_bind: \"%s\" isn't a valid key\n", Cmd_Argv( 1 ));
		return;
	}

	for( i = 0; i < split_numbinds[table]; i++ )
		if( binds[i].key == key )
			b = &binds[i];

	if( Cmd_Argc() == 2 )
	{
		if( b ) Con_Printf( "\"%s\" = \"%s\" (flags %x)\n", Cmd_Argv( 1 ), b->actions, b->flags );
		else Con_Printf( "\"%s\" is not bound\n", Cmd_Argv( 1 ));
		return;
	}

	if( !b )
	{
		if( split_numbinds[table] == SPLIT_MAX_BINDS )
		{
			Con_Printf( "%s: too many bindings\n", Cmd_Argv( 0 ));
			return;
		}
		b = &binds[split_numbinds[table]++];
	}

	memset( b, 0, sizeof( *b ));
	b->key = key;

	// steps: kept whole, one step runs per press (CL_SplitRunStep)
	actions = Cmd_Argv( 2 );
	if( Q_strchr( actions, '|' ))
	{
		Q_strncpy( b->actions, actions, sizeof( b->actions ));
		return;
	}

	// '+' buttons become held flags, the rest stays as pressed actions
	while( CL_SplitNextAction( &actions, token, sizeof( token )))
	{
		if( token[0] != '+' )
		{
			Q_strncat( b->actions, token, sizeof( b->actions ));
			Q_strncat( b->actions, ";", sizeof( b->actions ));
			continue;
		}
		for( i = 0; i < SPLIT_NUM_BUTTONS; i++ )
			if( !Q_stricmp( token, split_buttons[i].name ))
				break;
		if( i == SPLIT_NUM_BUTTONS )
			Con_Printf( "split_bind: unknown button \"%s\"\n", token );
		else SetBits( b->flags, split_buttons[i].flag );
	}
}

static void CL_SplitUnbindAll_f( void )
{
	split_numbinds[0] = split_numbinds[1] = 0;
	split_held = 0;
}

void CL_SplitInit( void )
{
	Cvar_RegisterVariable( &split_name );
	Cvar_RegisterVariable( &split_joincmds );
	Cvar_RegisterVariable( &split_userinfo );
	Cvar_RegisterVariable( &split_yawspeed );
	Cvar_RegisterVariable( &split_pitchspeed );
	Cvar_RegisterVariable( &split_deadzone );
	Cmd_AddCommand( "split_bind", CL_SplitBind_f, "bind a second gamepad key for splitscreen player 2" );
	Cmd_AddCommand( "split_menubind", CL_SplitBind_f, "bind a second gamepad key for player 2 while a text menu is open" );
	Cmd_AddCommand( "split_unbindall", CL_SplitUnbindAll_f, "remove all splitscreen player 2 bindings" );
	Cmd_AddCommand( "split_join", CL_SplitJoin_f, "splitscreen player 2 joins the local game" );
	Cmd_AddCommand( "split_leave", CL_SplitLeave_f, "splitscreen player 2 leaves the local game" );
	CL_SplitHudInit();
	Cbuf_AddText( "exec " SPLIT_CONFIG "\n" );
}

// runs the binding's actions, or only its next step when it cycles
static void CL_SplitRunStep( split_bind_t *b )
{
	char step[SPLIT_MAX_ACTIONS];
	const char *p = b->actions, *end;
	int i, count = 1;

	for( end = p; ( end = Q_strchr( end, '|' )); end++ )
		count++;
	if( count == 1 )
	{
		CL_SplitRunActions( p );
		return;
	}
	b->step %= count;
	for( i = 0; i < b->step; i++ )
		p = Q_strchr( p, '|' ) + 1;
	end = Q_strchr( p, '|' );
	Q_strncpy( step, p, end ? Q_min( sizeof( step ), (size_t)( end - p ) + 1 ) : sizeof( step ));
	b->step = ( b->step + 1 ) % count;
	CL_SplitRunActions( step );
}

void CL_SplitKeyEvent( int key, qboolean down )
{
	int active = CL_SplitMenuOpen() ? 1 : 0;
	int table, i;

	// both tables track the key, so a key held across a menu still releases;
	// only the active table runs actions and holds buttons
	split_held = 0;
	for( table = 0; table < 2; table++ )
	{
		for( i = 0; i < split_numbinds[table]; i++ )
		{
			split_bind_t *b = &split_binds[table][i];

			if( b->key == key )
			{
				if( table == active && down && !b->down && b->actions[0] )
					CL_SplitRunStep( b );
				b->down = down;
			}
			if( table == active && b->down )
				SetBits( split_held, b->flags );
		}
	}
}

void CL_SplitAxisEvent( int axis, short value )
{
	float v = value / 32767.0f;

	if( axis < 0 || axis >= 4 )
		return;
	if( fabs( v ) < split_deadzone.value )
		v = 0.0f;
	split_axes[axis] = bound( -1.0f, v, 1.0f );
}

void CL_SplitServerFrame( void )
{
	sv_client_t *cl = CL_SplitClient();
	float lx, ly, rx, ry, speed, frametime, look;
	int buttons;
	edict_t *ed;

	if( !cl )
		return;

	if( split_joindeadline && ( CL_SplitMenuOpen() || host.realtime > split_joindeadline ))
	{
		split_joindeadline = 0.0;
		CL_SplitRunActions( split_joincmds.string );
		CL_SplitMenuHide(); // answered without menuselect
	}

	ed = cl->edict;
	frametime = sv.frametime;

	// spawn, respawn and teleports set fixangle; a fake client gets no svc_setangle
	if( ed->v.fixangle )
	{
		VectorSet( split_angles, 0.0f, ed->v.angles[YAW], 0.0f );
		ed->v.fixangle = 0;
	}

	// squared response: fine aim near the center, fast turns at the edge
	lx = split_axes[0]; ly = split_axes[1];
	ry = split_axes[2]; rx = split_axes[3];
	look = FBitSet( split_held, SPLIT_SLOW ) ? Cvar_VariableValue( "joy_slow" ) : 1.0f;
	split_angles[YAW] = anglemod( split_angles[YAW] - rx * fabs( rx ) * split_yawspeed.value * look * frametime );
	split_angles[PITCH] += ry * fabs( ry ) * split_pitchspeed.value * look * frametime;
	split_angles[PITCH] = bound( -89.0f, split_angles[PITCH], 89.0f );
	if( split_aimassist && Cvar_VariableValue( "cl_autoaim" ))
	{
		split_aimassist( NUM_FOR_EDICT( ed ), split_angles, Cvar_VariableValue( "cl_autoaim_fov" ),
			Cvar_VariableValue( "cl_autoaim_speed" ) * frametime );
		split_angles[PITCH] = bound( -89.0f, split_angles[PITCH], 89.0f );
	}

	buttons = split_held & ~SPLIT_SLOW;
	speed = FBitSet( buttons, IN_RUN ) ? SPLIT_RUN_SPEED * SPLIT_WALK_SCALE : SPLIT_RUN_SPEED;
	if( ly < 0.0f ) SetBits( buttons, IN_FORWARD );
	if( ly > 0.0f ) SetBits( buttons, IN_BACK );
	if( lx < 0.0f ) SetBits( buttons, IN_MOVELEFT );
	if( lx > 0.0f ) SetBits( buttons, IN_MOVERIGHT );

	pfnRunPlayerMove( ed, split_angles, -ly * speed, lx * speed, 0.0f, buttons, 0,
		(byte)bound( 1, (int)( frametime * 1000.0f + 0.5f ), 255 ));
}

struct edict_s *CL_SplitEdict( void )
{
	sv_client_t *cl = CL_SplitClient();

	return cl ? cl->edict : NULL;
}

// observer modes of the game (OBS_* in ReGameDLL)
#define SPLIT_OBS_CHASE_LOCKED	1
#define SPLIT_OBS_CHASE_FREE	2
#define SPLIT_OBS_IN_EYE	4
#define SPLIT_CHASE_DISTANCE	112.0f

// the player whose eyes or back player 2 sees through: the spectated one while dead
// and following, else player 2 itself (roaming and map modes have no target)
struct edict_s *CL_SplitViewEdict( void )
{
	sv_client_t *cl = CL_SplitClient();
	edict_t *ed;

	if( !cl )
		return NULL;

	ed = cl->edict;
	if(( ed->v.iuser1 == SPLIT_OBS_CHASE_LOCKED || ed->v.iuser1 == SPLIT_OBS_CHASE_FREE
		|| ed->v.iuser1 == SPLIT_OBS_IN_EYE ) && ed->v.iuser2 > 0 && ed->v.iuser2 <= svs.maxclients )
	{
		edict_t *target = EDICT_NUM( ed->v.iuser2 );

		if( SV_IsValidEdict( target ))
			return target;
	}
	return ed;
}

qboolean CL_SplitView( int *entnum, vec3_t origin, vec3_t angles, float *fov )
{
	sv_client_t *cl = CL_SplitClient();
	edict_t *ed, *target;
	int mode;

	if( !cl )
		return false;

	ed = cl->edict;
	target = CL_SplitViewEdict();
	mode = ( target != ed ) ? ed->v.iuser1 : 0;

	VectorAdd( target->v.origin, target->v.view_ofs, origin );
	if( mode == SPLIT_OBS_CHASE_LOCKED || mode == SPLIT_OBS_CHASE_FREE )
	{
		// behind the target, pulled in front of walls; free chase turns with player 2's stick
		vec3_t forward, end;
		trace_t tr;

		if( mode == SPLIT_OBS_CHASE_LOCKED )
			VectorCopy( target->v.v_angle, angles );
		else VectorCopy( split_angles, angles );
		AngleVectors( angles, forward, NULL, NULL );
		VectorMA( origin, -SPLIT_CHASE_DISTANCE, forward, end );
		tr = SV_Move( origin, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, target, false );
		VectorMA( tr.endpos, 4.0f, forward, origin );
		*entnum = NUM_FOR_EDICT( ed ); // the followed player is drawn, not hidden as in first person
	}
	else
	{
		VectorAdd( target->v.v_angle, target->v.punchangle, angles );
		*entnum = NUM_FOR_EDICT( target );
	}
	*fov = ( target->v.fov > 0.0f ) ? target->v.fov : 90.0f;
	return true;
}
