/* Nativ Sync, the app (DESIGN.md, The Vita app): setting the background service up, pairing with a computer, and how
 * each game stands. The sync itself is the service's (shell/): this app only installs it, shows what it reports
 * (ux0:data/nativ-sync: pairs/, state/, status.txt) and opens pairing (pairing.txt, which the service reads).
 *
 * Installing copies the kernel module and the service beside the active taiHEN config (ux0:tai if it has one, else
 * ur0:tai) in a nativ-sync folder, and adds their two lines to that config; the config is backed up first, and
 * removing takes exactly those lines back out. Both take effect at the next restart. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/rng.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <taihen.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../user/nsync_user.h"

#define DATA "ux0:data/nativ-sync"
#define SAVES "ux0:user/00/savedata"
#define RGBA(r, g, b) RGBA8(r, g, b, 0xFF)
static const unsigned BG = RGBA(18, 20, 25), CARD = RGBA(28, 31, 39), CARD_HI = RGBA(38, 43, 54), INK = RGBA(233, 230, 223),
	DIM = RGBA(144, 151, 163), FAINT = RGBA(96, 102, 116), AMBER = RGBA(240, 180, 76), GOOD = RGBA(88, 194, 139), WARN = RGBA(232, 163, 61);

static vita2d_pgf *font;
static void trace(const char *fmt, int v) {   /* (start-up steps into DATA/app.log: where a crash at start happened) */
	char l[128]; int n = snprintf(l, sizeof l, fmt, v); l[n++] = '\n';
	SceUID f = sceIoOpen("ux0:data/nativ-sync/app.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (f >= 0) { sceIoWrite(f, l, n); sceIoClose(f); }
}
static char self_id[16], fw[8];
static int kernel_ok, has_firmware;

/* ---- small things ---- */
static int title_id(const char *t) {
	if (strlen(t) != 9) return 0;
	for (int i = 0; i < 9; i++) if (i < 4 ? !(t[i] >= 'A' && t[i] <= 'Z') : !(t[i] >= '0' && t[i] <= '9')) return 0;
	return 1;
}
static int read_file(const char *p, char *b, int cap) { SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); if (f < 0) return f; int n = sceIoRead(f, b, cap - 1); sceIoClose(f); if (n < 0) return n; b[n] = 0; return n; }
static int write_file(const char *p, const void *b, int n) { SceUID f = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (f < 0) return f; int w = sceIoWrite(f, b, n); sceIoClose(f); return w == n ? 0 : -1; }
static int exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }
static int copy_file(const char *src, const char *dst) {
	static char buf[64 * 1024];
	SceUID a = sceIoOpen(src, SCE_O_RDONLY, 0); if (a < 0) return a;
	SceUID b = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (b < 0) { sceIoClose(a); return b; }
	int n, r = 0; while ((n = sceIoRead(a, buf, sizeof buf)) > 0) if (sceIoWrite(b, buf, n) != n) { r = -1; break; }
	sceIoClose(a); sceIoClose(b); return n < 0 ? n : r;
}
static void text(int x, int y, unsigned c, float s, const char *t) { vita2d_pgf_draw_text(font, x, y, c, s, t); }
static int text_w(float s, const char *t) { return vita2d_pgf_text_width(font, s, t); }
static void text_fit(int x, int y, unsigned c, float s, const char *t, int max) {   /* cut with an ellipsis */
	if (text_w(s, t) <= max) { text(x, y, c, s, t); return; }
	char b[200]; int n = (int)strlen(t); if (n > 190) n = 190;
	while (n > 0) { memcpy(b, t, n); while (n > 0 && (b[n - 1] & 0xC0) == 0x80) n--; strcpy(b + n, "..."); if (text_w(s, b) <= max) break; n--; }
	text(x, y, c, s, b);
}
static void wrap(int x, int y, unsigned c, float s, const char *t, int max, int line_h) {   /* word-wrapped lines */
	char line[256] = ""; const char *p = t;
	while (*p) {
		const char *sp = p; while (*sp && *sp != ' ' && *sp != '\n') sp++;
		char trial[256]; snprintf(trial, sizeof trial, "%s%s%.*s", line, line[0] ? " " : "", (int)(sp - p), p);
		if (line[0] && text_w(s, trial) > max) { text(x, y, c, s, line); y += line_h; snprintf(line, sizeof line, "%.*s", (int)(sp - p), p); }
		else strcpy(line, trial);
		if (*sp == '\n') { text(x, y, c, s, line); y += line_h; line[0] = 0; }
		p = *sp ? sp + 1 : sp;
	}
	if (line[0]) text(x, y, c, s, line);
}
static unsigned u32le(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void sfo_title(const char *id, char *out, int cap) {
	static unsigned char b[8192]; char p[96]; out[0] = 0;
	snprintf(p, sizeof p, "ux0:app/%s/sce_sys/param.sfo", id);
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); if (f < 0) return;
	int len = sceIoRead(f, b, sizeof b); sceIoClose(f);
	if (len < 20 || u32le(b) != 0x46535000) return;
	unsigned keys = u32le(b + 8), data = u32le(b + 12), count = u32le(b + 16);
	for (unsigned i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)len; i++) {
		const unsigned char *e = b + 20 + i * 16; unsigned ko = e[0] | e[1] << 8, dl = u32le(e + 4), dof = u32le(e + 12);
		if (keys + ko < (unsigned)len && !strcmp((char *)b + keys + ko, "TITLE") && data + dof + dl <= (unsigned)len) {
			int k = 0; for (; k < cap - 1 && k < (int)dl && b[data + dof + k]; k++) out[k] = b[data + dof + k];
			out[k] = 0; return;
		}
	}
}

