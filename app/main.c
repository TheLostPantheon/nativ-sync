/* Nativ Sync, the app. For now a read-only check of the mount: is this firmware supported, which games have saves, and
 * for the one picked (X) the save's plain files as the game sees them, mounted, listed and unmounted again. Installing
 * the background service, pairing and status come next (README, Plan). */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <taihen.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/nsync_kernel.h"

#define MAXSAVES 256
static char saves[MAXSAVES][12]; static int nsaves;
static char lines[24][96]; static int nlines;
static void say(const char *fmt, const char *a, int b) { if (nlines < 24) snprintf(lines[nlines++], sizeof lines[0], fmt, a, b); }
static int title_id(const char *t) {
	if (strlen(t) != 9) return 0;
	for (int i = 0; i < 9; i++) if (i < 4 ? !(t[i] >= 'A' && t[i] <= 'Z') : !(t[i] >= '0' && t[i] <= '9')) return 0;
	return 1;
}
static void list_saves(void) {
	SceUID d = sceIoDopen("ux0:user/00/savedata"); SceIoDirent e;
	if (d < 0) return;
	while (nsaves < MAXSAVES && sceIoDread(d, &e) > 0) if (SCE_S_ISDIR(e.d_stat.st_mode) && title_id(e.d_name)) strcpy(saves[nsaves++], e.d_name);
	sceIoDclose(d);
}
static void check(const char *self, const char *t) {   /* the save's own files (not its sce_sys / sce_pfs) through the mount */
	char mp[16], path[64]; memset(mp, 0, sizeof mp); nlines = 0;
	int r = nsyncMountSave(t, self, mp);
	say("%s: mount 0x%08X", t, r);
	if (r < 0) return;
	snprintf(path, sizeof path, "ux0:user/00/savedata/%s", t);
	SceUID d = sceIoDopen(path); SceIoDirent e;
	if (d >= 0) { while (sceIoDread(d, &e) > 0) if (strcmp(e.d_name, "sce_sys") && strcmp(e.d_name, "sce_pfs")) say(SCE_S_ISDIR(e.d_stat.st_mode) ? "  %s/" : "  %s  (%d bytes)", e.d_name, (int)e.d_stat.st_size); sceIoDclose(d); }
	else say("  open 0x%08X", "", d);
	say("unmount%s 0x%08X", "", sceAppMgrUmount(mp));
}
int main(void) {
	char self[16] = "", fw[8] = "";
	int km = taiLoadStartKernelModule("app0:nsync_kernel.skprx", 0, NULL, 0);   /* (before app0 goes) */
	sceAppMgrAppParamGetString(0, 12, self, sizeof self);
	sceAppMgrUmount("app0:"); sceAppMgrUmount("savedata0:");   /* as Vita Save Manager does: this app's own mounts out of the way */
	int supported = km >= 0 || km == (int)0x8002D013 ? nsyncFirmware(fw) : 0;   /* (0x8002D013: loaded already) */
	list_saves();
	vita2d_init(); vita2d_set_clear_color(RGBA8(0x12, 0x14, 0x19, 0xFF));
	vita2d_pgf *font = vita2d_load_default_pgf();
	SceCtrlData pad, old; memset(&old, 0, sizeof old);
	int sel = 0;
	for (;;) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned press = pad.buttons & ~old.buttons; old = pad;
		if (press & SCE_CTRL_CIRCLE) break;
		if ((press & SCE_CTRL_DOWN) && sel + 1 < nsaves) sel++;
		if ((press & SCE_CTRL_UP) && sel > 0) sel--;
		if ((press & SCE_CTRL_CROSS) && supported && nsaves) check(self, saves[sel]);
		vita2d_start_drawing(); vita2d_clear_screen();
		vita2d_pgf_draw_text(font, 30, 40, RGBA8(0xF0, 0xB4, 0x4C, 0xFF), 1.2f, "Nativ Sync");
		char st[96];
		if (km < 0 && km != (int)0x8002D013) snprintf(st, sizeof st, "kernel module didn't load: 0x%08X", km);
		else snprintf(st, sizeof st, supported ? "firmware %s: supported" : "this firmware isn't supported yet", fw);
		vita2d_pgf_draw_text(font, 30, 70, RGBA8(0x90, 0x97, 0xA3, 0xFF), 1.0f, st);
		vita2d_pgf_draw_text(font, 30, 95, RGBA8(0x90, 0x97, 0xA3, 0xFF), 0.9f, "Up/Down: a game   X: read its save   O: quit   (reads only)");
		for (int i = 0, y = 130; i < nsaves && y < 520; i++, y += 22) {
			if (i < sel - 16) { y -= 22; continue; }
			vita2d_pgf_draw_text(font, 30, y, i == sel ? RGBA8(0xF0, 0xB4, 0x4C, 0xFF) : RGBA8(0xE9, 0xE6, 0xDF, 0xFF), 1.0f, saves[i]);
		}
		for (int i = 0; i < nlines; i++) vita2d_pgf_draw_text(font, 300, 130 + i * 22, RGBA8(0xE9, 0xE6, 0xDF, 0xFF), 0.95f, lines[i]);
		vita2d_end_drawing(); vita2d_swap_buffers();
	}
	vita2d_fini(); vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
