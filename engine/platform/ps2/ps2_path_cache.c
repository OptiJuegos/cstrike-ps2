#include "ps2_path_cache.h"
#include <stdint.h>
#include <string.h>

static unsigned cache_slot(const char *name, int gamedironly)
{
    uint32_t hash = 0x811c9dc5u;
    while (*name) hash = (hash ^ (unsigned char)*name++) * 0x01000193u;
    return (hash ^ (unsigned)!!gamedironly) % CS_PATH_CACHE_SLOTS;
}
void cs_path_cache_clear(CSPathCache *cache)
{
    memset(cache, 0, sizeof(*cache));
}
void *cs_path_cache_get(const CSPathCache *cache, const char *name,
                        int gamedironly, int *index)
{
    const CSPathCacheEntry *e = &cache->entries[cache_slot(name, gamedironly)];
    if (!e->target || e->gamedironly != !!gamedironly || strcmp(e->name, name))
        return NULL;
    if (index) *index = e->index;
    return e->target;
}
void cs_path_cache_put(CSPathCache *cache, const char *name,
                        int gamedironly, void *target, int index)
{
    CSPathCacheEntry *e;
    size_t length = strlen(name);
    if (length >= CS_PATH_CACHE_NAME || !target) return;
    e = &cache->entries[cache_slot(name, gamedironly)];
    memcpy(e->name, name, length + 1);
    e->gamedironly = !!gamedironly;
    e->index = index;
    e->target = target;
}
