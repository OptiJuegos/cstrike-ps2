/* Fixed region for big assets that die with the level, see arena_ps2.c. */
#ifndef ARENA_PS2_H
#define ARENA_PS2_H

#include <stddef.h>

void PS2_Arena_Init( void );
void *PS2_Arena_Alloc( size_t size ); // NULL when full: the caller uses malloc
int PS2_Arena_Contains( const void *ptr );
void PS2_Arena_Shrink( void *ptr, size_t size );
size_t PS2_Arena_LargestFree( void );
void PS2_Arena_Report( const char *when );

void PS2_Free( void *ptr ); // for anything from the arena or from malloc
size_t PS2_Heap_LargestBlock( size_t resolution );

#endif // ARENA_PS2_H