/* ---- what the service reports ---- */
typedef struct { char id[17], name[64]; } Pc;
static Pc pcs[8]; static int npcs;
static char online_pc[17];
typedef struct { char id[12], name[96], when[32]; long long when_stamp; int conflict; char pc_name[48], vita_when[48], vita_detail[96], pc_when[48], pc_detail[96]; vita2d_texture *icon; int icon_tried; } Game;
static Game games[200]; static int ngames;
static int service_alive;

static void load_pcs(void) {
	npcs = 0;
	SceUID d = sceIoDopen(DATA "/pairs"); if (d < 0) return;
	SceIoDirent e;
	while (npcs < 8 && sceIoDread(d, &e) > 0) {
		if (strlen(e.d_name) != 16) continue;
		char p[320], rec[100]; memset(rec, 0, sizeof rec); snprintf(p, sizeof p, DATA "/pairs/%s", e.d_name);
		SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); int n = f >= 0 ? sceIoRead(f, rec, 96) : 0; if (f >= 0) sceIoClose(f);
		strcpy(pcs[npcs].id, e.d_name);
		snprintf(pcs[npcs].name, sizeof pcs[npcs].name, "%.63s", n > 32 && rec[32] ? rec + 32 : "a computer");
		npcs++;
	}
	sceIoDclose(d);
}
static void load_status(void) {
	char s[1024]; online_pc[0] = 0;
	for (int i = 0; i < ngames; i++) games[i].conflict = 0;
	if (read_file(DATA "/status.txt", s, sizeof s) <= 0) return;
	for (char *l = strtok(s, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "online ", 7)) snprintf(online_pc, sizeof online_pc, "%s", l + 7);
		else if (!strncmp(l, "conflict ", 9)) for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, l + 9)) games[i].conflict = 1;
	}
}
static void load_conflicts(void) {   /* DATA/conflicts.txt (the service's): each side's when and what, for the choice */
	static char c[32 * 520];
	if (read_file(DATA "/conflicts.txt", c, sizeof c) <= 0) return;
	for (char *l = c; *l; ) {
		char *nl = l; while (*nl && *nl != '\n') nl++;
		char save = *nl; *nl = 0;
		char *f[8]; int n = 0; f[n++] = l;
		for (char *t = l; *t && n < 8; t++) if (*t == '\t') { *t = 0; f[n++] = t + 1; }
		if (n == 8) for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, f[0])) {
			Game *g = &games[i]; g->conflict = 1;
			snprintf(g->pc_name, sizeof g->pc_name, "%s", f[3]); snprintf(g->vita_when, sizeof g->vita_when, "%s", f[4]); snprintf(g->vita_detail, sizeof g->vita_detail, "%s", f[5]);
			snprintf(g->pc_when, sizeof g->pc_when, "%s", f[6]); snprintf(g->pc_detail, sizeof g->pc_detail, "%s", f[7]);
		}
		*nl = save; l = *nl ? nl + 1 : nl;
	}
}
static void pending_choice(void) {   /* a choice the service hasn't read yet: its game isn't waiting any more */
	char c[32]; if (read_file(DATA "/choice.txt", c, sizeof c) <= 0) return;
	for (int i = 0; i < ngames; i++) if (!strncmp(c, games[i].id, 9)) games[i].conflict = 0;
}
static long long stamp_of(const SceDateTime *t) { return ((((long long)t->year * 100 + t->month) * 100 + t->day) * 100 + t->hour) * 100 + t->minute; }
static void last_sync(Game *g) {   /* the newest base any paired computer recorded for it */
	SceDateTime best; memset(&best, 0, sizeof best); g->when[0] = 0;
	for (int i = 0; i < npcs; i++) {
		char p[128]; SceIoStat st; snprintf(p, sizeof p, DATA "/state/%s/%s.base", pcs[i].id, g->id);
		if (sceIoGetstat(p, &st) >= 0 && stamp_of(&st.st_mtime) > stamp_of(&best)) best = st.st_mtime;
	}
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	g->when_stamp = best.year ? stamp_of(&best) : 0;
	if (best.year) snprintf(g->when, sizeof g->when, "Synced %d %s", best.day, months[(best.month + 11) % 12]);   /* (a UTC date: near enough for a list) */
}
static int game_before(const Game *a, const Game *b) {   /* needs a choice, then synced (newest first), then the rest by name */
	int ra = a->conflict ? 0 : a->when_stamp ? 1 : 2, rb = b->conflict ? 0 : b->when_stamp ? 1 : 2;
	if (ra != rb) return ra < rb;
	if (ra == 1 && a->when_stamp != b->when_stamp) return a->when_stamp > b->when_stamp;
	return strcasecmp(a->name, b->name) < 0;
}
static void sort_games(void) {
	for (int i = 1; i < ngames; i++) for (int j = i; j > 0 && game_before(&games[j], &games[j - 1]); j--) { Game t = games[j]; games[j] = games[j - 1]; games[j - 1] = t; }
}
static void load_games(void) {
	for (int i = 0; i < ngames; i++) if (games[i].icon) vita2d_free_texture(games[i].icon);
	ngames = 0;
	SceUID d = sceIoDopen(SAVES); if (d < 0) return;
	SceIoDirent e;
	while (ngames < 200 && sceIoDread(d, &e) > 0) {
		if (!SCE_S_ISDIR(e.d_stat.st_mode) || !title_id(e.d_name)) continue;
		Game *g = &games[ngames]; memset(g, 0, sizeof *g);
		strcpy(g->id, e.d_name); sfo_title(g->id, g->name, sizeof g->name);
		if (!g->name[0]) memcpy(g->name, g->id, sizeof g->id);
		ngames++;
	}
	sceIoDclose(d);
	for (int i = 0; i < ngames; i++) last_sync(&games[i]);
	sort_games();
}
static int health(void) {   /* boots in a row the service didn't confirm: from 3 it's switched off */
	char b[16]; if (read_file("ur0:data/nativ-sync/health.txt", b, sizeof b) <= 0) return 0;
	return atoi(b);
}
static void refresh(void) { load_pcs(); load_games(); load_status(); load_conflicts(); pending_choice(); sort_games(); service_alive = kernel_ok ? nsyncUserServiceAlive() : 0; }
static int sel;
static void update(void) {   /* every couple of seconds on the home screen: new states, the order they give, the cursor kept on its game */
	char cur[12] = ""; if (sel < ngames) strcpy(cur, games[sel].id);
	for (int i = 0; i < ngames; i++) last_sync(&games[i]);
	load_status(); load_conflicts(); pending_choice(); sort_games();
	for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, cur)) sel = i;
	service_alive = kernel_ok ? nsyncUserServiceAlive() : 0;
}

