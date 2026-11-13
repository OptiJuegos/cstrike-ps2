#include "ps2_fastpak.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__PS2__)
#include <kernel.h>
#include <timer.h>
#else
#include <pthread.h>
#endif

#define CS_PAK_MAX_FILES 65536u
#define CS_PAK_MAX_NAMES (16u * 1024u * 1024u)
#define CS_PAK_READ_AHEAD (16u * 1024u)
/* A disc seeks for every refill, so its paks read further ahead. USB 1.1
 * (about 1 MB/s) pays a fixed cost per transfer too: same size. */
#define CS_PAK_READ_AHEAD_DISC (64u * 1024u)
/* Level loads read two pak files interleaved (a BSP and its WADs): with one
 * buffer each read evicted the other stream's data, and on a disc every
 * refill is a seek. Two buffers keep both streams sequential. */
#define CS_PAK_CACHE_SLOTS 2

typedef struct {
    char *name;
    uint32_t hash, offset, length;
} CSIndexEntry;

struct CSFastPak {
    int fd;
    uint32_t bytes, count, refs;
    int mcpk;
    char *path, *names;
    CSIndexEntry *index;
    /* One read-ahead buffer per pak, keyed by absolute offset and shared by
     * all its streams: dozens of WADs stay open for the whole level, and a
     * 16 KiB buffer each cost about 400 KB of EE RAM. Guarded by the lock. */
    struct {
        unsigned char *data;
        uint32_t start, used; // used: pak->clock at the last hit, for LRU
        size_t length;
    } cache[CS_PAK_CACHE_SLOTS];
    uint32_t clock;
    size_t read_ahead;
#if defined(__PS2__)
    int semaphore;
#else
    pthread_mutex_t mutex;
#endif
};
struct CSFastPakFile {
    CSFastPak *pak;
    uint32_t start, length, position;
};

