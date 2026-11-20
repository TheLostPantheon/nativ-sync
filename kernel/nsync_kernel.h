/* nsync_kernel.h: Nativ Sync's kernel module, as the app and the background service call it (syscalls). */
#pragma once

#define NSYNC_ERROR_FIRMWARE 0x80F10001   /* a firmware this module has no table entry for */
#define NSYNC_ERROR_PROCESS  0x80F10002   /* SceAppMgr knows no such process */
#define NSYNC_ERROR_ARGUMENT 0x80F10003   /* not a title id, or a bad argument */
#define NSYNC_ERROR_FULL     0x80F10004   /* the guard table is full */
#define NSYNC_ERROR_TIMEOUT  0x80F10005   /* nothing happened in time */

/* mount <save_titleid>'s save decrypted for the calling process, which then reads and writes its plain files through
 * ux0:user/00/savedata/<save_titleid>/; caller_titleid is the caller's own (an app's, or NPXS19999 for the shell);
 * mount_point (16 bytes) receives what sceAppMgrUmount takes afterwards */
int nsyncMountSave(const char *save_titleid, const char *caller_titleid, char *mount_point);
int nsyncFirmware(char *name);   /* 1 and the firmware's name (6 bytes) when supported */

/* ---- the launch guard (DESIGN.md, Starting a game on the Vita) ----
 * The service tells the module which games need something before they start. When one is started from the home screen,
 * the module holds its launch (the game's splash stays on screen) and:
 *   NSYNC_GUARD_SYNC      posts NSYNC_EVENT_LAUNCH and waits for nsyncGuardRelease (the service syncs meanwhile), showing
 *                         a progress screen if it takes more than a moment;
 *   NSYNC_GUARD_CONFLICT  shows the choice over the splash. Keep the Vita's and Play without syncing go on at once; Use
 *                         the PC's waits for the release like a sync.
 * Every choice is posted as NSYNC_EVENT_CHOICE. With no service waiting for events (none in the last few seconds), the
 * guard holds nothing: a conflict still asks, but without the PC option. */
enum { NSYNC_GUARD_NONE, NSYNC_GUARD_SYNC, NSYNC_GUARD_CONFLICT };
enum { NSYNC_CHOICE_NONE, NSYNC_CHOICE_VITA, NSYNC_CHOICE_PC, NSYNC_CHOICE_LATER };
enum { NSYNC_EVENT_LAUNCH = 1, NSYNC_EVENT_CHOICE, NSYNC_EVENT_STARTED };

typedef struct {   /* UTF-8; empty strings are shown as sensible defaults */
	int state;                  /* NSYNC_GUARD_* */
	char titleid[12];
	char game[96];              /* the game's name */
	char pc_name[48];           /* the paired computer's name */
	char vita_when[48], vita_detail[96];   /* the Vita's save: when, and what (the game's own slot text) */
	char pc_when[48], pc_detail[96];       /* the PC's */
} NsyncGuardInfo;

typedef struct {
	int kind;                   /* NSYNC_EVENT_*; STARTED: any app's process started (title id, pid), so the service knows what runs */
	char titleid[12];
	int choice;                 /* NSYNC_CHOICE_*, for NSYNC_EVENT_CHOICE */
	int pid;                    /* for NSYNC_EVENT_STARTED */
} NsyncEvent;

int nsyncGuardSet(const NsyncGuardInfo *info);   /* add, replace or (state NONE) remove a title's entry */
int nsyncGuardClear(void);                       /* remove every entry */
int nsyncGuardPcOnline(int online);              /* whether the PC can be asked right now (the choice's PC option) */
/* wait up to timeout_us for an event; also marks the service as alive. NSYNC_ERROR_TIMEOUT when there was none. */
int nsyncGuardWait(NsyncEvent *event, unsigned int timeout_us);
/* the answer to a held launch: ok nonzero when its sync finished, else the game starts with the Vita's save after a
 * note on screen */
int nsyncGuardRelease(const char *titleid, int ok);