/* ---- installing: the two lines in the active taiHEN config ---- */
static const char *config_path(void) { return exists("ux0:tai/config.txt") ? "ux0:tai/config.txt" : "ur0:tai/config.txt"; }
static void plugin_dir(char *out, int cap) { snprintf(out, cap, "%s/nativ-sync", !strcmp(config_path(), "ux0:tai/config.txt") ? "ux0:tai" : "ur0:tai"); }
static int installed(void) {
	static char c[16384]; if (read_file(config_path(), c, sizeof c) <= 0) return 0;
	return strstr(c, "nativ-sync/nsync_kernel.skprx") && strstr(c, "nativ-sync/nsync_boot.suprx");
}
/* the config with a line added under a section (*KERNEL, *main), the section made at the end when it has none */
static int add_line(char *c, int cap, const char *section, const char *line) {
	char hdr[32]; snprintf(hdr, sizeof hdr, "%s\n", section);
	char *at = strstr(c, hdr);
	int n = (int)strlen(c), ln = (int)strlen(line) + 1;
	if (!at) { if (n + (int)strlen(hdr) + ln + 2 >= cap) return -1; if (n && c[n - 1] != '\n') strcat(c, "\n"); strcat(c, hdr); strcat(c, line); strcat(c, "\n"); return 0; }
	char *ins = at + strlen(hdr);
	if (n + ln >= cap) return -1;
	memmove(ins + ln, ins, strlen(ins) + 1); memcpy(ins, line, ln - 1); ins[ln - 1] = '\n';
	return 0;
}
static int remove_lines(char *c, const char *needle) {   /* every line holding needle */
	int removed = 0; char *l = c;
	while (*l) {
		char *nl = strchr(l, '\n'); int len = nl ? (int)(nl - l) + 1 : (int)strlen(l);
		char save = l[len]; l[len] = 0; int hit = strstr(l, needle) != NULL; l[len] = save;
		if (hit) { memmove(l, l + len, strlen(l + len) + 1); removed++; } else l += len;
	}
	return removed;
}
static char install_msg[200];
static int do_install(void) {
	static char c[16384]; char dir[64], k[96], s[96], b[96], src[96], bak[96];
	const char *cfg = config_path();
	if (read_file(cfg, c, sizeof c - 512) < 0) { snprintf(install_msg, sizeof install_msg, "No taiHEN config at %s.", cfg); return -1; }
	plugin_dir(dir, sizeof dir); sceIoMkdir(dir, 0777);
	snprintf(k, sizeof k, "%s/nsync_kernel.skprx", dir); snprintf(s, sizeof s, "%s/nsync_service.suprx", dir); snprintf(b, sizeof b, "%s/nsync_boot.suprx", dir);
	snprintf(src, sizeof src, "ux0:app/%s/nsync_kernel.skprx", self_id); if (copy_file(src, k) < 0) { snprintf(install_msg, sizeof install_msg, "Couldn't copy the kernel module."); return -1; }
	snprintf(src, sizeof src, "ux0:app/%s/nsync_service.suprx", self_id); if (copy_file(src, s) < 0) { snprintf(install_msg, sizeof install_msg, "Couldn't copy the service."); return -1; }
	snprintf(src, sizeof src, "ux0:app/%s/nsync_boot.suprx", self_id); if (copy_file(src, b) < 0) { snprintf(install_msg, sizeof install_msg, "Couldn't copy the service's loader."); return -1; }
	snprintf(bak, sizeof bak, "%s.nativ-sync-backup", cfg); write_file(bak, c, (int)strlen(c));
	remove_lines(c, "nativ-sync/nsync_");   /* (an earlier install's lines: once each) */
	if (add_line(c, sizeof c, "*KERNEL", k) < 0 || add_line(c, sizeof c, "*main", b) < 0)   /* (the loader: the service can't load that early) */ { snprintf(install_msg, sizeof install_msg, "The config is too big to change safely."); return -1; }
	if (write_file(cfg, c, (int)strlen(c)) < 0) { snprintf(install_msg, sizeof install_msg, "Couldn't write %s (the backup is beside it).", cfg); return -1; }
	snprintf(install_msg, sizeof install_msg, "Installed. Restart your Vita to start it (the old config is %s).", bak);
	return 0;
}
static int do_remove(void) {
	static char c[16384]; char dir[64], p[96], bak[96];
	const char *cfg = config_path();
	if (read_file(cfg, c, sizeof c) < 0) { snprintf(install_msg, sizeof install_msg, "No taiHEN config at %s.", cfg); return -1; }
	snprintf(bak, sizeof bak, "%s.nativ-sync-backup", cfg); write_file(bak, c, (int)strlen(c));
	remove_lines(c, "nativ-sync/nsync_");
	if (write_file(cfg, c, (int)strlen(c)) < 0) { snprintf(install_msg, sizeof install_msg, "Couldn't write %s.", cfg); return -1; }
	plugin_dir(dir, sizeof dir);
	snprintf(p, sizeof p, "%s/nsync_service.suprx", dir); sceIoRemove(p);
	snprintf(p, sizeof p, "%s/nsync_boot.suprx", dir); sceIoRemove(p);
	snprintf(p, sizeof p, "%s/nsync_kernel.skprx", dir); sceIoRemove(p);
	sceIoRmdir(dir);
	snprintf(install_msg, sizeof install_msg, "Removed. It stops at the next restart; your pairings and backups stay in %s.", DATA);
	return 0;
}