static uint32_t get_le32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint32_t get_be32(const unsigned char *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static int name_compare(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        ca = (unsigned char)tolower(ca);
        cb = (unsigned char)tolower(cb);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}
static uint32_t name_hash(const char *name)
{
    uint32_t hash = 0x811c9dc5u;
    while (*name) {
        unsigned char c = (unsigned char)*name++;
        if (c == '\\') c = '/';
        c = (unsigned char)tolower(c);
        hash = (hash ^ c) * 0x01000193u;
    }
    return hash;
}
static int cmp_entries(const void *a, const void *b)
{
    const CSIndexEntry *x = (const CSIndexEntry *)a;
    const CSIndexEntry *y = (const CSIndexEntry *)b;
    return name_compare(x->name, y->name);
}
static void lock_pak(CSFastPak *pak)
{
#if defined(__PS2__)
    WaitSema(pak->semaphore);
#else
    pthread_mutex_lock(&pak->mutex);
#endif
}
static void unlock_pak(CSFastPak *pak)
{
#if defined(__PS2__)
    SignalSema(pak->semaphore);
#else
    pthread_mutex_unlock(&pak->mutex);
#endif
}
static void destroy_pak(CSFastPak *pak)
{
    uint32_t i;
    if (!pak) return;
    if (pak->index && !pak->mcpk)
        for (i = 0; i < pak->count; ++i) free(pak->index[i].name);
    free(pak->index);
    for (i = 0; i < CS_PAK_CACHE_SLOTS; ++i) free(pak->cache[i].data);
    free(pak->names);
    free(pak->path);
    if (pak->fd >= 0) close(pak->fd);
#if defined(__PS2__)
    DeleteSema(pak->semaphore);
#else
    pthread_mutex_destroy(&pak->mutex);
#endif
    free(pak);
}
static void retain_pak(CSFastPak *pak)
{
    lock_pak(pak);
    ++pak->refs;
    unlock_pak(pak);
}
static void release_pak(CSFastPak *pak)
{
    int last;
    if (!pak) return;
    lock_pak(pak);
    last = --pak->refs == 0;
    unlock_pak(pak);
    if (last) destroy_pak(pak);
}
/* The lseek + read transaction must be atomic for streams sharing one fd:
 * the caller holds the pak lock. */
static size_t read_locked(CSFastPak *pak, uint32_t offset, void *buffer, size_t n)
{
    size_t done = 0;
#if defined(__PS2__)
    /* Load-time throughput: one line per 4 MB read from any pak. */
    static uint64_t stat_bytes, stat_next = 4u << 20, stat_t0;
    static unsigned stat_reads;
    if (!stat_t0) stat_t0 = GetTimerSystemTime();
    stat_bytes += n;
    ++stat_reads;
    if (stat_bytes >= stat_next) {
        printf("[XASH-PS2] PAKIO %u KB in %u reads, %u ms since first read\n",
            (unsigned)(stat_bytes >> 10), stat_reads,
            (unsigned)((GetTimerSystemTime() - stat_t0) * 1000 / kBUSCLK));
        stat_next += 4u << 20;
    }
#endif
    if ((uint64_t)offset + n > pak->bytes) return 0;
    if (lseek(pak->fd, (off_t)offset, SEEK_SET) == (off_t)offset) {
        while (done < n) {
            /* 256 KiB pieces: a whole BSP in one read() over host: takes longer
             * than the hang watchdog waits, and each piece reports progress. */
            size_t piece = n - done < 0x40000 ? n - done : 0x40000;
            ssize_t got = read(pak->fd, (char *)buffer + done, piece);
            if (got <= 0) break;
            done += (size_t)got;
        }
    }
    return done;
}
static size_t read_absolute(CSFastPak *pak, uint32_t offset, void *buffer, size_t n)
{
    size_t done;
    lock_pak(pak);
    done = read_locked(pak, offset, buffer, n);
    unlock_pak(pak);
    return done;
}
static int valid_span(uint32_t offset, uint32_t size, uint32_t length)
{
    return (uint64_t)offset + size <= length;
}
static int index_pack(CSFastPak *pak, const unsigned char header[32])
{
    const uint32_t dir = get_le32(header + 4);
    const uint32_t length = get_le32(header + 8);
    unsigned char *raw;
    uint32_t i, count;
    if (!length || length % 64 || !valid_span(dir, length, pak->bytes)) return 0;
    count = length / 64;
    if (!count || count > CS_PAK_MAX_FILES) return 0;
    raw = (unsigned char *)malloc(length);
    pak->index = (CSIndexEntry *)calloc(count, sizeof(*pak->index));
    if (!raw || !pak->index) { free(raw); return 0; }
    pak->count = count;
    if (read_absolute(pak, dir, raw, length) != length) { free(raw); return 0; }
    for (i = 0; i < count; ++i) {
        const unsigned char *rec = raw + (size_t)i * 64;
        size_t namelen = 0;
        while (namelen < 56 && rec[namelen]) ++namelen;
        if (!namelen) { free(raw); return 0; }
        pak->index[i].name = (char *)malloc(namelen + 1);
        if (!pak->index[i].name) { free(raw); return 0; }
        memcpy(pak->index[i].name, rec, namelen);
        pak->index[i].name[namelen] = 0;
        pak->index[i].offset = get_le32(rec + 56);
        pak->index[i].length = get_le32(rec + 60);
        if (!valid_span(pak->index[i].offset, pak->index[i].length, pak->bytes)) {
            free(raw); return 0;
        }
    }
    free(raw);
    qsort(pak->index, count, sizeof(*pak->index), cmp_entries);
    return 1;
}
static int index_mcpk(CSFastPak *pak, const unsigned char header[32])
{
    uint32_t count = get_be32(header + 8), table = get_be32(header + 12);
    uint32_t name_start = get_be32(header + 16), name_bytes = get_be32(header + 20);
    unsigned char *raw;
    uint32_t i;
    if (get_be32(header + 4) != 1 || !count || count > CS_PAK_MAX_FILES ||
        !name_bytes || name_bytes > CS_PAK_MAX_NAMES || table < 32 ||
        !valid_span(table, count * 16u, name_start) ||
        !valid_span(name_start, name_bytes, pak->bytes)) return 0;
    pak->mcpk = 1;
    pak->count = count;
    pak->names = (char *)malloc((size_t)name_bytes + 1);
    pak->index = (CSIndexEntry *)calloc(count, sizeof(*pak->index));
    raw = (unsigned char *)malloc((size_t)count * 16u);
    if (!raw || !pak->index || !pak->names) { free(raw); return 0; }
    if (read_absolute(pak, name_start, pak->names, name_bytes) != name_bytes ||
        read_absolute(pak, table, raw, (size_t)count * 16u) != (size_t)count * 16u) {
        free(raw); return 0;
    }
    pak->names[name_bytes] = 0;
    for (i = 0; i < count; ++i) {
        const unsigned char *rec = raw + (size_t)i * 16u;
        uint32_t name_offset = get_be32(rec + 4);
        CSIndexEntry *e = &pak->index[i];
        if (name_offset >= name_bytes ||
            !memchr(pak->names + name_offset, 0, name_bytes - name_offset)) {
            free(raw); return 0;
        }
        e->name = pak->names + name_offset;
        e->hash = get_be32(rec);
        e->offset = get_be32(rec + 8);
        e->length = get_be32(rec + 12);
        if (!valid_span(e->offset, e->length, pak->bytes) ||
            e->hash != name_hash(e->name) ||
            (i && pak->index[i-1].hash > e->hash)) {
            free(raw); return 0;
        }
    }
    free(raw);
    return 1;
}
CSFastPak *cs_fastpak_mount(const char *filename)
{
    CSFastPak *pak;
    unsigned char header[32];
    off_t size;
    if (!filename || !filename[0]) { errno = EINVAL; return NULL; }
    pak = (CSFastPak *)calloc(1, sizeof(*pak));
    if (!pak) return NULL;
    pak->fd = -1;
#if defined(__PS2__)
    {
        ee_sema_t params = {0};
        params.init_count = 1;
        params.max_count = 1;
        pak->semaphore = CreateSema(&params);
        if (pak->semaphore < 0) { free(pak); return NULL; }
    }
#else
    if (pthread_mutex_init(&pak->mutex, NULL)) { free(pak); return NULL; }
#endif
    pak->refs = 1;
    pak->path = (char *)malloc(strlen(filename) + 1);
    if (pak->path) strcpy(pak->path, filename);
    if (!pak->path) goto bad;
    pak->read_ahead = (strncmp(filename, "cdrom", 5) && strncmp(filename, "mass", 4)) ?
        CS_PAK_READ_AHEAD : CS_PAK_READ_AHEAD_DISC;
    pak->fd = open(filename, O_RDONLY
#ifdef O_BINARY
        | O_BINARY
#endif
    );
    if (pak->fd < 0) goto bad;
    size = lseek(pak->fd, 0, SEEK_END);
    if (size < 32 || (uint64_t)size > 0x7fffffffu) goto bad;
    pak->bytes = (uint32_t)size;
    if (read_absolute(pak, 0, header, sizeof(header)) != sizeof(header)) goto bad;
    if (!memcmp(header, "PACK", 4)) {
        if (!index_pack(pak, header)) goto bad;
    } else if (!memcmp(header, "MCPK", 4)) {
        if (!index_mcpk(pak, header)) goto bad;
    } else goto bad;
    return pak;
bad:
    release_pak(pak);
    errno = EINVAL;
    return NULL;
}
void cs_fastpak_unmount(CSFastPak *pak) { release_pak(pak); }
unsigned cs_fastpak_entry_count(const CSFastPak *pak) { return pak ? pak->count : 0; }
const char *cs_fastpak_path(const CSFastPak *pak) { return pak ? pak->path : NULL; }
const char *cs_fastpak_name_at(const CSFastPak *pak, unsigned index,
                               CSFastPakEntry *entry)
{
    if (!pak || index >= pak->count) return NULL;
    if (entry) {
        entry->offset = pak->index[index].offset;
        entry->length = pak->index[index].length;
    }
    return pak->index[index].name;
}
int cs_fastpak_find_index(const CSFastPak *pak, const char *name)
{
    uint32_t lo, hi;
    const CSIndexEntry *e = NULL;
    if (!pak || !name || !*name) return -1;
    while (*name == '/') ++name;
    lo = 0; hi = pak->count;
    if (pak->mcpk) {
        const uint32_t hash = name_hash(name);
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (pak->index[mid].hash < hash) lo = mid + 1;
            else hi = mid;
        }
        for (; lo < pak->count && pak->index[lo].hash == hash; ++lo)
            if (name_compare(pak->index[lo].name, name) == 0) { e = &pak->index[lo]; break; }
    } else {
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            int diff = name_compare(pak->index[mid].name, name);
            if (diff < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo < pak->count && !name_compare(pak->index[lo].name, name))
            e = &pak->index[lo];
    }
    return e ? (int)(e - pak->index) : -1;
}
int cs_fastpak_find(const CSFastPak *pak, const char *name, CSFastPakEntry *out)
{
    int index = cs_fastpak_find_index(pak, name);
    const CSIndexEntry *e;
    if (index < 0) return 0;
    e = &pak->index[index];
    if (out) { out->offset = e->offset; out->length = e->length; }
    return 1;
}
CSFastPakFile *cs_fastpak_open(CSFastPak *pak, const char *name)
{
    int index = cs_fastpak_find_index(pak, name);
    if (index < 0) { errno = ENOENT; return NULL; }
    return cs_fastpak_open_at(pak, (unsigned)index);
}
CSFastPakFile *cs_fastpak_open_at(CSFastPak *pak, unsigned index)
{
    CSFastPakEntry e;
    CSFastPakFile *file;
    if (!cs_fastpak_name_at(pak, index, &e)) { errno = ENOENT; return NULL; }
    file = (CSFastPakFile *)calloc(1, sizeof(*file));
    if (!file) return NULL;
    file->pak = pak;
    file->start = e.offset;
    file->length = e.length;
    retain_pak(pak);
    return file;
}
size_t cs_fastpak_read(CSFastPakFile *file, void *buffer, size_t requested)
{
    size_t copied = 0;
    if (!file || (!buffer && requested)) { errno = EINVAL; return 0; }
    if (requested > file->length - file->position)
        requested = file->length - file->position;
    while (copied < requested) {
        CSFastPak *pak = file->pak;
        uint32_t at = file->start + file->position; // absolute pak offset
        size_t take;
        int i, slot = -1, oldest = 0;
        lock_pak(pak);
        if (requested - copied >= pak->read_ahead) {
            take = read_locked(pak, at, (char *)buffer + copied, requested - copied);
            unlock_pak(pak);
            file->position += (uint32_t)take;
            copied += take;
            break;
        }
        for (i = 0; i < CS_PAK_CACHE_SLOTS; ++i) {
            if (pak->cache[i].length && at >= pak->cache[i].start &&
                at - pak->cache[i].start < pak->cache[i].length) { slot = i; break; }
            if (pak->cache[i].used < pak->cache[oldest].used) oldest = i;
        }
        if (slot < 0) {
            size_t prefetch = file->length - file->position;
            slot = oldest;
            if (!pak->cache[slot].data)
                pak->cache[slot].data = (unsigned char *)malloc(pak->read_ahead);
            if (!pak->cache[slot].data) {
                take = read_locked(pak, at, (char *)buffer + copied, requested - copied);
                unlock_pak(pak);
                file->position += (uint32_t)take;
                copied += take;
                break;
            }
            if (prefetch > pak->read_ahead) prefetch = pak->read_ahead;
            pak->cache[slot].start = at;
            pak->cache[slot].length = read_locked(pak, at, pak->cache[slot].data, prefetch);
            if (!pak->cache[slot].length) { unlock_pak(pak); break; }
        }
        pak->cache[slot].used = ++pak->clock;
        take = pak->cache[slot].length - (size_t)(at - pak->cache[slot].start);
        if (take > requested - copied) take = requested - copied;
        memcpy((char *)buffer + copied, pak->cache[slot].data + (size_t)(at - pak->cache[slot].start), take);
        unlock_pak(pak);
        file->position += (uint32_t)take;
        copied += take;
    }
    return copied;
}
int cs_fastpak_seek(CSFastPakFile *file, int64_t offset, int whence)
{
    int64_t position;
    if (!file) return -1;
    switch (whence) {
    case SEEK_SET: position = offset; break;
    case SEEK_CUR:
        if (offset < -(int64_t)file->position ||
            offset > (int64_t)file->length - file->position) return -1;
        position = (int64_t)file->position + offset; break;
    case SEEK_END:
        if (offset < -(int64_t)file->length || offset > 0) return -1;
        position = (int64_t)file->length + offset; break;
    default: return -1;
    }
    if (position < 0 || position > file->length) return -1;
    file->position = (uint32_t)position;
    return 0;
}
uint32_t cs_fastpak_tell(const CSFastPakFile *file) { return file ? file->position : 0; }
uint32_t cs_fastpak_size(const CSFastPakFile *file) { return file ? file->length : 0; }
void cs_fastpak_close(CSFastPakFile *file)
{
    if (!file) return;
    release_pak(file->pak);
    free(file);
}
