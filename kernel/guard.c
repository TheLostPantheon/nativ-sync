/* guard.c: the launch guard (nsync_kernel.h, DESIGN.md "Starting a game on the Vita").
 *
 * Every app start goes through SceProcessmgr's ksceKernelCreateProcess, called by SceAppMgr on its launch thread with the
 * title id and the path being started. Holding that call keeps the game's start-up splash on screen; meanwhile this
 * module can show its own picture on the display's overlay layer (1, the one the shell's own overlays use, blended over
 * the splash), read the buttons, and wait for the background service, which syncs in the shell while the launch waits
 * (checked on 3.65: SceAppMgr holds no lock across the launch, a save mounts in 30 ms meanwhile). The hook's work runs
 * on a stack of its own: the launch thread's is small, and overrunning it stopped every launch until a reboot. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/display.h>
#include <psp2kern/ctrl.h>
#include <string.h>
#include <taihen.h>
#include "nsync_kernel.h"
#include "screens.h"

int ksceKernelRunWithStack(SceSize stack_size, int (*to_call)(void *), void *args);

#define MAX_GUARDS 32
#define SERVICE_ALIVE_US  15000000   /* a service that waited for events this recently is running */
#define CHOICE_US         60000000   /* no answer: play without syncing */
#define SYNC_QUIET_US      1000000   /* a sync this quick shows nothing over the splash */
#define SYNC_LIMIT_US     25000000   /* then the game starts anyway */
#define NOTE_US            4000000

static NsyncGuardInfo guards[MAX_GUARDS];
static NsyncEvent queue[32]; static int q_first, q_count;
static SceUID lock = -1, events = -1;
static int pc_online;
static SceInt64 service_seen = -SERVICE_ALIVE_US;
static volatile int released, release_ok; static char held[12];
static tai_hook_ref_t hook_ref; static SceUID hook = -1;

static SceInt64 now(void) { return ksceKernelGetSystemTimeWide(); }
static void enter(void) { ksceKernelLockMutex(lock, 1, NULL); }
static void leave(void) { ksceKernelUnlockMutex(lock, 1); }
static void post(int kind, const char *titleid, int choice, int pid) {
	enter();
	if (q_count < (int)(sizeof queue / sizeof *queue)) {
		NsyncEvent *e = &queue[(q_first + q_count++) % (sizeof queue / sizeof *queue)];
		memset(e, 0, sizeof *e); e->kind = kind; strncpy(e->titleid, titleid, 11); e->choice = choice; e->pid = pid;
		ksceKernelSignalSema(events, 1);
	}
	leave();
}

/* ---- the overlay ---- */
static SceUID fb_uid = -1; static Canvas canvas; static SceDisplayFrameBuf fb; static int head;
static int overlay_ready(void) {   /* the framebuffer, made once and kept: the display may still point at it */
	if (fb_uid < 0) {
		fb_uid = ksceKernelAllocMemBlock("NativSyncOverlay", 0x40404006, 0x240000, NULL);   /* kernel CDRAM, as BubbleLock */
		if (fb_uid < 0) return 0;
		void *base; ksceKernelGetMemBlockBase(fb_uid, &base);
		canvas.px = base; canvas.w = 960; canvas.h = 544; canvas.pitch = 1024;
	}
	memset(&fb, 0, sizeof fb); fb.size = sizeof fb; fb.base = canvas.px; fb.pitch = 1024;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = 960; fb.height = 544;
	head = ksceDisplayGetPrimaryHead();
	return 1;
}
static void show(void) { ksceDisplaySetFrameBufInternal(head, 1, &fb, SCE_DISPLAY_SETBUF_NEXTFRAME); ksceDisplayWaitVblankStart(); }
static void hide(void) { screen_clear(&canvas); show(); }
static uint32_t buttons(void) { SceCtrlData pad; memset(&pad, 0, sizeof pad); ksceCtrlPeekBufferPositive(0, &pad, 1); return pad.buttons; }

static void cat(char *d, int n, const char *a, const char *b, const char *c) {
	const char *s[3] = {a, b, c}; int k = 0;
	for (int i = 0; i < 3; i++) for (const char *p = s[i]; p && *p && k < n - 1; p++) d[k++] = *p;
	d[k] = 0;
}

