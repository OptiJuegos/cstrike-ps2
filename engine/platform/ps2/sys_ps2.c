/* Native PS2 OS layer used by Xash3D. No PSP firmware or Wine dependency. */
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sifrpc.h>
#include <kernel.h>
#include <timer.h>
#include <delaythread.h>
#include <debug.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <ee_debug.h>
#include "fs_paths_ps2.h"

/* ps2sdk libkernel errno.o puts its weak errno in a .data with 1-byte
 * alignment, so the linker can place it at an odd address (0x456726 seen).
 * The first errno write then raises an address store exception on a real
 * EE; PCSX2 ignores it. This strong, aligned definition replaces it. */
#undef errno
int errno __attribute__((aligned(4)));

static u64 ps2_start_tick;

/*
 * A call through a NULL pointer on the EE runs the kernel vector at address 0,
 * which ends in "syscall 0" and hangs with nothing logged. Catch that syscall
 * and print the caller's registers and stack through the EE SIO, the only
 * output usable in kernel mode. PCSX2 shows it in its log.
 */
#define PS2_SIO_ISR     (*(volatile u32 *)0x1000F130)
#define PS2_SIO_TXFIFO  (*(volatile u8 *)0x1000F180)

extern char _fdata[];
void ps2_crash_stub(void);
u32 ps2_crash_regs[16];

__asm__(
    ".set push\n.set noreorder\n.set noat\n"
    ".globl ps2_crash_stub\n"
    "ps2_crash_stub:\n"
    "la $k0, ps2_crash_regs\n"
    "sw $31, 0($k0)\n"  "sw $29, 4($k0)\n"
    "mfc0 $k1, $14\n"   "sw $k1, 8($k0)\n"
    "sw $4, 12($k0)\n"  "sw $5, 16($k0)\n"
    "sw $16, 20($k0)\n" "sw $17, 24($k0)\n" "sw $18, 28($k0)\n" "sw $19, 32($k0)\n"
    "sw $20, 36($k0)\n" "sw $21, 40($k0)\n" "sw $22, 44($k0)\n" "sw $23, 48($k0)\n"
    "sw $30, 52($k0)\n" "sw $25, 56($k0)\n"
    "j ps2_crash_report\n"
    "nop\n"
    ".set pop\n");

static void ps2_crash_putc(char c)
{
    while (PS2_SIO_ISR & 0xF000) { }
    PS2_SIO_TXFIFO = (u8)c;
    PS2_SIO_ISR = 7;
}

static void ps2_crash_puts(const char *s)
{
    while (*s) ps2_crash_putc(*s++);
}

static void ps2_crash_hex(const char *name, u32 value)
{
    int shift;
    ps2_crash_puts(name);
    for (shift = 28; shift >= 0; shift -= 4)
        ps2_crash_putc("0123456789abcdef"[(value >> shift) & 15]);
    ps2_crash_putc(' ');
}

void ps2_crash_report(void)
{
    static const char *const names[] = { "ra=", "sp=", "epc=", "a0=", "a1=", "s0=", "s1=",
        "s2=", "s3=", "s4=", "s5=", "s6=", "s7=", "fp=", "t9=" };
    u32 sp = 0, *p, *kernel = (u32 *)ps2_crash_regs[1];
    unsigned i, found = 0;

    ps2_crash_puts("\n[XASH-PS2] CRASH ");
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        ps2_crash_hex(names[i], ps2_crash_regs[i]);

    // the syscall runs on the kernel stack; the user's registers are saved there
    ps2_crash_puts("\n[XASH-PS2] CRASH_KERNEL ");
    for (i = 0; i < 128; i++)
    {
        if (kernel[i])
            ps2_crash_hex("", kernel[i]);
        if (!sp && kernel[i] >= 0x1F00000 && kernel[i] < 0x2000000 && !(kernel[i] & 15))
            sp = kernel[i];
    }

    ps2_crash_puts("\n[XASH-PS2] CRASH_STACK ");
    ps2_crash_hex("sp=", sp);
    if (sp)
    {
        for (p = (u32 *)sp; p < (u32 *)(sp + 0x4000) && found < 32; p++)
        {
            if (*p >= 0x100000 && *p < (u32)_fdata && (*p & 3) == 0)
            {
                ps2_crash_hex("", *p);
                found++;
            }
        }
    }
    ps2_crash_puts("\n");
    for (;;) { }
}