/* ---- the screens ---- */
enum { HOME, PAIR, SETTINGS, GAME };
static int screen = HOME, scroll, settings_sel;
static char code[8]; static char pair_before[8][17]; static int pair_nbefore, paired_now; static SceInt64 pair_t0;
static void open_pairing(void) {
	unsigned r; sceKernelGetRandomNumber(&r, sizeof r);
	snprintf(code, sizeof code, "%06u", r % 1000000);
	sceIoMkdir(DATA, 0777); write_file(DATA "/pairing.txt", code, 6);
	load_pcs(); pair_nbefore = npcs; for (int i = 0; i < npcs; i++) strcpy(pair_before[i], pcs[i].id);
	paired_now = -1; pair_t0 = sceKernelGetProcessTimeWide();
	screen = PAIR;
}
static void close_pairing(void) { sceIoRemove(DATA "/pairing.txt"); screen = HOME; refresh(); }

static void cross(int cx, int cy, unsigned c) {   /* (the system font has no cross glyph) */
	for (int d = -1; d <= 1; d++) { vita2d_draw_line(cx - 5 + d, cy - 5, cx + 5 + d, cy + 5, c); vita2d_draw_line(cx - 5 + d, cy + 5, cx + 5 + d, cy - 5, c); }
}
static void button_hint(int x, int y, const char *glyph, unsigned gc, const char *label) {
	vita2d_draw_fill_circle(x + 11, y - 7, 12, CARD_HI);
	if (!strcmp(glyph, "\xE2\x9C\x95")) cross(x + 11, y - 7, gc);
	else text(x + 11 - text_w(1.0f, glyph) / 2, y - 1, gc, 1.0f, glyph);
	text(x + 30, y, INK, 1.0f, label);
}
static void draw_header(const char *title) {
	text(30, 46, AMBER, 1.1f, "Nativ Sync");
	if (title) text(30 + text_w(1.1f, "Nativ Sync") + 16, 46, DIM, 1.0f, title);
}
static void draw_home(void) {
	draw_header(NULL);
	char st[160];
	if (!kernel_ok) snprintf(st, sizeof st, has_firmware ? "Its kernel module didn't load." : "This firmware isn't supported.");
	else if (installed() && health() >= 3) snprintf(st, sizeof st, "Switched off: it failed to start %d times in a row. Settings turns it back on.", health() - 1);
	else if (!installed()) snprintf(st, sizeof st, "Not set up yet: Settings installs it.");
	else if (!service_alive) snprintf(st, sizeof st, "Set up: restart your Vita to start it.");
	else if (!npcs) snprintf(st, sizeof st, "Running. Pair a computer to start syncing.");
	else {
		const char *who = "";
		for (int i = 0; i < npcs; i++) if (!strcmp(pcs[i].id, online_pc)) who = pcs[i].name;
		if (who[0]) snprintf(st, sizeof st, "Running \xC2\xB7 connected to %s", who);
		else snprintf(st, sizeof st, "Running \xC2\xB7 paired with %s%s, not connected now", pcs[0].name, npcs > 1 ? " and others" : "");
	}
	vita2d_draw_fill_circle(36, 74, 5, service_alive ? GOOD : kernel_ok ? WARN : RGBA(229, 96, 90));
	text(50, 80, DIM, 1.0f, st);
	/* the games: icon, name, state */
	const int top = 104, row = 58, rows = 7;
	if (sel < scroll) scroll = sel;
	if (sel >= scroll + rows) scroll = sel - rows + 1;
	if (!ngames) text(30, top + 40, DIM, 1.0f, "No saves on this Vita yet.");
	for (int i = scroll; i < ngames && i < scroll + rows; i++) {
		Game *g = &games[i]; int y = top + (i - scroll) * row;
		vita2d_draw_rectangle(20, y, 920, row - 6, i == sel ? CARD_HI : CARD);
		if (i == sel) vita2d_draw_rectangle(20, y, 4, row - 6, AMBER);   /* (the focused row: plain to see) */
		if (!g->icon_tried) { char p[96]; snprintf(p, sizeof p, "ur0:appmeta/%s/icon0.png", g->id); g->icon = vita2d_load_PNG_file(p); g->icon_tried = 1; }
		if (g->icon) vita2d_draw_texture_scale(g->icon, 28, y + 3, 46.0f / vita2d_texture_get_width(g->icon), 46.0f / vita2d_texture_get_height(g->icon));
		text_fit(88, y + 33, INK, 1.0f, g->name, 560);
		const char *state = g->conflict ? "Needs a choice" : g->when[0] ? g->when : "Not synced yet";
		unsigned sc = g->conflict ? WARN : g->when[0] ? GOOD : FAINT;
		text(920 - 12 - text_w(1.0f, state), y + 33, sc, 1.0f, state);
	}
	if (ngames > rows) { char m[32]; snprintf(m, sizeof m, "%d of %d", sel + 1, ngames); text(920 - text_w(1.0f, m), 524, FAINT, 1.0f, m); }
	button_hint(30, 524, "\xE2\x9C\x95", RGBA(122, 166, 255), "Open");
	button_hint(150, 524, "\xE2\x96\xB3", RGBA(88, 214, 171), "Pair a computer");
	button_hint(370, 524, "\xE2\x96\xA1", RGBA(240, 140, 200), "Settings");
	button_hint(540, 524, "\xE2\x97\x8B", RGBA(255, 110, 110), "Quit");
}
static void draw_pair(void) {
	draw_header("Pair with a computer");
	vita2d_draw_rectangle(120, 110, 720, 320, CARD);
	if (paired_now >= 0) {
		char m[160]; snprintf(m, sizeof m, "Paired with %s", pcs[paired_now].name);
		text(480 - text_w(1.5f, m) / 2, 230, GOOD, 1.5f, m);
		wrap(160, 290, DIM, 1.0f, "Its games sync from now on: before a game starts and after it closes, on either side.", 640, 28);
	} else if (!exists(DATA "/pairing.txt")) {
		text(480 - text_w(1.2f, "Pairing closed") / 2, 230, WARN, 1.2f, "Pairing closed");
		wrap(160, 280, DIM, 1.0f, "The code ran out, or three wrong codes closed it so a guess can't get in. Go back and open pairing again for a new code.", 640, 28);
	} else {
		text(160, 160, DIM, 1.0f, "On your computer, in Nativ:");
		text(160, 192, INK, 1.0f, "Settings, Vita sync, Find my Vita, then type");
		char spaced[16]; snprintf(spaced, sizeof spaced, "%c%c%c %c%c%c", code[0], code[1], code[2], code[3], code[4], code[5]);
		text(480 - text_w(3.2f, spaced) / 2, 300, AMBER, 3.2f, spaced);
		int left = 300 - (int)((sceKernelGetProcessTimeWide() - pair_t0) / 1000000); if (left < 0) left = 0;
		char t[64]; snprintf(t, sizeof t, "This code works for %d:%02d more.", left / 60, left % 60);
		text(480 - text_w(1.0f, t) / 2, 380, FAINT, 1.0f, t);
	}
	button_hint(30, 524, "\xE2\x97\x8B", RGBA(255, 110, 110), "Back");
}
/* one game: its state, and for one needing a choice both saves and the choice (DATA/choice.txt, which the service
 * reads within a second and hands to the computer: now when it's connected, else when it next connects) */
