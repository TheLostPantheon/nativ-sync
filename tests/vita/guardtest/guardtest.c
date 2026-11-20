/* guardtest: a stand-in for the background service, for trying the launch guard on a Vita. Loaded into SceShell after
 * the kernel module, it reads a scenario from ux0:data/nativ-sync/test/scenario.txt whenever the file changes:
 *     <seq> <conflict|sync|none> <TITLEID> <answer after ms> <ok 0|1> <pc online 0|1>
 * sets that guard (the game's name from its param.sfo, sample save details), answers the guard's events as told, and
 * logs everything to ux0:data/nativ-sync/test/guardtest.txt. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "nsync_kernel.h"

#define DIR "ux0:data/nativ-sync/test"
void *memset(void *d, int c, unsigned int n) { return sceClibMemset(d, c, n); }   /* (no libc: the compiler's zeroing calls land here) */
static char logbuf[6144]; static int loglen; static SceInt64 t0;
static void flush(void) { SceUID f = sceIoOpen(DIR "/guardtest.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (f >= 0) { sceIoWrite(f, logbuf, loglen); sceIoClose(f); } }
#define LOG(...) do { if (loglen < (int)sizeof logbuf - 200) { loglen += sceClibSnprintf(logbuf + loglen, sizeof logbuf - loglen, "%6d ", (int)((sceKernelGetSystemTimeWide() - t0) / 1000)); \
	loglen += sceClibSnprintf(logbuf + loglen, sizeof logbuf - loglen, __VA_ARGS__); flush(); } } while (0)

static int read_file(const char *p, char *b, int cap) { SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); if (f < 0) return f; int n = sceIoRead(f, b, cap); sceIoClose(f); return n; }
static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void sfo_title(const char *titleid, char *out, int n) {   /* TITLE from ux0:app/<id>/sce_sys/param.sfo (stored plain) */
	static unsigned char b[8192]; char p[64]; out[0] = 0;
	sceClibSnprintf(p, sizeof p, "ux0:app/%s/sce_sys/param.sfo", titleid);
	int len = read_file(p, (char *)b, sizeof b);
	if (len < 20 || u32(b) != 0x46535000) return;
	unsigned keys = u32(b + 8), data = u32(b + 12), count = u32(b + 16);
	for (unsigned i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)len; i++) {
		const unsigned char *e = b + 20 + i * 16;
		unsigned ko = e[0] | e[1] << 8, dl = u32(e + 4), dof = u32(e + 12);
		if (keys + ko < (unsigned)len && !sceClibStrcmp((char *)b + keys + ko, "TITLE") && data + dof + dl <= (unsigned)len) {
			int k = 0; for (; k < n - 1 && k < (int)dl && b[data + dof + k]; k++) out[k] = b[data + dof + k];
			out[k] = 0; return;
		}
	}
}
static int word(const char **s, char *out, int n) {
	while (**s == ' ' || **s == '\n' || **s == '\r') (*s)++;
	int k = 0; while (**s && **s != ' ' && **s != '\n' && **s != '\r') { if (k < n - 1) out[k++] = **s; (*s)++; }
	out[k] = 0; return k;
}
static int num(const char **s) { char w[16]; word(s, w, sizeof w); int v = 0; for (char *c = w; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0'); return v; }

static int answer_ms, answer_ok;
static int service(SceSize a, void *p) {
	char last[128] = "", cur[128], kind[16], tid[16];
	for (;;) {
		int n = read_file(DIR "/scenario.txt", cur, sizeof cur - 1);
		if (n > 0) { cur[n] = 0; if (sceClibStrcmp(cur, last)) {
			sceClibStrncpy(last, cur, sizeof last);
			const char *s = cur; int seq = num(&s); word(&s, kind, sizeof kind); word(&s, tid, sizeof tid);
			answer_ms = num(&s); answer_ok = num(&s); int online = num(&s);
			NsyncGuardInfo g; sceClibMemset(&g, 0, sizeof g);
			g.state = !sceClibStrcmp(kind, "conflict") ? NSYNC_GUARD_CONFLICT : !sceClibStrcmp(kind, "sync") ? NSYNC_GUARD_SYNC : NSYNC_GUARD_NONE;
			sceClibStrncpy(g.titleid, tid, sizeof g.titleid - 1);
			sfo_title(tid, g.game, sizeof g.game);
			sceClibStrncpy(g.pc_name, "Test PC", sizeof g.pc_name);
			sceClibStrncpy(g.vita_when, "Today, 21:40", sizeof g.vita_when); sceClibStrncpy(g.vita_detail, "Sample: the Vita's save", sizeof g.vita_detail);
			sceClibStrncpy(g.pc_when, "Today, 18:02", sizeof g.pc_when); sceClibStrncpy(g.pc_detail, "Sample: the PC's save", sizeof g.pc_detail);
			nsyncGuardClear();
			int r = nsyncGuardSet(&g); int o = nsyncGuardPcOnline(online);
			LOG("scenario %d: %s %s ('%s'), answer after %d ms ok %d, pc online %d: set 0x%08X, online 0x%08X\n", seq, kind, tid, g.game, answer_ms, answer_ok, online, r, o);
		} }
		NsyncEvent e; sceClibMemset(&e, 0, sizeof e);
		int r = nsyncGuardWait(&e, 1000000);
		if (r == (int)NSYNC_ERROR_TIMEOUT) continue;
		if (r < 0) { LOG("wait 0x%08X\n", r); sceKernelDelayThread(1000000); continue; }
		static const char *const kinds[] = {"?", "LAUNCH", "CHOICE", "STARTED"}, *const choices[] = {"none", "keep the Vita's", "use the PC's", "later"};
		LOG("event %s %s choice %s pid 0x%X\n", kinds[e.kind <= 3 ? e.kind : 0], e.titleid, choices[e.choice >= 0 && e.choice <= 3 ? e.choice : 0], e.pid);
		if (e.kind == NSYNC_EVENT_LAUNCH || (e.kind == NSYNC_EVENT_CHOICE && e.choice == NSYNC_CHOICE_PC)) {
			sceKernelDelayThread(answer_ms * 1000);
			LOG("release %s ok %d: 0x%08X\n", e.titleid, answer_ok, nsyncGuardRelease(e.titleid, answer_ok));
		}
	}
	return 0;
}
void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize a, void *p) {
	t0 = sceKernelGetSystemTimeWide(); loglen = 0;
	sceIoMkdir(DIR, 0777); LOG("guardtest loaded\n");
	SceUID t = sceKernelCreateThread("nsync_guardtest", service, 0x10000100, 0x4000, 0, 0, NULL);
	if (t >= 0) sceKernelStartThread(t, 0, NULL); else LOG("thread 0x%08X\n", t);
	return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize a, void *p) { return SCE_KERNEL_STOP_SUCCESS; }
