/*
 * PS2 network bring-up for the engine's BSD socket path (net_ws.c).
 * Loads DEV9, NETMAN and SMAP on the IOP, starts the EE lwIP stack (libps2ip)
 * and turns DHCP on. It does not wait for the lease: lwIP finishes DHCP in
 * its own thread, and sockets work once it is bound. Without a network
 * adapter, DEV9 fails to load and the game stays offline.
 * The IOP stack (ps2ip-nm + ps2ips) would save ~250 KB of EE RAM, but in
 * PCSX2 (2026-11-12) it hung on 1400 byte datagrams and the client never
 * accepted the server's challenge, so it is not used.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <netman.h>
#include <ps2ip.h>
#include "fs_paths_ps2.h"

#if XASH_PS2_NET

extern unsigned char cs_ps2_dev9_start[], cs_ps2_dev9_end[];
extern unsigned char cs_ps2_netman_start[], cs_ps2_netman_end[];
extern unsigned char cs_ps2_smap_start[], cs_ps2_smap_end[];

static int ps2_net_ready;

static int ps2_net_module(const char *name, unsigned char *start, unsigned char *end)
{
    int result = -1;
    int id = SifExecModuleBuffer(start, (unsigned)(end - start), 0, NULL, &result);
    printf("[XASH-PS2] NET %s id=%d result=%d\n", name, id, result);
    // a module that refuses to stay resident (no adapter) returns 1
    return id >= 0 && result != 1;
}

/* libcglue has no gethostname. net_ws.c resolves the result to find the
 * address it prints and reports, so return the adapter's IP. */
int gethostname(char *name, size_t len)
{
    t_ip_info info;
    if (!ps2_net_ready || ps2ip_getconfig("sm0", &info) < 0) { strncpy(name, "127.0.0.1", len); return 0; }
    struct ip4_addr *a = (struct ip4_addr *)&info.ipaddr;
    snprintf(name, len, "%d.%d.%d.%d", ip4_addr1(a), ip4_addr2(a), ip4_addr3(a), ip4_addr4(a));
    printf("[XASH-PS2] NET ip=%s dhcp_state=%d\n", name, info.dhcp_status);
    return 0;
}

/* Returns 0 when there is no usable adapter: the caller then never opens IP
 * sockets, because libcglue has no socket backend without ps2ip. */
int PS2_NetInit(void)
{
    struct ip4_addr ip, mask, gw;
    t_ip_info info;

    /* host: on a console is ps2link over this same adapter: loading our
     * NETMAN/SMAP takes it over and every later host: read hangs.
     * PCSX2 host: is emulated, not ps2link: build with -NetOnHost there. */
#if !XASH_PS2_NET_HOST
    if (!strncmp(PS2_FS_LaunchPath(), "host", 4)) {
        printf("[XASH-PS2] NET skipped: launched from host:\n");
        return 0;
    }
#endif
    // stock rom0:LOADFILE cannot load EE buffers without this patch; idempotent
    SifLoadFileInit();
    sbv_patch_enable_lmb();
    if (!ps2_net_module("dev9", cs_ps2_dev9_start, cs_ps2_dev9_end)) return 0;
    if (!ps2_net_module("netman", cs_ps2_netman_start, cs_ps2_netman_end)) return 0;
    if (!ps2_net_module("smap", cs_ps2_smap_start, cs_ps2_smap_end)) return 0;
    if (NetManInit() != 0) { printf("[XASH-PS2] NET NetManInit failed\n"); return 0; }

    // ps2ip.h asks for a dummy address before DHCP is enabled
    IP4_ADDR(&ip, 169, 254, 0, 1);
    IP4_ADDR(&mask, 255, 255, 0, 0);
    ip4_addr_set_zero(&gw);
    ps2ipInit(&ip, &mask, &gw);
    ps2_net_ready = 1;
    // The EE kernel never time-slices, and gsKit_sync_flip busy-waits on CSR,
    // so the main thread must rank below lwIP's tcpip thread (88) or DHCP and
    // every packet wait until the engine blocks on file I/O.
    ChangeThreadPriority(GetThreadId(), 100);

    if (ps2ip_getconfig("sm0", &info) >= 0) {
        info.dhcp_enabled = 1;
        printf("[XASH-PS2] NET dhcp=%d\n", ps2ip_setconfig(&info));
    } else {
        printf("[XASH-PS2] NET no sm0 interface\n");
    }
    return 1;
}

static int ps2_net_parse(const char *text, struct ip4_addr *out)
{
    int a, b, c, d;
    if (sscanf(text, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return 0;
    IP4_ADDR(out, a, b, c, d);
    return 1;
}

/* net_apply (Join Server menu): DHCP, or the static address typed there.
 * Returns 0 when there is no adapter or an address does not parse. */
int PS2_NetConfig(int dhcp, const char *ip, const char *mask, const char *gw)
{
    t_ip_info info;
    if (!ps2_net_ready || ps2ip_getconfig("sm0", &info) < 0) return 0;
    if (!dhcp && !(ps2_net_parse(ip, (struct ip4_addr *)&info.ipaddr)
        && ps2_net_parse(mask, (struct ip4_addr *)&info.netmask)
        && ps2_net_parse(gw, (struct ip4_addr *)&info.gw))) return 0;
    info.dhcp_enabled = dhcp;
    printf("[XASH-PS2] NET apply dhcp=%d result=%d\n", dhcp, ps2ip_setconfig(&info));
    return 1;
}

#else // !XASH_PS2_NET

/* Network off (build-engine.ps1 without -Net): no IOP network modules, no
 * lwIP. The engine stays offline; local and splitscreen games still work. */
int gethostname(char *name, size_t len)
{
    strncpy(name, "127.0.0.1", len);
    return 0;
}

int PS2_NetInit(void)
{
    printf("[XASH-PS2] NET disabled in this build\n");
    return 0;
}

int PS2_NetConfig(int dhcp, const char *ip, const char *mask, const char *gw)
{
    (void)dhcp; (void)ip; (void)mask; (void)gw;
    return 0;
}

#endif // XASH_PS2_NET