static char game_id[12], game_msg[200];
static Game *cur_game(void) { for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, game_id)) return &games[i]; return 0; }
static const char *or_(const char *s, const char *d) { return s && s[0] ? s : d; }
static int pc_online(void) { return online_pc[0] != 0; }
static void choose(Game *g, const char *c) {
	char line[32]; int n = snprintf(line, sizeof line, "%s %s\n", g->id, c);
	write_file(DATA "/choice.txt", line, n);
	const char *pc = or_(g->pc_name, "your computer");
	if (!strcmp(c, "vita")) snprintf(game_msg, sizeof game_msg, pc_online() ? "Keeping the Vita's save: %s takes it now, and backs its own up." : "Keeping the Vita's save: %s takes it when it next connects, and backs its own up.", pc);
	else snprintf(game_msg, sizeof game_msg, pc_online() ? "Using %s's save: it's on its way. This Vita's save is backed up first." : "Using %s's save: it comes when that computer next connects. This Vita's save is backed up first.", pc);
	g->conflict = 0;
}
static void play_now(Game *g) {
	char uri[64]; snprintf(uri, sizeof uri, "psgm:play?titleid=%s", g->id);
	sceAppMgrLaunchAppByUri(0xFFFFF, uri);
	sceKernelExitProcess(0);
}
static void side(int x, const char *who, const char *when, const char *what) {
	vita2d_draw_rectangle(x, 200, 410, 120, CARD_HI);
	text(x + 18, 232, AMBER, 1.0f, who);
	text_fit(x + 18, 266, INK, 1.0f, or_(when, "Changed since the last sync"), 374);
	text_fit(x + 18, 298, DIM, 1.0f, or_(what, ""), 374);
}
static void draw_game(void) {
	Game *g = cur_game();
	draw_header(NULL);
	if (!g) { screen = HOME; return; }
	if (g->icon) vita2d_draw_texture_scale(g->icon, 30, 66, 60.0f / vita2d_texture_get_width(g->icon), 60.0f / vita2d_texture_get_height(g->icon));
	text_fit(106, 108, INK, 1.4f, g->name, 820);
	if (g->conflict) {
		char l[200]; snprintf(l, sizeof l, "Your Vita and %s both have new progress since the last sync. Which save should this game continue from?", or_(g->pc_name, "your computer"));
		wrap(30, 158, DIM, 1.0f, l, 900, 26);
		char on_pc[80]; snprintf(on_pc, sizeof on_pc, "On %s", or_(g->pc_name, "your computer"));
		side(30, "On this Vita", g->vita_when, g->vita_detail); side(520, on_pc, g->pc_when, g->pc_detail);
		wrap(30, 360, FAINT, 1.0f, "The save you don't keep is backed up, so you can change your mind later.", 900, 24);
		button_hint(30, 524, "\xE2\x9C\x95", RGBA(122, 166, 255), "Keep the Vita's");
		button_hint(240, 524, "\xE2\x96\xA1", RGBA(240, 140, 200), "Use the computer's");
		button_hint(490, 524, "\xE2\x96\xB3", RGBA(88, 214, 171), "Play now");
		button_hint(660, 524, "\xE2\x97\x8B", RGBA(255, 110, 110), "Back");
	} else {
		text(30, 170, g->when[0] ? GOOD : DIM, 1.0f, g->when[0] ? g->when : "Not synced yet");
		if (game_msg[0]) wrap(30, 220, WARN, 1.0f, game_msg, 900, 26);
		else wrap(30, 220, DIM, 1.0f, "It syncs on its own: before it starts and after it closes, here and on a paired computer.", 900, 26);
		button_hint(30, 524, "\xE2\x96\xB3", RGBA(88, 214, 171), "Play now");
		button_hint(200, 524, "\xE2\x97\x8B", RGBA(255, 110, 110), "Back");
	}
}
static void draw_settings(void) {
	draw_header("Settings");
	int y = 100, inst = installed();
	const char *items[12]; int n = 0;
	int off = inst && health() >= 3;
	items[n++] = off ? "Turn the background service back on" : inst ? "Remove the background service" : "Install the background service";
	for (int i = 0; i < npcs && n < 10; i++) items[n++] = pcs[i].name;
	if (settings_sel >= n) settings_sel = n - 1;
	wrap(30, y, DIM, 1.0f, inst ? "The service starts with your Vita and syncs in the background. Removing it takes effect at the next restart; your pairings and backups stay."
	                            : "The service starts with your Vita and syncs in the background. Installing adds two lines to your taiHEN config (backed up first) and takes effect at the next restart.", 880, 24);
	y += 66;
	for (int i = 0; i < n; i++) {
		if (i == 1) { text(30, y + 22, DIM, 1.0f, "Paired computers (Cross removes one):"); y += 34; }
		vita2d_draw_rectangle(20, y, 920, 44, i == settings_sel ? CARD_HI : CARD);
		if (i == settings_sel) vita2d_draw_rectangle(20, y, 4, 44, AMBER);
		text_fit(36, y + 30, i == 0 ? AMBER : INK, 1.0f, items[i], 860);
		y += 50;
	}
	if (install_msg[0]) wrap(30, y + 30, WARN, 1.0f, install_msg, 880, 24);
	char about[96]; snprintf(about, sizeof about, "Firmware %s \xC2\xB7 Nativ Sync 0.1", has_firmware ? fw : "not supported");
	text(30, 494, FAINT, 1.0f, about);
	button_hint(30, 524, "\xE2\x9C\x95", RGBA(122, 166, 255), "Choose");
	button_hint(200, 524, "\xE2\x97\x8B", RGBA(255, 110, 110), "Back");
	if (install_msg[0] && strstr(install_msg, "Restart")) button_hint(360, 524, "\xE2\x96\xB3", RGBA(88, 214, 171), "Restart now");
}

