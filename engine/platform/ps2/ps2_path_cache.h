#ifndef CS_PS2_PATH_CACHE_H
#define CS_PS2_PATH_CACHE_H

/* Bounded positive cache of resolved archive locations. The FS owns targets
 * and must clear this before changing searchpaths or loose-file overrides.
 * Keys are exact: case/slash equivalence is decided by the normal resolver. */
#define CS_PATH_CACHE_SLOTS 64
#define CS_PATH_CACHE_NAME 256
typedef struct {
    void *target;
    int index, gamedironly;
    char name[CS_PATH_CACHE_NAME];
} CSPathCacheEntry;
typedef struct {
    CSPathCacheEntry entries[CS_PATH_CACHE_SLOTS];
} CSPathCache;

void cs_path_cache_clear(CSPathCache *cache);
void *cs_path_cache_get(const CSPathCache *cache, const char *name,
                        int gamedironly, int *index);
void cs_path_cache_put(CSPathCache *cache, const char *name,
                        int gamedironly, void *target, int index);
#endif
