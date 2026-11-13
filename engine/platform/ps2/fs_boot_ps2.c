/* Geometry Dash PS2 startup policy, adapted to Xash's two-directory layout.
 * Probe once, bounded USB retries/child scan, then freeze the asset root. */
#include "fs_paths_ps2.h"
#include "asset_locator_ps2.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <kernel.h>
#include <delaythread.h>
#include <loadfile.h>
#include <sbv_patches.h>

void PS2_LoadUsbd(void);
extern unsigned char cs_ps2_mass_start[], cs_ps2_mass_end[];

typedef struct { unsigned probes; int usb_only; } BootProbe;

static int metadata_exists(const char *root, const char *relative)
{
    char path[1024];
    unsigned char first;
    int fd, present;
    if (!PS2_FS_JoinRoot(root, relative, path, sizeof(path))) return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    present = read(fd, &first, 1) == 1;
    close(fd);
    return present;
}
static int probe_installation(const char *root, void *context)
{
    BootProbe *p = context;
    if (p->usb_only && !PS2_AssetIsUsb(root)) return 0;
    ++p->probes;
    return (metadata_exists(root, "valve/liblist.gam") ||
            metadata_exists(root, "valve/gameinfo.txt")) &&
           (metadata_exists(root, "cstrike/liblist.gam") ||
            metadata_exists(root, "cstrike/gameinfo.txt"));
}
static void load_usb(void)
{
    static int attempted;
    int id, result = -1;
    if (attempted) return;
    attempted = 1;
    SifLoadFileInit();
    /* Stock rom0:LOADFILE cannot load EE buffers until this SDK patch.
     * A loader with a newer LOADFILE may already provide the service. */
    printf("[XASH-PS2] USB load-buffer patch=%d\n", sbv_patch_enable_lmb());
    /* Same SDK modules as GD's USB boot support. Never reset the IOP:
     * existing loader devices and later pad/audio modules must survive. */
    PS2_LoadUsbd();
    id = SifExecModuleBuffer(cs_ps2_mass_start,
        (unsigned)(cs_ps2_mass_end - cs_ps2_mass_start), 0, NULL, &result);
    printf("[XASH-PS2] USB usbhdfsd id=%d result=%d\n", id, result);
}
static int scan_children(const char *launch, const char *cwd, BootProbe *probe)
{
    Ps2AssetCandidates candidates;
    unsigned i;
    PS2_AssetCandidatesInit(&candidates, launch, cwd);
    for (i = 0; i < candidates.count; ++i) {
        const char *root = candidates.paths[i];
        DIR *dir;
        struct dirent *entry;
        unsigned examined = 0;
        if (probe->usb_only && !PS2_AssetIsUsb(root)) continue;
        if (!((i == 0 && !PS2_AssetIsDisc(root)) ||
              ((PS2_AssetIsUsb(root) || PS2_AssetIsHost(root)) &&
               PS2_AssetIsDeviceRoot(root)))) continue;
        dir = opendir(root);
        if (!dir) continue;
        while (examined < 64 && (entry = readdir(dir)) != NULL) {
            char child[1024];
            const char *name = entry->d_name;
            if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) continue;
            ++examined;
            if (strpbrk(name, ":/\\")) continue;
            if (PS2_FS_JoinRoot(root, name, child, sizeof(child)) &&
                probe_installation(child, probe) && PS2_FS_SetRoot(child)) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    return 0;
}
int PS2_FS_Init(const char *preferred)
{
    const char *launch = PS2_FS_LaunchPath();
    char cwd[CS_PS2_ROOT_BYTES] = {0};
    BootProbe probe = {0, 0};
    unsigned attempt;
    int found;
    if (PS2_FS_RootResolved()) return 1;
    /* rom0:FILEIO runs dread()'s SIF DMA with interrupts on; on real consoles
     * listing a USB folder can hang there. PCSX2 never shows it. */
    printf("[XASH-PS2] FILEIO patch=%d\n", sbv_patch_fileio());
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    probe.usb_only = PS2_AssetIsUsb(preferred && *preferred ? preferred : launch);
    if (!(preferred && *preferred) && !strchr(launch, ':') && PS2_AssetIsUsb(cwd))
        probe.usb_only = 1;
    printf("[XASH-PS2] FS_LAUNCH %s cwd=%s\n", *launch ? launch : "(unknown)", cwd);
    found = PS2_FS_SelectRoot(launch, cwd, preferred, probe_installation, &probe);
    if (!found && !PS2_AssetIsDisc(launch) &&
        (!(preferred && *preferred) || PS2_AssetIsUsb(preferred))) {
        load_usb();
        for (attempt = 0; attempt < 30 && !found; ++attempt) {
            DelayThread(100000);
            found = PS2_FS_SelectRoot(launch, cwd, preferred, probe_installation, &probe);
        }
    }
    if (!found && !(preferred && *preferred) && !PS2_AssetIsDisc(launch))
        found = scan_children(launch, cwd, &probe);
    if (found)
        printf("[XASH-PS2] FS_ROOT_CACHED root=%s probes=%u\n", PS2_FS_Root(), probe.probes);
    else
        printf("[XASH-PS2] FS_ROOT_NOT_FOUND launch=%s probes=%u\n", launch, probe.probes);
    return found;
}