int main(void) {
	sceAppMgrAppParamGetString(0, 12, self_id, sizeof self_id);
	char kpath[96];
	snprintf(kpath, sizeof kpath, "ux0:app/%s/nsync_kernel.skprx", self_id);   /* (not app0: the kernel can't see a process's own mount) */
	sceIoRemove("ux0:data/nativ-sync/app.log"); sceIoMkdir("ux0:data/nativ-sync", 0777);
	int km = taiLoadStartKernelModule(kpath, 4, "app", 0);   /* ("app": not a boot, for its start count) */
	trace("kernel module 0x%08X", km);
	snprintf(kpath, sizeof kpath, "ux0:app/%s/nsync_user.suprx", self_id);   /* the bridge, bound to it now (nsync_user.c) */
	int um = sceKernelLoadStartModule(kpath, 0, NULL, 0, NULL, NULL);
	trace("bridge 0x%08X", um);
	kernel_ok = (km >= 0 || km == (int)0x8002D013) && um >= 0;
	has_firmware = kernel_ok && nsyncUserFirmware(fw);
	kernel_ok = kernel_ok && has_firmware;
	trace("firmware known %d", has_firmware);
	vita2d_init(); vita2d_set_clear_color(BG);
	font = vita2d_load_default_pgf();
	trace("drawing ready %d", font != NULL);
	load_pcs(); trace("computers %d", npcs);
	load_games(); trace("games %d", ngames);
	load_status(); trace("status read %d", 1);
	service_alive = kernel_ok ? nsyncUserServiceAlive() : 0; trace("service alive %d", service_alive);
	SceCtrlData pad, old; memset(&old, 0, sizeof old);
	int frame = 0;
	for (;;) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned press = pad.buttons & ~old.buttons; old = pad;
		if (++frame % 120 == 0) {
			if (screen == HOME) update();
			if (screen == PAIR && paired_now < 0) {   /* the computer whose id is new (folder order isn't pairing order) */
				load_pcs();
				for (int i = 0; i < npcs && paired_now < 0; i++) { int old = 0; for (int j = 0; j < pair_nbefore; j++) if (!strcmp(pcs[i].id, pair_before[j])) old = 1; if (!old) paired_now = i; }
			}
			if (screen == PAIR && paired_now < 0 && sceKernelGetProcessTimeWide() - pair_t0 > 300000000LL) sceIoRemove(DATA "/pairing.txt");
		}
		if (screen == HOME) {
			if (press & SCE_CTRL_CIRCLE) break;
			if ((press & SCE_CTRL_DOWN) && sel + 1 < ngames) sel++;
			if ((press & SCE_CTRL_UP) && sel > 0) sel--;
			if (press & SCE_CTRL_TRIANGLE) open_pairing();
			if (press & SCE_CTRL_SQUARE) { screen = SETTINGS; settings_sel = 0; install_msg[0] = 0; }
			if ((press & SCE_CTRL_CROSS) && sel < ngames) { snprintf(game_id, sizeof game_id, "%s", games[sel].id); game_msg[0] = 0; screen = GAME; }
		} else if (screen == GAME) {
			Game *g = cur_game();
			if (press & SCE_CTRL_CIRCLE) { screen = HOME; update(); }
			else if (g && (press & SCE_CTRL_TRIANGLE)) play_now(g);
			else if (g && g->conflict && (press & SCE_CTRL_CROSS)) choose(g, "vita");
			else if (g && g->conflict && (press & SCE_CTRL_SQUARE)) choose(g, "pc");
		} else if (screen == PAIR) {
			if (press & SCE_CTRL_CIRCLE) close_pairing();
		} else {
			int n = 1 + npcs;
			if (press & SCE_CTRL_CIRCLE) { screen = HOME; refresh(); }
			if ((press & SCE_CTRL_DOWN) && settings_sel + 1 < n) settings_sel++;
			if ((press & SCE_CTRL_UP) && settings_sel > 0) settings_sel--;
			if (press & SCE_CTRL_CROSS) {
				if (settings_sel == 0) {
					if (installed() && health() >= 3) { write_file("ur0:data/nativ-sync/health.txt", "0\n", 2); snprintf(install_msg, sizeof install_msg, "Turned back on. Restart your Vita to start it."); }
					else if (installed()) do_remove(); else if (kernel_ok) do_install(); else snprintf(install_msg, sizeof install_msg, "This firmware isn't supported, so it can't be installed.");
				}
				else {
					char p[96]; snprintf(p, sizeof p, DATA "/pairs/%s", pcs[settings_sel - 1].id); sceIoRemove(p);
					snprintf(install_msg, sizeof install_msg, "Removed %s. Pair it again to sync with it.", pcs[settings_sel - 1].name); load_pcs();
				}
			}
			if ((press & SCE_CTRL_TRIANGLE) && strstr(install_msg, "Restart")) scePowerRequestColdReset();
		}
		vita2d_start_drawing(); vita2d_clear_screen();
		if (frame == 1) trace("first frame %d", screen);
		if (screen == HOME) draw_home(); else if (screen == PAIR) draw_pair(); else if (screen == GAME) draw_game(); else draw_settings();
		vita2d_end_drawing(); vita2d_swap_buffers();
	}
	if (screen == PAIR) sceIoRemove(DATA "/pairing.txt");
	vita2d_fini(); vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