static int ask(const NsyncGuardInfo *g, int pc_ok) {   /* the conflict choice; NSYNC_CHOICE_LATER on no answer */
	if (!overlay_ready()) return NSYNC_CHOICE_LATER;
	screen_choice(&canvas, g, pc_ok);
	uint32_t old = buttons();   /* (a button still down from starting the game doesn't count) */
	int choice = NSYNC_CHOICE_NONE; SceInt64 t0 = now();
	while (!choice && now() - t0 < CHOICE_US) {
		show();
		uint32_t b = buttons(), press = b & ~old; old = b;
		if (press & SCE_CTRL_CROSS) choice = NSYNC_CHOICE_VITA;
		else if ((press & SCE_CTRL_SQUARE) && pc_ok) choice = NSYNC_CHOICE_PC;
		else if (press & SCE_CTRL_CIRCLE) choice = NSYNC_CHOICE_LATER;
	}
	hide();
	return choice ? choice : NSYNC_CHOICE_LATER;
}

/* wait for the service's nsyncGuardRelease of this launch: quietly at first, then with a progress screen; on failure or
 * no answer, a note and the game starts with the Vita's save */
static void wait_release(const NsyncGuardInfo *g, const char *doing) {
	SceInt64 t0 = now(); int shown = 0;
	while (!released && now() - t0 < SYNC_LIMIT_US) {
		if (now() - t0 < SYNC_QUIET_US) { ksceKernelDelayThread(10000); continue; }
		if (!shown && overlay_ready()) { screen_progress(&canvas, g->game[0] ? g->game : g->titleid, doing, 0); shown = 1; }
		if (shown) show(); else ksceKernelDelayThread(10000);
	}
	if (released && release_ok) { if (shown) hide(); return; }
	if (overlay_ready()) {
		char l1[120];
		cat(l1, sizeof l1, g->pc_name[0] ? g->pc_name : "Your PC", released ? " couldn't finish the sync." : " didn't answer in time.", 0);
		screen_progress(&canvas, g->game[0] ? g->game : g->titleid, l1, "The game starts with the save on this Vita.");
		for (SceInt64 t1 = now(); now() - t1 < NOTE_US; ) show();
		hide();
	}
}

typedef struct { const char *titleid; int type; const char *path; void *opt; } Launch;
static NsyncGuardInfo cur;   /* (one launch at a time: SceAppMgr's single launch thread) */
static int guarded(Launch *l) {
	char t[12]; memset(t, 0, sizeof t);
	if (l->titleid) strncpy(t, l->titleid, 11);
	int found = 0;
	enter();
	for (int i = 0; i < MAX_GUARDS; i++) if (guards[i].state && !strcmp(guards[i].titleid, t)) { cur = guards[i]; found = 1; break; }
	int alive = now() - service_seen < SERVICE_ALIVE_US, online = pc_online;
	leave();
	if (found) {
		char doing[120];
		memcpy(held, t, sizeof held); released = 0; release_ok = 0;
		if (cur.state == NSYNC_GUARD_SYNC && alive) {
			post(NSYNC_EVENT_LAUNCH, t, 0, 0);
			cat(doing, sizeof doing, "Syncing with ", cur.pc_name[0] ? cur.pc_name : "your PC", "\xE2\x80\xA6");
			wait_release(&cur, doing);
		} else if (cur.state == NSYNC_GUARD_CONFLICT) {
			int choice = ask(&cur, alive && online);
			post(NSYNC_EVENT_CHOICE, t, choice, 0);
			if (choice == NSYNC_CHOICE_PC) {
				cat(doing, sizeof doing, "Getting your save from ", cur.pc_name[0] ? cur.pc_name : "your PC", "\xE2\x80\xA6");
				wait_release(&cur, doing);
			}
			if (choice == NSYNC_CHOICE_VITA || choice == NSYNC_CHOICE_PC) {   /* settled here: the service sets it again if need be */
				enter();
				for (int i = 0; i < MAX_GUARDS; i++) if (guards[i].state && !strcmp(guards[i].titleid, t)) guards[i].state = NSYNC_GUARD_NONE;
				leave();
			}
		}
		held[0] = 0;
	}
	SceUID r = TAI_CONTINUE(SceUID, hook_ref, l->titleid, l->type, l->path, l->opt);
	if (r >= 0) post(NSYNC_EVENT_STARTED, t, 0, r);
	return r;
}
static SceUID create_process(const char *titleid, int type, const char *path, void *opt) {
	Launch l = {titleid, type, path, opt};
	return ksceKernelRunWithStack(0x4000, (int (*)(void *))guarded, &l);
}

