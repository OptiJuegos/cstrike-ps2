/* Adapted from Geometry Dash PS2 ps2/src/asset_locator.c. */
#include "asset_locator_ps2.h"
#include "fs_paths_ps2.h"
#include <ctype.h>
#include <string.h>
static int device_prefix(const char *path)
{
    const char *colon = path ? strchr(path, ':') : NULL;
    if (!colon || colon - path < 2 || (size_t)(colon - path) > 15) return 0;
    for (const char *p = path; p < colon; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    return 1;
}
int PS2_AssetIsDisc(const char *path)
{
    return path && (!strncmp(path, "cdrom", 5) || !strncmp(path, "cdfs:", 5));
}
int PS2_AssetIsUsb(const char *path) { return path && !strncmp(path, "mass", 4); }
int PS2_AssetIsHost(const char *path) { return path && !strncmp(path, "host:", 5); }
int PS2_AssetIsDeviceRoot(const char *path)
{
    if (!device_prefix(path)) return 0;
    const char *tail = strchr(path, ':') + 1;
    return !*tail || ((*tail == '/' || *tail == '\\') && !tail[1]);
}
int PS2_AssetCandidatesAdd(Ps2AssetCandidates *out, const char *root)
{
    if (!out || !device_prefix(root)) return 0;
    size_t n = strlen(root);
    if (!n || n >= CSPS2_ASSET_PATH_BYTES) return 0;
    char normalized[CSPS2_ASSET_PATH_BYTES];
    memcpy(normalized, root, n + 1);
    for (size_t i = 0; i < n; ++i)
        if (normalized[i] == '\\') normalized[i] = '/';
    while (n > 2 && normalized[n-1] == '/' && normalized[n-2] != ':')
        normalized[--n] = 0;
    for (unsigned i = 0; i < out->count; ++i)
        if (!strcmp(out->paths[i], normalized)) return 0;
    if (out->count >= CSPS2_ASSET_MAX_ROOTS) return 0;
    strcpy(out->paths[out->count++], normalized);
    return 1;
}
static void add_launch(Ps2AssetCandidates *out, const char *argv0)
{
    if (!device_prefix(argv0)) return;
    size_t n = strlen(argv0);
    if (n >= CSPS2_ASSET_PATH_BYTES) return;
    char path[CSPS2_ASSET_PATH_BYTES];
    memcpy(path, argv0, n + 1);
    for (size_t i = 0; i < n; ++i)
        if (path[i] == '\\') path[i] = '/';
    char *colon = strchr(path, ':');
    char *slash = strrchr(colon + 1, '/');
    if (slash && slash > colon + 1) *slash = 0;
    else colon[1] = 0;
    PS2_AssetCandidatesAdd(out, path);
    out->launch_is_disc = PS2_AssetIsDisc(argv0);
}
static void add_usb_defaults(Ps2AssetCandidates *out)
{
    PS2_AssetCandidatesAdd(out, "mass:/CS-PS2");
    PS2_AssetCandidatesAdd(out, "mass0:/CS-PS2");
    PS2_AssetCandidatesAdd(out, "mass1:/CS-PS2");
}
void PS2_AssetCandidatesInit(Ps2AssetCandidates *out,
                             const char *argv0, const char *cwd)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    add_launch(out, argv0);
    /* Optical media always ship assets.pak at the ISO root, regardless of
     * where the launcher ELF lives on that disc. Avoid USB-first searches. */
    if (out->launch_is_disc) {
        PS2_AssetCandidatesAdd(out, "cdrom0:");
        PS2_AssetCandidatesAdd(out, "cdfs:");
    }
    /* A USB launch should not be hijacked by unrelated host: resources.
     * Prefer its own directory and the standard USB install folder. */
    const int launch_is_usb = PS2_AssetIsUsb(argv0);
    if (launch_is_usb) add_usb_defaults(out);
    /* Preserve PCSX2's host directory and development layouts. */
    if (device_prefix(cwd)) PS2_AssetCandidatesAdd(out, cwd);
    /* Without a usable launch path or CWD, prefer host: for PCSX2/ps2link
     * rather than wasting time probing a possibly unmounted USB device. */
    if (!out->count) PS2_AssetCandidatesAdd(out, "host:");
    /* Common USB install layouts before device-root enumeration.
     * These are optional defaults, not mandatory installation folders. */
    if (!out->launch_is_disc && !launch_is_usb) add_usb_defaults(out);
    static const char *devices[] = {
        "host:", "mass:/", "mass0:/", "mass1:/", "cdrom0:", "cdfs:"
    };
    for (unsigned i = 0; i < sizeof(devices)/sizeof(devices[0]); ++i)
        PS2_AssetCandidatesAdd(out, devices[i]);
}

int PS2_FS_SelectRoot(const char *launch, const char *cwd, const char *preferred,
                     PS2RootProbe probe, void *context)
{
    Ps2AssetCandidates candidates;
    unsigned i;
    if (PS2_FS_RootResolved()) return 1;
    if (!probe) return 0;
    if (preferred && *preferred)
        return probe(preferred, context) && PS2_FS_SetRoot(preferred);
    PS2_AssetCandidatesInit(&candidates, launch, cwd);
    for (i = 0; i < candidates.count; ++i)
        if (probe(candidates.paths[i], context) && PS2_FS_SetRoot(candidates.paths[i]))
            return 1;
    return 0;
}
