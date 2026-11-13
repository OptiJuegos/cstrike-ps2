#ifndef CS_PS2_FASTPAK_H
#define CS_PS2_FASTPAK_H

#include <stddef.h>
#include <stdint.h>

/* A read-only, persistent-descriptor archive backend for PS2.
 * Recognises Quake/GoldSrc PACK and Geometry Dash/OptiCraft MCPK v1.
 * Does not replace Xash3D FS; the caller chooses when to use it. */
typedef struct CSFastPak CSFastPak;
typedef struct CSFastPakFile CSFastPakFile;

typedef struct {
    uint32_t offset;
    uint32_t length;
} CSFastPakEntry;

/* Open and index an archive once. Returns NULL (errno set) on failure. */
CSFastPak *cs_fastpak_mount(const char *filename);
/* Unmount may occur while virtual files are open. They retain the descriptor. */
void cs_fastpak_unmount(CSFastPak *pak);
unsigned cs_fastpak_entry_count(const CSFastPak *pak);
const char *cs_fastpak_path(const CSFastPak *pak);
int cs_fastpak_find(const CSFastPak *pak, const char *name, CSFastPakEntry *entry);
/* Stable index for FS search/open: avoids copying the directory or looking up twice. */
int cs_fastpak_find_index(const CSFastPak *pak, const char *name);
CSFastPakFile *cs_fastpak_open_at(CSFastPak *pak, unsigned index);
/* Enumerate cached index when importing the archive into Xash3D searchpaths. */
const char *cs_fastpak_name_at(const CSFastPak *pak, unsigned index,
                               CSFastPakEntry *entry);
CSFastPakFile *cs_fastpak_open(CSFastPak *pak, const char *name);
size_t cs_fastpak_read(CSFastPakFile *file, void *buffer, size_t size);
int cs_fastpak_seek(CSFastPakFile *file, int64_t offset, int whence);
uint32_t cs_fastpak_tell(const CSFastPakFile *file);
uint32_t cs_fastpak_size(const CSFastPakFile *file);
void cs_fastpak_close(CSFastPakFile *file);

#endif
