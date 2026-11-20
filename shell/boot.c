/* nsync_boot: what the taiHEN config loads into SceShell at boot (*main). taiHEN loads *main plugins as the shell
 * starts, before the shell has loaded its network libraries, and the service (which imports SceNet, SceNetCtl) can't
 * load then. This imports only libraries that are always there; its thread loads the service from the same folder once
 * that works, retrying while it doesn't, and logs each try. (The arrangement vagentd_boot has.) */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdarg.h>

#define LOG "ux0:data/nativ-sync/service.log"
#define TRIES 90   /* 2 s apart: three minutes */
static const char *const paths[] = {"ur0:tai/nativ-sync/nsync_service.suprx", "ux0:tai/nativ-sync/nsync_service.suprx"};
static volatile int running = 1;
static SceUID thread = -1;

static void logf_(const char *fmt, ...) {
	char m[160]; va_list ap; va_start(ap, fmt);
	int n = sceClibVsnprintf(m, sizeof m - 1, fmt, ap); va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof m - 2) n = sizeof m - 2;
	m[n++] = '\n';
	SceUID f = sceIoOpen(LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (f >= 0) { sceIoWrite(f, m, n); sceIoClose(f); }
}
static int unhealthy(void) {   /* boots in a row the service didn't confirm (the kernel module counts them) */
	char b[16]; int n = 0; sceClibMemset(b, 0, sizeof b);
	SceUID f = sceIoOpen("ur0:data/nativ-sync/health.txt", SCE_O_RDONLY, 0);
	if (f >= 0) { sceIoRead(f, b, sizeof b - 1); sceIoClose(f); }
	for (char *c = b; *c >= '0' && *c <= '9'; c++) n = n * 10 + (*c - '0');
	return n;
}
static int loader(SceSize a, void *p) {
	const char *path = 0; SceIoStat st;
	int n = unhealthy();
	if (n >= 3) { logf_("nsync_boot: switched off: the service didn't stay up %d starts in a row (the app turns it back on)", n - 1); return sceKernelExitDeleteThread(0); }
	for (int i = 0; i < 2 && !path; i++) if (sceIoGetstat(paths[i], &st) >= 0) path = paths[i];
	if (!path) { logf_("nsync_boot: no service file"); return sceKernelExitDeleteThread(0); }
	SceUID last = 0;
	for (int i = 0; running && i < TRIES; i++) {
		sceKernelDelayThread(2 * 1000 * 1000);
		int status = 0;
		SceUID id = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
		if (id >= 0 || id == (SceUID)0x8002D013) { logf_("nsync_boot: the service loaded after %d tries: 0x%08X", i + 1, id); return sceKernelExitDeleteThread(0); }
		if (id != last || i % 15 == 0) logf_("nsync_boot: try %d: 0x%08X", i + 1, id);
		last = id;
	}
	logf_("nsync_boot: gave up");
	return sceKernelExitDeleteThread(0);
}
void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
	sceIoMkdir("ux0:data/nativ-sync", 0777);
	thread = sceKernelCreateThread("nativ_sync_boot", loader, 160, 0x2000, 0, 0, NULL);
	if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) { running = 0; return SCE_KERNEL_STOP_SUCCESS; }