/*
 * Real consoles have no stdout. Every "[XASH-PS2]" line goes to the debug
 * screen until the GS is taken by gsKit, and to boot.log in the installation
 * root (buffered until the root is known). The link wraps printf and puts.
 */
static int ps2_screen_log;
volatile u32 ps2_wd_progress; // bumped by every printf and host frame; the hang watchdog reads it
static volatile int ps2_fatal_shown; // the error screen idles on purpose: the watchdog must not wipe it
static int ps2_log_started;
static unsigned ps2_log_lines;
static char ps2_log_pending[8192];
static size_t ps2_log_pending_len;

void PS2_ScreenLogStop(void) { ps2_screen_log = 0; }

int __real_open(const char *path, int flags, ...);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_close(int fd);
ssize_t __real_read(int fd, void *buffer, size_t n);

static void ps2_log_append(const char *data, size_t n)
{
    static int fd = -1;
    char path[CS_PS2_ROOT_BYTES + 16];
    /* Only host: roots reach here, and ps2client writes each line through at
     * once, so one open for the session is enough. Reopening per line cost
     * four network round trips per line. */
    if (fd < 0) {
        if (ps2_log_started || !PS2_FS_MakePath("boot.log", path, sizeof(path))) return;
        ps2_log_started = 1;
        fd = __real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return;
    }
    if (ps2_log_pending_len) {
        write(fd, ps2_log_pending, ps2_log_pending_len);
        ps2_log_pending_len = 0;
    }
    write(fd, data, n);
}

/* Release build (build-engine.ps1 -Release): no boot.log, and only errors are
 * printed. Every printed line is a SIF round trip to the IOP on a console. */
#ifndef XASH_PS2_RELEASE
#define XASH_PS2_RELEASE 0
#endif

static int ps2_log_keep(const char *text)
{
    return !XASH_PS2_RELEASE || strstr(text, "SYS_ERROR") || strstr(text, "HANG")
        || strstr(text, "EXCEPTION") || strstr(text, "Error");
}

static void ps2_boot_log(const char *text)
{
    size_t n;
    if (strncmp(text, "[XASH-PS2]", 10)) return;
    if (ps2_screen_log) scr_printf("%s", text + 11);
    if (XASH_PS2_RELEASE) return;
    /* shortcut: line cap keeps per-frame traces from stalling on USB writes;
     * raise it when debugging a late in-game hang. */
    if (++ps2_log_lines > 3000) return;
    n = strlen(text);
    if (!PS2_FS_RootResolved()) {
        if (ps2_log_pending_len + n < sizeof(ps2_log_pending)) {
            memcpy(ps2_log_pending + ps2_log_pending_len, text, n);
            ps2_log_pending_len += n;
        }
        return;
    }
    /* Test: writing boot.log line by line may itself hang usbhdfsd on a real
     * console, so USB installs log to the screen only. */
    if (strncmp(PS2_FS_Root(), "host:", 5)) return;
    ps2_log_append(text, n);
}

int __wrap_printf(const char *format, ...)
{
    char text[1024];
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ps2_wd_progress++;
    if (!ps2_log_keep(text)) return n;
    fputs(text, stdout);
    ps2_boot_log(text);
    return n;
}

int __wrap_puts(const char *text)
{
    char line[1024];
    snprintf(line, sizeof(line), "%s\n", text);
    if (!ps2_log_keep(line)) return 1;
    fputs(line, stdout);
    ps2_boot_log(line);
    return 1;
}