/* ---- syscalls ---- */
static int valid_id(const char *t) {
	for (int i = 0; i < 9; i++) if (i < 4 ? !(t[i] >= 'A' && t[i] <= 'Z') : !(t[i] >= '0' && t[i] <= '9')) return 0;
	return t[9] == 0;
}
int nsyncGuardSet(const NsyncGuardInfo *info) {
	uint32_t st; ENTER_SYSCALL(st);
	static NsyncGuardInfo g;   /* (under the lock below; the struct is too big for a syscall stack) */
	int res = 0;
	enter();
	if (ksceKernelCopyFromUser(&g, info, sizeof g) < 0) res = NSYNC_ERROR_ARGUMENT;
	else {
		g.titleid[11] = g.game[95] = g.pc_name[47] = g.vita_when[47] = g.vita_detail[95] = g.pc_when[47] = g.pc_detail[95] = 0;
		if (!valid_id(g.titleid) || g.state < 0 || g.state > NSYNC_GUARD_CONFLICT) res = NSYNC_ERROR_ARGUMENT;
		else {
			int slot = -1;
			for (int i = 0; i < MAX_GUARDS; i++) if (guards[i].state && !strcmp(guards[i].titleid, g.titleid)) slot = i;
			if (slot < 0 && g.state) for (int i = 0; i < MAX_GUARDS && slot < 0; i++) if (!guards[i].state) slot = i;
			if (slot < 0) res = g.state ? NSYNC_ERROR_FULL : 0;
			else guards[slot] = g;
		}
	}
	leave();
	EXIT_SYSCALL(st);
	return res;
}
int nsyncGuardClear(void) {
	uint32_t st; ENTER_SYSCALL(st);
	enter(); memset(guards, 0, sizeof guards); leave();
	EXIT_SYSCALL(st);
	return 0;
}
int nsyncGuardPcOnline(int online) {
	uint32_t st; ENTER_SYSCALL(st);
	pc_online = online != 0;
	EXIT_SYSCALL(st);
	return 0;
}
int nsyncGuardWait(NsyncEvent *event, unsigned int timeout_us) {
	uint32_t st; ENTER_SYSCALL(st);
	service_seen = now();
	SceUInt timeout = timeout_us;
	int res = ksceKernelWaitSema(events, 1, &timeout);
	service_seen = now();
	if (res < 0) res = NSYNC_ERROR_TIMEOUT;
	else {
		NsyncEvent e;
		enter(); e = queue[q_first]; q_first = (q_first + 1) % (sizeof queue / sizeof *queue); q_count--; leave();
		res = ksceKernelCopyToUser(event, &e, sizeof e) < 0 ? NSYNC_ERROR_ARGUMENT : 0;
	}
	EXIT_SYSCALL(st);
	return res;
}
int nsyncGuardRelease(const char *titleid, int ok) {
	uint32_t st; ENTER_SYSCALL(st);
	char t[12]; memset(t, 0, sizeof t);
	ksceKernelStrncpyFromUser(t, titleid, 11);
	int res = NSYNC_ERROR_ARGUMENT;
	if (held[0] && !strcmp(held, t)) { release_ok = ok != 0; released = 1; res = 0; }
	EXIT_SYSCALL(st);
	return res;
}

/* ---- start and stop (from module_start / module_stop) ---- */
int guard_start(void) {
	lock = ksceKernelCreateMutex("NativSyncGuard", 0, 0, NULL);
	events = ksceKernelCreateSema("NativSyncEvents", 0, 0, sizeof queue / sizeof *queue, NULL);
	if (lock < 0 || events < 0) return -1;
	/* ksceKernelCreateProcess: SceProcessmgrForKernel 0xEB1F8EF7 / 0x68068618 from 3.63 on, 0x71CF71FD on 3.60 */
	hook = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref, "SceProcessmgr", 0xEB1F8EF7, 0x68068618, create_process);
	if (hook < 0) hook = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref, "SceProcessmgr", TAI_ANY_LIBRARY, 0x71CF71FD, create_process);
	return hook;
}
void guard_stop(void) {
	if (hook >= 0) taiHookReleaseForKernel(hook, hook_ref);
	hook = -1;
}
