/* Phase timers for studio models, from the client's renderer and ref_gu.
 * Printed by PS2_Perf_EndFrame after the PERF line. Compiled out except on PS2. */
#ifndef PS2_STUDIO_PERF_H
#define PS2_STUDIO_PERF_H

#if defined( __PS2__ )

enum
{
	PS2_STUDIO_BONES,  // StudioSetupBones, whole: includes MATRIX
	PS2_STUDIO_MATRIX, //   QuaternionMatrix and ConcatTransforms per bone
	PS2_STUDIO_MERGE,  // StudioMergeBones: weapons on players
	PS2_STUDIO_VERTS,  // transform of the vertices by their bone
	PS2_STUDIO_LIGHT,  // light per normal
	PS2_STUDIO_MESH,   // tricmds to sceGuDrawArray: includes the color kind of GSKIND
	PS2_STUDIO_SETUP,  // R_StudioSetupLighting: light vector per bone
	PS2_STUDIO_DLIGHT, // R_StudioDynamicLight and R_StudioEntityLight
	PS2_STUDIO_TOTAL,  // whole client draw call per model: includes everything above
	PS2_STUDIO_PREP,   // player setup before the bones: model, gait, transform, bbox
	PS2_STUDIO_SAVE,   // StudioSaveBones
	PS2_STUDIO_FINAL,  // R_StudioRenderFinal: includes VERTS, LIGHT and MESH
	PS2_STUDIO_SKIN,   // R_StudioSetupSkin: texture and state per mesh, inside MESH
	PS2_STUDIO_COUNT
};

enum { PS2_STUDIO_N_DRAWS, PS2_STUDIO_N_BONES, PS2_STUDIO_N_VERTS, PS2_STUDIO_N_NORMS, PS2_STUDIO_N_STRIPS, PS2_STUDIO_N_STRIPVERTS, PS2_STUDIO_N_LIGHTCALLS, PS2_STUDIO_N_LIGHTMISSES, PS2_STUDIO_N_COUNT };

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long long ps2_studio_cycles[PS2_STUDIO_COUNT];
extern unsigned int ps2_studio_counts[PS2_STUDIO_N_COUNT];
extern unsigned int cpu_ticks( void );
#ifdef __cplusplus
}
#endif

#define PS2_STUDIO_BEGIN( id ) unsigned int ps2_studio_start_##id = cpu_ticks()
#define PS2_STUDIO_END( id ) ps2_studio_cycles[PS2_STUDIO_##id] += cpu_ticks() - ps2_studio_start_##id
#define PS2_STUDIO_ADD( id, n ) ps2_studio_counts[PS2_STUDIO_N_##id] += ( n )

#else

#define PS2_STUDIO_BEGIN( id )
#define PS2_STUDIO_END( id )
#define PS2_STUDIO_ADD( id, n )

#endif // __PS2__

#endif // PS2_STUDIO_PERF_H