/* Debug: with -DPS2_IO_TRACE=1 every open/stat/opendir is logged before and after
 * the call, so the last boot.log line names the path that hangs a real console.
 * Off by default: over host: each traced call adds two network round trips. */
#ifndef PS2_IO_TRACE
#define PS2_IO_TRACE 0
#endif
/* Without the trace, the last call is still kept in memory: the watchdog's
 * HANG line prints it. No "->" in it means the hang is inside that call. */
static char ps2_io_last[160];
#define IO_TRACE(...) do { if (PS2_IO_TRACE) printf(__VA_ARGS__); \
    else snprintf(ps2_io_last, sizeof(ps2_io_last), __VA_ARGS__); } while (0)
int __real_stat(const char *path, struct stat *st);
DIR *__real_opendir(const char *path);

/* cdrom0: only answers exact ISO9660 names: "cdrom0:\VALVE\ASSETS.PAK;1".
 * Map engine paths ("cdrom0:/valve/assets.pak") the way tools/make_ps2_iso.py
 * names the disc: uppercase, '\' separators, other characters to '_'.
 * Returns path unchanged when it is not on the disc. */
static const char *ps2_disc_path(const char *path, char *out, size_t size)
{
    const char *p;
    size_t n;
    if (strncmp(path, "cdrom0:", 7)) return path;
    n = 7;
    memcpy(out, path, n);
    p = path + 7;
    if (*p != '/' && *p != '\\') out[n++] = '\\';
    for (; *p && n + 3 < size; ++p) {
        char c = *p;
        if (c == '/' || c == '\\') c = '\\';
        else if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        else if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_' && c != '.') c = '_';
        out[n++] = c;
    }
    if (*p) return path; // too long for out: let the driver fail on it
    memcpy(out + n, ";1", 3);
    return out;
}

int __wrap_open(const char *path, int flags, ...)
{
    va_list args;
    int mode, fd;
    char disc[1024];
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
    path = ps2_disc_path(path, disc, sizeof(disc));
    IO_TRACE("[XASH-PS2] IO open %s flags=%x\n", path, flags);
    fd = __real_open(path, flags, mode);
    IO_TRACE("[XASH-PS2] IO open -> %d\n", fd);
    return fd;
}

int __wrap_stat(const char *path, struct stat *st)
{
    int ret;
    IO_TRACE("[XASH-PS2] IO stat %s\n", path);
    if (!strncmp(path, "cdrom0:", 7)) {
        /* The disc driver can fail stat() for files that open fine, and
         * folders cannot be opened: answer for regular files through open(). */
        int fd = __wrap_open(path, O_RDONLY, 0);
        if (fd < 0) return -1;
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG;
        st->st_size = __real_lseek(fd, 0, SEEK_END);
        __real_close(fd);
        return 0;
    }
    ret = __real_stat(path, st);
    IO_TRACE("[XASH-PS2] IO stat -> %d\n", ret);
    return ret;
}

off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    off_t ret;
    IO_TRACE("[XASH-PS2] IO lseek fd=%d off=%ld whence=%d\n", fd, (long)offset, whence);
    ret = __real_lseek(fd, offset, whence);
    IO_TRACE("[XASH-PS2] IO lseek -> %ld\n", (long)ret);
    return ret;
}

/* One multi-MB read from the disc takes longer than the watchdog's ~12 s, and
 * the watchdog's printf then shares the FILEIO RPC with that read and both
 * deadlock. Read in chunks: each call is short and counts as progress. */
#define PS2_READ_CHUNK (256 * 1024)

ssize_t __wrap_read(int fd, void *buffer, size_t n)
{
    ssize_t ret, total = 0;
    IO_TRACE("[XASH-PS2] IO read fd=%d n=%u buf=%p\n", fd, (unsigned)n, buffer);
    while (n > 0) {
        size_t part = n < PS2_READ_CHUNK ? n : PS2_READ_CHUNK;
        ret = __real_read(fd, (char *)buffer + total, part);
        ps2_wd_progress++; // a slow load that keeps reading is not a hang
        if (ret < 0) { if (!total) total = ret; break; }
        total += ret;
        n -= (size_t)ret;
        if ((size_t)ret < part) break; // end of file
    }
    IO_TRACE("[XASH-PS2] IO read -> %ld\n", (long)total);
    return total;
}

