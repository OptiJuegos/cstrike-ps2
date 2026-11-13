/*
gl_cull.c - render culling routines
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

#include "gu_local.h"
#include "entity_types.h"

/*
=============================================================

FRUSTUM AND PVS CULLING

=============================================================
*/
/*
=================
R_CullBox

Returns true if the box is completely outside the frustum
=================
*/
qboolean R_CullBox( const vec3_t mins, const vec3_t maxs )
{
	return GL_FrustumCullBox( &RI.frustum, mins, maxs, 0 );
}

/*
=================
R_CullSphere

Returns true if the sphere is completely outside the frustum
=================
*/
qboolean R_CullSphere( const vec3_t centre, const float radius )
{
	return GL_FrustumCullSphere( &RI.frustum, centre, radius, 0 );
}

/*
=============
R_CullModel
=============
*/
int R_CullModel( cl_entity_t *e, const vec3_t absmin, const vec3_t absmax )
{
	if( e == gEngfuncs.GetViewModel() )
	{
		if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
			return 1;

		if( RP_NORMALPASS() && !ENGINE_GET_PARM( PARM_THIRDPERSON ) && CL_IsViewEntityLocalPlayer())
			return 0;

		return 1;
	}

	// local client can't view himself if camera or thirdperson is not active
	if( RP_LOCALCLIENT( e ) && !ENGINE_GET_PARM( PARM_THIRDPERSON ) && CL_IsViewEntityLocalPlayer())
		return 1;

	if( R_CullBox( absmin, absmax ))
		return 1;

#if XASH_PS2
	// Map decor has stable world-space bounds. Followers and render effects
	// need their existing paths because their visible extent can differ.
	qboolean decor = r_split_pvs->value >= 2.0f && !e->player && e->index > 0 && e->model
		&& ( e->model->type == mod_studio || e->model->type == mod_sprite )
		&& e->curstate.movetype == MOVETYPE_NONE && !e->curstate.aiment
		&& e->curstate.renderfx == kRenderFxNone
		&& ( e->curstate.rendermode == kRenderNormal
			|| e->curstate.rendermode == kRenderTransTexture
			|| e->curstate.rendermode == kRenderTransAlpha );
	if( decor && e->model->type == mod_studio )
	{
		const studiohdr_t *header = e->model->cache.data;
		// A parent must still update its attachments. During sequence blending,
		// the previous pose may also extend beyond the supplied current bounds.
		decor = header && !header->numattachments
			&& ( !e->latched.sequencetime || e->latched.sequencetime + 0.2f <= gpGlobals->time );
	}

	// The server sends the union of both players' visibility. Use this
	// camera's PVS before processing the model or sprite for that view.
	if(( e->player || decor ) && r_split_pvs->value && RI.drawWorld && RP_NORMALPASS()
		&& !RI.drawOrtho && !RI.drawCubemap
		&& RI.viewleaf && RI.viewleaf->contents != CONTENTS_SOLID
		&& WORLDMODEL && WORLDMODEL->visdata
		&& !r_novis->value && !r_nocull->value && !r_lockpvs->value )
	{
		cvar_t *split = gEngfuncs.pfnGetCvarPointer( "_split_active", 0 );
		vec3_t mins, maxs;
		float margin = 0.0f;
		int i;

		// The engine changes this value directly, without updating its string.
		if( !split || !split->value )
			return 0;

		// The merged weapon may extend beyond the player's sequence bounds.
		// Include its radius and a minimum reach for the standard CS weapons.
		if( e->player && e->curstate.weaponmodel )
		{
			model_t *weapon = gEngfuncs.pfnGetModelByIndex( e->curstate.weaponmodel );
			margin = weapon ? Q_max( 64.0f, weapon->radius ) : 64.0f;
		}
		if( decor && e->model->type == mod_sprite )
		{
			const msprite_t *sprite = e->model->cache.data;
			vec3_t half;
			const float scale = e->curstate.scale != 0.0f ? fabsf( e->curstate.scale ) : 1.0f;
			if( !sprite )
				return 0;
			VectorSubtract( absmax, absmin, half );
			VectorScale( half, 0.5f, half );
			const float radius = Q_max( VectorLength( half ), sprite->radius * scale ) + 0.01f;

			// Sprites rotate toward each camera. Use a sphere that encloses
			// their scaled bounds in every orientation for the PVS query.
			for( i = 0; i < 3; i++ )
			{
				const float center = ( absmin[i] + absmax[i] ) * 0.5f;
				mins[i] = center - radius;
				maxs[i] = center + radius;
			}
		}
		else
		{
			for( i = 0; i < 3; i++ )
			{
				mins[i] = absmin[i] - margin;
				maxs[i] = absmax[i] + margin;
			}
		}
		if( !gEngfuncs.Mod_BoxVisible( mins, maxs, RI.visbytes ))
			return 1;
	}
#endif

	return 0;
}

/*
=================
R_CullSurface

cull invisible surfaces
=================
*/
int R_CullSurface( msurface_t *surf, gl_frustum_t *frustum, uint clipflags )
{
	cl_entity_t	*e = RI.currententity;

	if( !surf || !surf->texinfo || !surf->texinfo->texture )
		return CULL_OTHER;

	if( r_nocull->value )
		return CULL_VISIBLE;

	// world surfaces can be culled by vis frame too
	if( RI.currententity == gEngfuncs.GetEntityByIndex( 0 ) && surf->visframe != tr.framecount )
		return CULL_VISFRAME;

	// only static ents can be culled by frustum
	if( !R_StaticEntity( e )) frustum = NULL;

	if( !VectorIsNull( surf->plane->normal ))
	{
		float	dist;

		// can use normal.z for world (optimisation)
		if( RI.drawOrtho )
		{
			vec3_t	orthonormal;

			if( e == gEngfuncs.GetEntityByIndex( 0 ) ) orthonormal[2] = surf->plane->normal[2];
			else Matrix4x4_VectorRotate( RI.objectMatrix, surf->plane->normal, orthonormal );
			dist = orthonormal[2];
		}
		else dist = PlaneDiff( tr.modelorg, surf->plane );

		if( glState.faceCull == GL_FRONT )
		{
			if( FBitSet( surf->flags, SURF_PLANEBACK ))
			{
				if( dist >= -BACKFACE_EPSILON )
					return CULL_BACKSIDE; // wrong side
			}
			else
			{
				if( dist <= BACKFACE_EPSILON )
					return CULL_BACKSIDE; // wrong side
			}
		}
		else if( glState.faceCull == GL_BACK )
		{
			if( FBitSet( surf->flags, SURF_PLANEBACK ))
			{
				if( dist <= BACKFACE_EPSILON )
					return CULL_BACKSIDE; // wrong side
			}
			else
			{
				if( dist >= -BACKFACE_EPSILON )
					return CULL_BACKSIDE; // wrong side
			}
		}
	}

	if( frustum && GL_FrustumCullBox( frustum, surf->info->mins, surf->info->maxs, clipflags ))
		return CULL_FRUSTUM;

	return CULL_VISIBLE;
}
