/* DevCheck: proves the dev loop of docs/vita/DEV_SETUP.md on hardware and Vita3K.
 *   - logs to ux0:data/wowee/devcheck.log and, if ux0:data/wowee/loghost.txt holds
 *     "<ip>[:port]" (default port 9999), to a UDP listener on the dev machine
 *   - TRIANGLE: null write (core dump test)   START: quit
 *   - or write "crash" / "quit" into ux0:data/wowee/devcheck.cmd (lets a host script drive the
 *     app in an emulator with no key presses); the file is deleted once read
 */
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR_PATH "ux0:data/wowee"
#define LOG_PATH DIR_PATH "/devcheck.log"
#define HOST_PATH DIR_PATH "/loghost.txt"
#define CMD_PATH DIR_PATH "/devcheck.cmd"

static char net_memory[128 * 1024];
static int g_sock = -1;
static SceNetSockaddrIn g_dest;

static void net_log_init(void) {
    char buf[64] = {0};
    SceUID fd = sceIoOpen(HOST_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    buf[strcspn(buf, " \r\n\t")] = 0;
    int port = 9999;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam param = {net_memory, sizeof net_memory, 0};
    sceNetInit(&param);
    sceNetCtlInit();
    g_sock = sceNetSocket("devcheck", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    memset(&g_dest, 0, sizeof g_dest);
    g_dest.sin_family = SCE_NET_AF_INET;
    g_dest.sin_port = sceNetHtons((unsigned short)port);
    sceNetInetPton(SCE_NET_AF_INET, buf, &g_dest.sin_addr);
}

static void log_line(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';

    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, (unsigned)n);
        sceIoClose(fd);
    }
    if (g_sock >= 0)
        sceNetSendto(g_sock, line, (unsigned)n, 0, (SceNetSockaddr *)&g_dest, sizeof g_dest);
}

/* Returns 'c' (crash), 'q' (quit) or 0. */
static char read_command(void) {
    char buf[16] = {0};
    SceUID fd = sceIoOpen(CMD_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    sceIoRemove(CMD_PATH);
    return strncmp(buf, "crash", 5) == 0 ? 'c' : strncmp(buf, "quit", 4) == 0 ? 'q' : 0;
}

int main(void) {
    sceIoMkdir(DIR_PATH, 0777);
    net_log_init();
    log_line("devcheck started, net log %s", g_sock >= 0 ? "on" : "off");
    log_line("press TRIANGLE to crash (null write), START to quit");

    SceCtrlData pad;
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        char cmd = read_command();
        if ((pad.buttons & SCE_CTRL_START) || cmd == 'q')
            break;
        if ((pad.buttons & SCE_CTRL_TRIANGLE) || cmd == 'c') {
            log_line("crashing on purpose");
            *(volatile int *)0 = 1;
        }
        sceKernelDelayThread(50 * 1000);
    }
    log_line("devcheck exit");
    sceKernelExitProcess(0);
    return 0;
}