int __wrap_close(int fd)
{
    int ret;
    IO_TRACE("[XASH-PS2] IO close fd=%d\n", fd);
    ret = __real_close(fd);
    IO_TRACE("[XASH-PS2] IO close -> %d\n", ret);
    return ret;
}

DIR *__wrap_opendir(const char *path)
{
    DIR *dir;
    IO_TRACE("[XASH-PS2] IO opendir %s\n", path);
    dir = __real_opendir(path);
    IO_TRACE("[XASH-PS2] IO opendir -> %s\n", dir ? "ok" : "fail");
    return dir;
}

/* Fatal errors stay on screen; exiting would drop back to a black screen. */
void PS2_FatalScreen(const char *text)
{
    printf("[XASH-PS2] SYS_ERROR %s\n", text);
    ps2_fatal_shown = 1;
    init_scr();
    scr_printf("\n\n  Xash3D PS2 error:\n\n  %s\n%s", text,
        XASH_PS2_RELEASE ? "" : "\n  See boot.log next to the ELF.\n");
    for (;;) DelayThread(1000000);
}

extern unsigned char cs_ps2_iomanx_start[], cs_ps2_iomanx_end[];
extern unsigned char cs_ps2_filexio_start[], cs_ps2_filexio_end[];
extern unsigned char cs_ps2_usbd_start[], cs_ps2_usbd_end[];
extern unsigned char cs_ps2_mass_start[], cs_ps2_mass_end[];
int fileXioInit(void); /* fileXio_rpc.h refuses newlib builds; only init is used */

static void ps2_exec_module(const char *name, unsigned char *start, unsigned char *end)
{
    int result = -1;
    int id = SifExecModuleBuffer(start, (unsigned)(end - start), 0, NULL, &result);
    printf("[XASH-PS2] IOP %s id=%d result=%d\n", name, id, result);
}

/* usbd is shared by USB mass storage, keyboard and mouse; load it only once. */
void PS2_LoadUsbd(void)
{
    static int loaded;
    if (loaded) return;
    loaded = 1;
    ps2_exec_module("usbd", cs_ps2_usbd_start, cs_ps2_usbd_end);
}

/*
 * On a real console, file calls through the BIOS rom0:FILEIO and whatever
 * modules the loader left behind hang at random points over USB. Launched
 * from USB, do what OPL and uLaunchELF do: reboot the IOP and load our own
 * iomanX, fileXio, usbd and usbhdfsd, then route newlib file calls through
 * fileXio. host: (PCSX2/ps2link) keeps the loader's IOP untouched.
 */
static void ps2_reset_iop_for_usb(void)
{
    const char *launch = PS2_FS_LaunchPath();
    if (strncmp(launch, "mass", 4)) return;
    printf("[XASH-PS2] IOP reset\n");
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync()) { }
    sceSifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    ps2_exec_module("iomanX", cs_ps2_iomanx_start, cs_ps2_iomanx_end);
    ps2_exec_module("fileXio", cs_ps2_filexio_start, cs_ps2_filexio_end);
    PS2_LoadUsbd();
    ps2_exec_module("usbhdfsd", cs_ps2_mass_start, cs_ps2_mass_end);
    printf("[XASH-PS2] IOP fileXioInit=%d\n", fileXioInit());
}

/*
 * PCSX2 ignores misaligned loads and cache problems; a real EE raises an
 * exception and freezes silently. Catch CPU exceptions and print where they
 * happened. Symbolize with addr2line on the same ELF.
 */
