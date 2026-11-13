/*
JoinServer.cpp -- type a server address and connect; on PS2 also the
network adapter setup (DHCP or a static address)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#include "Framework.h"
#include "PicButton.h"
#include "CheckBox.h"
#include "Field.h"

class CMenuJoinServer : public CMenuFramework
{
public:
	CMenuJoinServer() : CMenuFramework( "CMenuJoinServer" ) { }

	void Hide() override
	{
		// the field never gets QM_LOSTFOCUS when the whole menu closes,
		// so the keyboard would stay on screen over the next menu
		UI_EnableTextInput( false );
		CMenuFramework::Hide();
	}

private:
	void _Init( void ) override;
	void ApplyNet( void );
	void Connect( void );
	void SearchLan( void );

	CMenuField server;
#ifdef __PS2__
	CMenuCheckBox dhcp;
	CMenuField ip, mask, gw;
#endif
};

void CMenuJoinServer::ApplyNet( void )
{
	server.WriteCvar();
#ifdef __PS2__
	dhcp.WriteCvar();
	ip.WriteCvar();
	mask.WriteCvar();
	gw.WriteCvar();
	EngFuncs::ClientCmd( FALSE, "net_apply\n" );
#endif
	EngFuncs::ClientCmd( FALSE, "trysaveconfig\n" );
}

void CMenuJoinServer::Connect( void )
{
	char cmd[128];

	if( !server.GetBuffer()[0] )
		return;

	ApplyNet();
	snprintf( cmd, sizeof( cmd ), "connect %s\n", server.GetBuffer() );
	EngFuncs::ClientCmd( FALSE, cmd );
}

void CMenuJoinServer::SearchLan( void )
{
	ApplyNet(); // the LAN broadcast goes out with the address set here
	UI_LanGame_Menu();
}

static void SetupField( CMenuField &field, const char *name, const char *status, const char *cvar, int x, int y )
{
	field.szName = name;
	field.szStatusText = status;
	field.iMaxLength = 64;
	field.LinkCvar( cvar );
	// two columns right of the banner, rows far enough apart that each label
	// (drawn 46 above) clears the field over it; the last row ends above the
	// on-screen keyboard (starts at y 435)
	field.SetRect( x, y, 300, 36 );
}

void CMenuJoinServer::_Init( void )
{
	// register before linking, otherwise WriteCvar() finds nothing
	EngFuncs::CvarRegister( "ui_server", "", FCVAR_ARCHIVE );

	// no banner: gfx/shell/head_multi is not shipped and the PC renderer
	// draws a missing picture as a white box
	AddItem( background );
	AddButton( L( "Connect" ), L( "Connect to the server" ),
		PC_JOIN_GAME, VoidCb( &CMenuJoinServer::Connect ));
	AddButton( L( "Search LAN" ), L( "List the servers on the local network" ),
		PC_LAN_GAME, VoidCb( &CMenuJoinServer::SearchLan ));
	AddButton( L( "Done" ), L( "Go back to the Main menu" ),
		PC_DONE, VoidCb( &CMenuJoinServer::Hide ));

	SetupField( server, L( "Server" ), L( "Server address, like 192.168.1.36:27015" ), "ui_server", 420, 170 );
	AddItem( server );
#ifdef __PS2__
	dhcp.SetNameAndStatus( "DHCP", L( "Get the PS2 address from the router. Off: use the address below" ) );
	dhcp.LinkCvar( "net_dhcp" );
	dhcp.SetCoord( 800, 170 );
	AddItem( dhcp );

	SetupField( ip, L( "PS2 address" ), L( "Static PS2 address (only when DHCP is off)" ), "net_ip", 420, 285 );
	SetupField( mask, L( "Netmask" ), L( "Static netmask (only when DHCP is off)" ), "net_mask", 800, 285 );
	SetupField( gw, L( "Gateway" ), L( "Static gateway, the router address (only when DHCP is off)" ), "net_gw", 420, 395 );
	AddItem( ip );
	AddItem( mask );
	AddItem( gw );
#endif
}

ADD_MENU( menu_joinserver, CMenuJoinServer, UI_JoinServer_Menu );
