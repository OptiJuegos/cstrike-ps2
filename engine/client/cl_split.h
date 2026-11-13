/*
cl_split.h - splitscreen internals shared by cl_split.c, cl_splithud.c and cl_view.c
The public entry points are in common.h.
*/
#ifndef CL_SPLIT_H
#define CL_SPLIT_H

// cl_split.c
void CL_SplitClientCommand( const char *command );	// as player 2; no-op without player 2

// cl_game.c
const cl_enginefunc_t *CL_EngineFuncs( void );

// cl_splithud.c
extern convar_t _split_active;
void CL_SplitHudInit( void );
void CL_SplitHudReset( void );
qboolean CL_SplitMenuOpen( void );
void CL_SplitMenuHide( void );	// hide without telling the server
void CL_SplitDrawHud( int x, int y, int width, int height );
qboolean CL_SplitViewModel( cl_entity_t *view, const vec3_t origin, const vec3_t angles );
void CL_SplitViewModelDone( const cl_entity_t *view );

#endif