/* Fill the whole TV with one color through GS privileged registers only: no
 * DMA, so it works with interrupts off and the renderer mid-transfer. Tells a
 * silent freeze apart when no printf can reach the host.
 * red = CPU exception, magenta = exception inside the report, yellow = hang. */
static void ps2_bg_signal(u32 rgb)
{
    /* 0x12000000 through the TLB, like gsKit: the KSEG1 alias 0xB2000000
     * raises an address error (cause 5) from a user-mode thread. */
    *(volatile u64 *)0x12000000 = 0;   // PMODE: both read circuits off
    *(volatile u64 *)0x120000E0 = rgb; // BGCOLOR: R | G << 8 | B << 16
}

static u32 ps2_ex_info[5], ps2_ex_stack[24];
static unsigned ps2_ex_found, ps2_ex_count;
static u32 ps2_ex_report_stack[2048] __attribute__((aligned(16)));
// shared by every parked thread: they only sleep, the kernel keeps their context
static u32 ps2_ex_park_stack[256] __attribute__((aligned(16)));

static void ps2_ex_park(void)
{
    for (;;) SleepThread();
}

/* Runs after ERET, out of exception context: SIF RPC works again, so printf
 * reaches ps2client (host:) and boot.log. Inside the handler it cannot. */
static void ps2_ex_report(void)
{
    unsigned i;
    ps2_screen_log = 0; // already on screen
    printf("[XASH-PS2] EE EXCEPTION cause=%u epc=%08x badvaddr=%08x ra=%08x sp=%08x\n",
        (unsigned)ps2_ex_info[0], (unsigned)ps2_ex_info[1], (unsigned)ps2_ex_info[2],
        (unsigned)ps2_ex_info[3], (unsigned)ps2_ex_info[4]);
    printf("[XASH-PS2] stack:");
    for (i = 0; i < ps2_ex_found; i++) printf(" %08x", (unsigned)ps2_ex_stack[i]);
    printf("\n");
    fflush(stdout);
    init_scr(); // after the host report: a GS reset can stall on a busy renderer
    scr_printf("\n\n  EE EXCEPTION cause=%u\n  epc=%08x badvaddr=%08x\n  ra=%08x sp=%08x\n  stack:",
        (unsigned)ps2_ex_info[0], (unsigned)ps2_ex_info[1], (unsigned)ps2_ex_info[2],
        (unsigned)ps2_ex_info[3], (unsigned)ps2_ex_info[4]);
    for (i = 0; i < ps2_ex_found; i++) scr_printf("%s%08x", i % 6 ? " " : "\n  ", (unsigned)ps2_ex_stack[i]);
    for (;;) DelayThread(1000000);
}

static int ps2_ex_handler(EE_RegFrame *frame)
{
    u32 *p, sp = frame->sp[0];
    unsigned found = 0;
    if (ps2_ex_count++)
    {
        /* Another thread (audio, pad...) often faults right after the first
         * one. Park it so the first report still reaches the screen and
         * host log; magenta only if faults keep coming. */
        if (ps2_ex_count > 8) { ps2_bg_signal(0xFF00FF); for (;;) { } }
        frame->epc = (u32)ps2_ex_park;
        frame->sp[0] = (u32)&ps2_ex_park_stack[256 - 16];
        frame->sp[1] = 0;
        return 0;
    }
    ps2_bg_signal(0x0000FF);
    /* No GS work here: init_scr resets GS and DMA, and with interrupts off it
       hangs forever if the renderer has a transfer in flight. */
    ps2_ex_info[0] = (frame->cause >> 2) & 31;
    ps2_ex_info[1] = frame->epc;
    ps2_ex_info[2] = frame->badvaddr;
    ps2_ex_info[3] = frame->ra[0];
    ps2_ex_info[4] = sp;
    if (sp >= 0x100000 && sp < 0x2000000)
    {
        for (p = (u32 *)sp; p < (u32 *)(sp + 0x2000) && found < 24; p++)
        {
            if (*p >= 0x100000 && *p < (u32)_fdata && (*p & 3) == 0)
                ps2_ex_stack[found++] = *p;
        }
    }
    ps2_ex_found = found;
    /* Resume into ps2_ex_report on a clean stack; the crashed one may be bad. */
    frame->epc = (u32)ps2_ex_report;
    frame->sp[0] = (u32)&ps2_ex_report_stack[2048 - 16];
    frame->sp[1] = 0;
    return 0;
}

static void ps2_install_exception_screen(void)
{
    static const int causes[] = { 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13 };
    unsigned i;
    ee_dbg_install(1);
    for (i = 0; i < sizeof(causes) / sizeof(causes[0]); i++)
        ee_dbg_set_level1_handler(causes[i], ps2_ex_handler);
}

/*
 * Hang watchdog: a hang raises no exception. A kernel alarm samples the
 * interrupted PC about every 4 s. After three samples with no printf, a
 * high-priority thread prints them: an address in the ELF is where the EE
 * spins (symbolize.ps1); 8000xxxx means every thread is blocked in the kernel.
 * shortcut: also fires in a quiet menu (one line per ~12 s); debug builds only.
 */
static volatile u32 ps2_wd_armed, ps2_wd_last, ps2_wd_idle, ps2_wd_pc[3];
static s32 ps2_wd_thread;
static u8 ps2_wd_stack[8192] __attribute__((aligned(16)));
extern void *_gp;

static void ps2_wd_alarm(s32 id, u16 time, void *common)
{
    u32 epc;
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc)); // PC of the code the timer interrupted
    if (!ps2_wd_armed)
    {
        ps2_wd_armed = 1; // self-test: the first tick proves alarm, thread and printf work
        ps2_wd_pc[0] = epc;
        iWakeupThread(ps2_wd_thread);
    }
    else if (ps2_wd_progress != ps2_wd_last)
    {
        ps2_wd_last = ps2_wd_progress;
        ps2_wd_idle = 0;
    }
    else if (ps2_wd_idle < 3)
    {
        ps2_wd_pc[ps2_wd_idle++] = epc;
        if (ps2_wd_idle == 3) iWakeupThread(ps2_wd_thread);
    }
    iSetAlarm(0xFFFF, ps2_wd_alarm, NULL);
}

/* pc 00082000 is the kernel idle loop: every thread waits. Show who waits on
 * what (st 4 = WAIT; wt 1 = sleep, 2 = semaphore wid): func names the thread
 * (symbolize.ps1). Screen first: printf blocks if the IOP is gone. */
static void ps2_wd_dump_threads(void)
{
    ee_thread_status_t st;
    char line[96];
    int id, n;
    static char all[2048];
    size_t used = 0;
    for (id = 1, n = 0; id < 256 && n < 16; id++)
    {
        if (ReferThreadStatus(id, &st) < 0 || st.status == THS_DORMANT) continue;
        snprintf(line, sizeof(line), "  t%d func=%08x st=%x pr=%d wt=%u wid=%u wk=%u\n",
            id, (unsigned)st.func, st.status, st.current_priority,
            (unsigned)st.waitType, (unsigned)st.waitId, (unsigned)st.wakeupCount);
        scr_printf("%s", line);
        used += snprintf(all + used, sizeof(all) - used, "[XASH-PS2] HANG_THREAD%s", line + 1);
        if (used >= sizeof(all)) break;
        n++;
    }
    printf("%s", all);
}

static void ps2_wd_main(void *arg)
{
    for (;;)
    {
        SleepThread();
        if (ps2_ex_count || ps2_fatal_shown) continue; // an error screen owns the TV; its idle loop is not a hang
        if (ps2_wd_idle < 3)
            printf("[XASH-PS2] WATCHDOG_ARMED pc=%08x\n", (unsigned)ps2_wd_pc[0]);
        else
        {
            ps2_bg_signal(0x00FFFF); // before printf: it blocks if the IOP is gone
            /* USB installs have no host log: put the PCs on the TV. Stays yellow
             * if the GS reset stalls on a renderer stuck mid-transfer. */
            ps2_screen_log = 0;
            init_scr();
            scr_printf("\n\n  HANG pc=%08x %08x %08x\n  last_io=%s",
                (unsigned)ps2_wd_pc[0], (unsigned)ps2_wd_pc[1], (unsigned)ps2_wd_pc[2],
                ps2_io_last[0] ? ps2_io_last + 11 : "none\n");
            ps2_wd_dump_threads();
            printf("[XASH-PS2] HANG pc=%08x %08x %08x last_io=%s",
                (unsigned)ps2_wd_pc[0], (unsigned)ps2_wd_pc[1], (unsigned)ps2_wd_pc[2],
                ps2_io_last[0] ? ps2_io_last + 11 : "none\n");
        }
    }
}

static void ps2_install_hang_watchdog(void)
{
    ee_thread_t t;
    ee_thread_status_t st;
    memset(&t, 0, sizeof(t));
    ReferThreadStatus(GetThreadId(), &st);
    if (st.current_priority < 2) ChangeThreadPriority(GetThreadId(), 2); // the reporter must preempt a spinning main thread
    t.func = ps2_wd_main;
    t.stack = ps2_wd_stack;
    t.stack_size = sizeof(ps2_wd_stack);
    t.gp_reg = &_gp;
    t.initial_priority = 1;
    ps2_wd_thread = CreateThread(&t);
    StartThread(ps2_wd_thread, NULL);
    printf("[XASH-PS2] WATCHDOG thread=%d alarm=%d prio=%d\n",
        (int)ps2_wd_thread, (int)SetAlarm(0xFFFF, ps2_wd_alarm, NULL), st.current_priority);
}

/* Called first thing in main(): Memory_Init, Cmd/Cvar/Con_Init run before
 * Platform_Init, and a crash there used to leave a black screen. */
void PS2_EarlyInit(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); // a hang must not swallow the last lines sent to ps2client
    init_scr();
    ps2_screen_log = 1;
    scr_printf("\n  Xash3D PS2 starting...\n  launch: %s\n\n", PS2_FS_LaunchPath());
    ps2_install_exception_screen();
    SetSyscall(0, ps2_crash_stub);
}

void Platform_Init(void)
{
    scr_printf("  platform init\n");
    sceSifInitRpc(0);
    ps2_reset_iop_for_usb();
    InitTimer(0);
    StartTimerSystemTime();
    ps2_install_hang_watchdog(); // SetAlarm is syscall 0xfc, installed by InitTimer
    ps2_start_tick = GetTimerSystemTime();
    printf("[XASH-PS2] STAGE=PLATFORM_INIT\n");
}

void Platform_Shutdown(void)
{
    printf("[XASH-PS2] STAGE=PLATFORM_SHUTDOWN\n");
}

double Platform_DoubleTime(void)
{
    /* PS2SDK returns bus-clock ticks for the system timer. */
    return (double)(GetTimerSystemTime() - ps2_start_tick) / (double)kBUSCLK;
}

void Platform_Sleep(int msec)
{
    if (msec > 0) DelayThread((unsigned)msec * 1000);
}

/* The standard stderr message box already lives in engine/common/sys_con.c.
 * Do not duplicate it when statically linking the original engine. */
void Platform_ShellExecute(const char *path, const char *params)
{
    printf("[XASH-PS2] Open URL unsupported: %s %s\n",
           path ? path : "", params ? params : "");
}

void *Platform_GetNativeObject(const char *name) { (void)name; return NULL; }
void Platform_Vibrate(float life, char flags) { (void)life; (void)flags; }

void Platform_GetClipboardText(char *buffer, size_t size)
{
    if (buffer && size) buffer[0] = '\0';
}

void Platform_SetClipboardText(const char *buffer, size_t size)
{
    (void)buffer; (void)size;
}
