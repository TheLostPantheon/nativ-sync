/* saves.c: a save's files and slots, its manifest, staging, commits and backups (PROTOCOL.md, Manifests and Requests).
 * Every call that touches a save mounts it decrypted for the shell (the kernel module), works through the save's own
 * path, and unmounts before returning; the caller holds the launch guard around it so the game can't start meanwhile. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/rtc.h>
#include "svc.h"

#define MAX_FILES 128
#define PATHLEN 160
#define SLOTS 256
#define SLOT_PARAM 0x34C
#define SLOT_FILE "sce_sys/sdslot.dat"
#define KEEP_BACKUPS 10

typedef struct { char path[PATHLEN]; uint32_t size; uint8_t hash[32]; } Entry;
typedef struct { int n, nslots; Entry e[MAX_FILES]; struct { int slot; uint8_t hash[32]; } s[SLOTS]; } Listing;

static Listing want, have;               /* (one request at a time: the service serializes them) */
static uint8_t iobuf[64 * 1024];
static uint8_t param[SLOT_PARAM];
static char mp[16];

#define strcmp_ sceClibStrcmp
static int len_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void cpy(char *d, const char *s, int cap) { int i = 0; for (; i < cap - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int lower_eq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) { char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b; if (x != y) return 0; }
	return !*a && !*b;
}

/* ---- mounting ---- */
static int mount_save(const char *id) { sceClibMemset(mp, 0, sizeof mp); return nsyncMountSave(id, SHELL_TITLE, mp); }
static void unmount_save(void) { if (mp[0]) sceAppMgrUmount(mp); mp[0] = 0; }
static void save_path(char *out, int cap, const char *id, const char *rel) {
	if (rel && rel[0]) sceClibSnprintf(out, cap, SAVES "/%s/%s", id, rel); else sceClibSnprintf(out, cap, SAVES "/%s", id);
}

/* a path the PC may name: relative, '/' separated, no empty, "." or ".." parts, nothing under sce_sys or sce_pfs */
static int ok_path(const char *p) {
	int n = len_(p);
	if (!n || n >= PATHLEN || p[0] == '/' || p[n - 1] == '/') return 0;
	const char *seg = p; char first[16]; int fi = 0;
	for (const char *c = p;; c++) {
		if (*c == '\\' || *c == ':') return 0;
		if (*c == '/' || !*c) {
			int sl = (int)(c - seg);
			if (!sl || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
			if (seg == p) { for (fi = 0; fi < sl && fi < 15; fi++) first[fi] = seg[fi]; first[fi] = 0; }
			if (!*c) break;
			seg = c + 1;
		}
	}
	return !lower_eq(first, "sce_sys") && !lower_eq(first, "sce_pfs");
}
static void mkdirs(const char *path) {   /* every parent folder of a file's path */
	char b[256]; cpy(b, path, sizeof b);
	for (char *c = b + 4; *c; c++) if (*c == '/') { *c = 0; sceIoMkdir(b, 0777); *c = '/'; }
}

/* ---- files ---- */
static int hash_file(const char *abs, uint8_t out[32], uint32_t *size) {
	SceUID f = sceIoOpen(abs, SCE_O_RDONLY, 0);
	if (f < 0) return f;
	NsyncHash h; nsync_hash_init(&h); uint32_t total = 0; int n;
	while ((n = sceIoRead(f, iobuf, sizeof iobuf)) > 0) { nsync_hash_update(&h, iobuf, (size_t)n); total += (uint32_t)n; }
	sceIoClose(f);
	nsync_hash_final(&h, out);
	if (size) *size = total;
	return n < 0 ? n : 0;
}
static int copy_file(const char *src, const char *dst) {
	SceUID a = sceIoOpen(src, SCE_O_RDONLY, 0);
	if (a < 0) return a;
	mkdirs(dst);
	SceUID b = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (b < 0) { sceIoClose(a); return b; }
	int n, r = 0;
	while ((n = sceIoRead(a, iobuf, sizeof iobuf)) > 0) if ((r = sceIoWrite(b, iobuf, n)) != n) { r = r < 0 ? r : -1; break; } else r = 0;
	if (n < 0) r = n;
	sceIoClose(a);
	int c = sceIoClose(b);
	return r < 0 ? r : c < 0 ? c : 0;
}
static void rm_tree(const char *abs) {   /* a folder and everything in it */
	SceUID d = sceIoDopen(abs);
	if (d < 0) { sceIoRemove(abs); return; }
	SceIoDirent e; char p[256];
	while (sceIoDread(d, &e) > 0) {
		if (!strcmp_(e.d_name, ".") || !strcmp_(e.d_name, "..")) continue;
		sceClibSnprintf(p, sizeof p, "%s/%s", abs, e.d_name);
		if (SCE_S_ISDIR(e.d_stat.st_mode)) rm_tree(p); else sceIoRemove(p);
	}
	sceIoDclose(d);
	sceIoRmdir(abs);
}

/* every file under root (but sce_sys and sce_pfs at the top), sorted by path in byte order; hashed if asked */
static char stack[12][PATHLEN];
static int walk(const char *root, Listing *L, int hash) {
	int sp = 0; stack[sp++][0] = 0; L->n = 0;
	char abs[256], rel[PATHLEN], r2[PATHLEN];
	while (sp) {
		cpy(rel, stack[--sp], sizeof rel);
		if (rel[0]) sceClibSnprintf(abs, sizeof abs, "%s/%s", root, rel); else cpy(abs, root, sizeof abs);
		SceUID d = sceIoDopen(abs);
		if (d < 0) return rel[0] ? d : 0;   /* (no folder at all: an empty listing) */
		SceIoDirent e;
		while (sceIoDread(d, &e) > 0) {
			if (!strcmp_(e.d_name, ".") || !strcmp_(e.d_name, "..")) continue;
			if (!rel[0] && (lower_eq(e.d_name, "sce_sys") || lower_eq(e.d_name, "sce_pfs"))) continue;
			if (rel[0]) sceClibSnprintf(r2, sizeof r2, "%s/%s", rel, e.d_name); else cpy(r2, e.d_name, sizeof r2);
			if (len_(r2) >= PATHLEN - 1) { sceIoDclose(d); return -2; }
			if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (sp >= 12) { sceIoDclose(d); return -3; } cpy(stack[sp++], r2, PATHLEN); continue; }
			if (L->n >= MAX_FILES) { sceIoDclose(d); return -4; }
			Entry *x = &L->e[L->n++]; cpy(x->path, r2, PATHLEN); x->size = (uint32_t)e.d_stat.st_size;
			if (hash) {
				sceClibSnprintf(abs, sizeof abs, "%s/%s", root, r2);
				int r = hash_file(abs, x->hash, &x->size);
				if (r < 0) { sceIoDclose(d); return r; }
				if (rel[0]) sceClibSnprintf(abs, sizeof abs, "%s/%s", root, rel); else cpy(abs, root, sizeof abs);
			}
		}
		sceIoDclose(d);
	}
	for (int i = 1; i < L->n; i++) for (int j = i; j > 0 && strcmp_(L->e[j - 1].path, L->e[j].path) > 0; j--) {   /* (few files) */
		Entry t = L->e[j]; L->e[j] = L->e[j - 1]; L->e[j - 1] = t;
	}
	return 0;
}

/* ---- slots: sce_sys/sdslot.dat ("SDSL", a used byte per slot at 0x200 + n, 0x400-byte records from 0x400) ---- */
static int slot_param(const char *id, int n, uint8_t out[SLOT_PARAM]) {
	char p[128]; save_path(p, sizeof p, id, SLOT_FILE);
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0);
	if (f < 0) return f;
	sceIoLseek(f, 0x400 + n * 0x400, SCE_SEEK_SET);
	int r = sceIoRead(f, out, SLOT_PARAM);
	sceIoClose(f);
	return r == SLOT_PARAM ? 0 : -1;
}
static int read_slots(const char *id, Listing *L) {
	char p[128]; save_path(p, sizeof p, id, SLOT_FILE); L->nslots = 0;
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0);
	if (f < 0) return 0;   /* a save without slots */
	uint8_t used[SLOTS];
	sceIoLseek(f, 0x200, SCE_SEEK_SET);
	int r = sceIoRead(f, used, SLOTS);
	for (int n = 0; r == SLOTS && n < SLOTS; n++) if (used[n]) {
		sceIoLseek(f, 0x400 + n * 0x400, SCE_SEEK_SET);
		if (sceIoRead(f, param, SLOT_PARAM) != SLOT_PARAM) { r = -1; break; }
		L->s[L->nslots].slot = n; crypto_blake2b(L->s[L->nslots].hash, 32, param, SLOT_PARAM); L->nslots++;
	}
	sceIoClose(f);
	return r == SLOTS ? 0 : -1;
}
static int write_slot(const char *id, int n, const uint8_t *p, int used) {   /* p NULL: only the used byte */
	char path[128]; save_path(path, sizeof path, id, SLOT_FILE);
	SceUID f = sceIoOpen(path, SCE_O_WRONLY, 0);
	if (f < 0) return f;
	int r = 0;
	if (p) { sceIoLseek(f, 0x400 + n * 0x400, SCE_SEEK_SET); if (sceIoWrite(f, p, SLOT_PARAM) != SLOT_PARAM) r = -1; }
	uint8_t u = (uint8_t)(used != 0);
	sceIoLseek(f, 0x200 + n, SCE_SEEK_SET); if (sceIoWrite(f, &u, 1) != 1) r = -1;
	int c = sceIoClose(f);
	return r < 0 ? r : c;
}

static void manifest_hash(const Listing *L, uint8_t out[32]) {
	NsyncHash h; nsync_hash_init(&h);
	for (int i = 0; i < L->n; i++) nsync_manifest_file(&h, L->e[i].path, L->e[i].size, L->e[i].hash);
	for (int i = 0; i < L->nslots; i++) nsync_manifest_slot(&h, L->s[i].slot, L->s[i].hash);
	nsync_hash_final(&h, out);
}
static int current(const char *id, Listing *L) {   /* (mounted) */
	char root[64]; save_path(root, sizeof root, id, 0);
	int r = walk(root, L, 1);
	return r < 0 ? r : read_slots(id, L);
}

/* ---- MANIFEST ---- */
static void jtext(char *out, int cap, int *len, const uint8_t *s, int max) {   /* a slot text field: cut on a UTF-8 boundary */
	char b[200]; int n = 0;
	while (n < max && n < (int)sizeof b - 1 && s[n]) n++;
	if (n == (int)sizeof b - 1 || n == max) while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
	sceClibMemcpy(b, s, n); b[n] = 0;
	svc_jstr(out, cap, len, b);
}
int save_manifest(const char *id, char *out, int cap) {
	int r = mount_save(id);
	if (r < 0) return r;
	r = current(id, &have);
	if (r < 0) { unmount_save(); return r; }
	uint8_t h[32]; char hex[65]; int len = 0;
	manifest_hash(&have, h); nsync_hex(hex, h, 32);
	svc_cat(out, cap, &len, "{\"hash\":\"%s\",\"files\":[", hex);
	for (int i = 0; i < have.n; i++) {
		nsync_hex(hex, have.e[i].hash, 32);
		svc_cat(out, cap, &len, "%s{\"path\":", i ? "," : ""); svc_jstr(out, cap, &len, have.e[i].path);
		svc_cat(out, cap, &len, ",\"size\":%u,\"hash\":\"%s\"}", have.e[i].size, hex);
	}
	svc_cat(out, cap, &len, "],\"slots\":[");
	for (int i = 0; i < have.nslots; i++) {
		int n = have.s[i].slot;
		if (slot_param(id, n, param) < 0) sceClibMemset(param, 0, sizeof param);
		nsync_hex(hex, have.s[i].hash, 32);
		const uint8_t *t = param + 0x30C;   /* modifiedTime: year, month, day, hour, minute, second (u16 each) */
		svc_cat(out, cap, &len, "%s{\"slot\":%d,\"hash\":\"%s\",\"title\":", i ? "," : "", n, hex);
		jtext(out, cap, &len, param + 0x4, 64);
		svc_cat(out, cap, &len, ",\"subtitle\":"); jtext(out, cap, &len, param + 0x44, 128);
		svc_cat(out, cap, &len, ",\"detail\":"); jtext(out, cap, &len, param + 0xC4, 160);
		svc_cat(out, cap, &len, ",\"time\":\"%04d-%02d-%02d %02d:%02d\"}", t[0] | t[1] << 8, t[2] | t[3] << 8, t[4] | t[5] << 8, t[6] | t[7] << 8, t[8] | t[9] << 8);
	}
	svc_cat(out, cap, &len, "]}");
	unmount_save();
	return len < cap - 1 ? len : -5;
}

/* ---- GET / PUT ---- */
static int slot_number(const char *path) {   /* "slot/<n>" -> n, else -1 */
	if (sceClibStrncmp(path, "slot/", 5) || !path[5]) return -1;
	int n = 0; for (const char *c = path + 5; *c; c++) { if (*c < '0' || *c > '9') return -1; n = n * 10 + (*c - '0'); }
	return n < SLOTS ? n : -1;
}
int save_get(const char *id, const char *path, uint32_t off, uint32_t len, uint8_t *out) {
	int slot = slot_number(path);
	if (slot < 0 && !ok_path(path)) return -1;
	int r = mount_save(id);
	if (r < 0) return r;
	if (slot >= 0) {
		r = slot_param(id, slot, param);
		if (r >= 0) { if (off > SLOT_PARAM) off = SLOT_PARAM; r = (int)(SLOT_PARAM - off < len ? SLOT_PARAM - off : len); sceClibMemcpy(out, param + off, r); }
	} else {
		char abs[256]; save_path(abs, sizeof abs, id, path);
		SceUID f = sceIoOpen(abs, SCE_O_RDONLY, 0);
		if (f < 0) r = f;
		else { sceIoLseek(f, off, SCE_SEEK_SET); r = sceIoRead(f, out, len); sceIoClose(f); }
	}
	unmount_save();
	return r;
}
static void stage_path(char *out, int cap, const char *id, const char *path) {
	int slot = slot_number(path);
	if (slot >= 0) sceClibSnprintf(out, cap, DATA "/stage/%s/s/%d", id, slot); else sceClibSnprintf(out, cap, DATA "/stage/%s/f/%s", id, path);
}
int save_put(const char *id, const char *path, uint32_t off, uint32_t total, const uint8_t *data, uint32_t n) {
	int slot = slot_number(path);
	if ((slot < 0 && !ok_path(path)) || off + n > total || (slot >= 0 && total != SLOT_PARAM)) return -1;
	char p[256]; stage_path(p, sizeof p, id, path);
	mkdirs(p);
	SceUID f = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | (off ? 0 : SCE_O_TRUNC), 0666);
	if (f < 0) return f;
	sceIoLseek(f, off, SCE_SEEK_SET);
	int w = n ? sceIoWrite(f, data, n) : 0, c = sceIoClose(f);
	return w != (int)n ? -1 : c;
}

/* ---- backups ---- */
static void stamp(char *out, int cap) {
	SceDateTime t; sceRtcGetCurrentClockLocalTime(&t);
	sceClibSnprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
}
static int backup_to(const char *id, const char *dir) {   /* (mounted) the current files and used slots, plain */
	char root[64], src[256], dst[256]; save_path(root, sizeof root, id, 0);
	int r = walk(root, &have, 0);
	for (int i = 0; r >= 0 && i < have.n; i++) {
		sceClibSnprintf(src, sizeof src, "%s/%s", root, have.e[i].path); sceClibSnprintf(dst, sizeof dst, "%s/f/%s", dir, have.e[i].path);
		r = copy_file(src, dst);
	}
	if (r >= 0) r = read_slots(id, &have);
	for (int i = 0; r >= 0 && i < have.nslots; i++) {
		if ((r = slot_param(id, have.s[i].slot, param)) < 0) break;
		sceClibSnprintf(dst, sizeof dst, "%s/s/%d", dir, have.s[i].slot); mkdirs(dst);
		SceUID f = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (f < 0) { r = f; break; }
		if (sceIoWrite(f, param, SLOT_PARAM) != SLOT_PARAM) r = -1;
		sceIoClose(f);
	}
	return r;
}
static int read_param_file(const char *p) {   /* into param */
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0);
	if (f < 0) return f;
	int r = sceIoRead(f, param, SLOT_PARAM); sceIoClose(f);
	return r == SLOT_PARAM ? 0 : -1;
}
static int restore_from(const char *id, const char *dir) {   /* (mounted) the save becomes exactly the backup */
	char root[64], src[256], dst[256], fdir[200]; save_path(root, sizeof root, id, 0);
	int r = walk(root, &have, 0);
	for (int i = 0; r >= 0 && i < have.n; i++) { sceClibSnprintf(dst, sizeof dst, "%s/%s", root, have.e[i].path); sceIoRemove(dst); }
	sceClibSnprintf(fdir, sizeof fdir, "%s/f", dir);
	if (r >= 0) r = walk(fdir, &want, 0);
	for (int i = 0; r >= 0 && i < want.n; i++) {
		sceClibSnprintf(src, sizeof src, "%s/%s", fdir, want.e[i].path); sceClibSnprintf(dst, sizeof dst, "%s/%s", root, want.e[i].path);
		r = copy_file(src, dst);
	}
	if (r >= 0) r = read_slots(id, &have);
	for (int n = 0; r >= 0 && n < SLOTS; n++) {
		sceClibSnprintf(src, sizeof src, "%s/s/%d", dir, n);
		int in_backup = read_param_file(src) == 0, used = 0;
		for (int i = 0; i < have.nslots; i++) if (have.s[i].slot == n) used = 1;
		if (in_backup) r = write_slot(id, n, param, 1);
		else if (used) r = write_slot(id, n, 0, 0);
	}
	return r;
}
static void prune(const char *id) {   /* the newest KEEP_BACKUPS backups of a save stay */
	char dir[128], names[24][20], p[200]; int n = 0;
	sceClibSnprintf(dir, sizeof dir, DATA "/backups/%s", id);
	for (;;) {
		SceUID d = sceIoDopen(dir); if (d < 0) return;
		SceIoDirent e; n = 0; char oldest[20] = "";
		while (sceIoDread(d, &e) > 0) {
			if (e.d_name[0] == '.' || !SCE_S_ISDIR(e.d_stat.st_mode)) continue;
			if (n < 24) cpy(names[n], e.d_name, 20);
			n++;
			if (!oldest[0] || strcmp_(e.d_name, oldest) < 0) cpy(oldest, e.d_name, sizeof oldest);
		}
		sceIoDclose(d);
		if (n <= KEEP_BACKUPS || !oldest[0]) return;
		sceClibSnprintf(p, sizeof p, "%s/%s", dir, oldest); rm_tree(p);
	}
}

/* ---- COMMIT ---- */
static int parse_hex32(const char *s, uint8_t out[32]) { return nsync_unhex(out, 32, s); }
static int parse_manifest(const char *text, int len, Listing *L) {   /* the canonical text: F and S lines */
	L->n = L->nslots = 0;
	const char *p = text, *end = text + len;
	while (p < end) {
		const char *nl = p; while (nl < end && *nl != '\n') nl++;
		if (nl == end) return -1;   /* (every line ends in a newline) */
		if (p[0] == 'F' && p[1] == ' ') {
			const char *t1 = p + 2; while (t1 < nl && *t1 != '\t') t1++;
			const char *t2 = t1 + 1; while (t2 < nl && *t2 != '\t') t2++;
			if (t2 >= nl || L->n >= MAX_FILES || t1 - (p + 2) >= PATHLEN) return -1;
			Entry *x = &L->e[L->n++];
			int pl = (int)(t1 - (p + 2)); sceClibMemcpy(x->path, p + 2, pl); x->path[pl] = 0;
			if (!ok_path(x->path)) return -1;
			uint32_t sz = 0; for (const char *c = t1 + 1; c < t2; c++) { if (*c < '0' || *c > '9') return -1; sz = sz * 10 + (uint32_t)(*c - '0'); }
			x->size = sz;
			if (nl - (t2 + 1) != 64 || parse_hex32(t2 + 1, x->hash)) return -1;
		} else if (p[0] == 'S' && p[1] == ' ') {
			const char *t1 = p + 2; int n = 0; while (t1 < nl && *t1 >= '0' && *t1 <= '9') n = n * 10 + (*t1++ - '0');
			if (*t1 != '\t' || n >= SLOTS || L->nslots >= SLOTS || nl - (t1 + 1) != 64 || parse_hex32(t1 + 1, L->s[L->nslots].hash)) return -1;
			L->s[L->nslots++].slot = n;
		} else return -1;
		p = nl + 1;
	}
	return 0;
}
int save_commit(const char *id, const char *pc_id, const char *hash_hex, const char *text, int text_len, char *out,
                int cap, char *why, int why_cap) {
	#define FAIL(...) do { sceClibSnprintf(why, why_cap, __VA_ARGS__); goto done; } while (0)
	uint8_t target[32], got[32]; char stage[128], src[256], dst[256], root[64], bdir[128], name[20];
	int mounted = 0, applied = 0, r;
	why[0] = 0;
	if (parse_hex32(hash_hex, target)) FAIL("bad hash");
	crypto_blake2b(got, 32, (const uint8_t *)text, (size_t)text_len);
	if (crypto_verify32(got, target)) FAIL("the manifest doesn't match its hash");
	if (parse_manifest(text, text_len, &want)) FAIL("the manifest doesn't read");
	sceClibSnprintf(stage, sizeof stage, DATA "/stage/%s", id);
	if ((r = mount_save(id)) < 0) FAIL("mount 0x%08X", r);
	mounted = 1;
	save_path(root, sizeof root, id, 0);
	if ((r = current(id, &have)) < 0) FAIL("reading the save 0x%08X", r);
	for (int i = 0; i < want.n; i++) {   /* every file staged as named, or already here as named */
		uint8_t h[32]; uint32_t sz;
		sceClibSnprintf(src, sizeof src, "%s/f/%s", stage, want.e[i].path);
		if (hash_file(src, h, &sz) >= 0) { if (sz != want.e[i].size || crypto_verify32(h, want.e[i].hash)) FAIL("staged %s doesn't match", want.e[i].path); continue; }
		int here = 0;
		for (int j = 0; j < have.n; j++) if (!strcmp_(have.e[j].path, want.e[i].path) && have.e[j].size == want.e[i].size && !crypto_verify32(have.e[j].hash, want.e[i].hash)) here = 1;
		if (!here) FAIL("%s is neither staged nor here", want.e[i].path);
	}
	for (int i = 0; i < want.nslots; i++) {
		uint8_t h[32];
		sceClibSnprintf(src, sizeof src, "%s/s/%d", stage, want.s[i].slot);
		if (read_param_file(src) == 0) { crypto_blake2b(h, 32, param, SLOT_PARAM); if (crypto_verify32(h, want.s[i].hash)) FAIL("staged slot %d doesn't match", want.s[i].slot); continue; }
		int here = 0;
		for (int j = 0; j < have.nslots; j++) if (have.s[j].slot == want.s[i].slot && !crypto_verify32(have.s[j].hash, want.s[i].hash)) here = 1;
		if (!here) FAIL("slot %d is neither staged nor here", want.s[i].slot);
	}
	if (want.nslots) { save_path(dst, sizeof dst, id, SLOT_FILE); SceIoStat st; if (sceIoGetstat(dst, &st) < 0) FAIL("this save has no slot file"); }

	stamp(name, sizeof name);
	sceClibSnprintf(bdir, sizeof bdir, DATA "/backups/%s/%s", id, name);
	if ((r = backup_to(id, bdir)) < 0) FAIL("backing up 0x%08X", r);
	applied = 1;
	for (int i = 0; i < want.n; i++) {   /* staged files in */
		sceClibSnprintf(src, sizeof src, "%s/f/%s", stage, want.e[i].path); sceClibSnprintf(dst, sizeof dst, "%s/%s", root, want.e[i].path);
		SceIoStat st; if (sceIoGetstat(src, &st) < 0) continue;
		if ((r = copy_file(src, dst)) < 0) FAIL("writing %s 0x%08X", want.e[i].path, r);
	}
	if ((r = walk(root, &have, 0)) < 0) FAIL("listing 0x%08X", r);
	for (int j = 0; j < have.n; j++) {   /* files not in the manifest out */
		int keep = 0; for (int i = 0; i < want.n; i++) if (!strcmp_(have.e[j].path, want.e[i].path)) keep = 1;
		if (!keep) { sceClibSnprintf(dst, sizeof dst, "%s/%s", root, have.e[j].path); sceIoRemove(dst); }
	}
	if ((r = read_slots(id, &have)) < 0) FAIL("reading the slots 0x%08X", r);
	for (int n = 0; n < SLOTS; n++) {
		int wanted = 0, used = 0;
		for (int i = 0; i < want.nslots; i++) if (want.s[i].slot == n) wanted = 1;
		for (int j = 0; j < have.nslots; j++) if (have.s[j].slot == n) used = 1;
		sceClibSnprintf(src, sizeof src, "%s/s/%d", stage, n);
		if (wanted && read_param_file(src) == 0) { if ((r = write_slot(id, n, param, 1)) < 0) FAIL("writing slot %d 0x%08X", n, r); }
		else if (!wanted && used) { if ((r = write_slot(id, n, 0, 0)) < 0) FAIL("clearing slot %d 0x%08X", n, r); }
	}
	if ((r = current(id, &have)) < 0) FAIL("reading back 0x%08X", r);
	manifest_hash(&have, got);
	if (crypto_verify32(got, target)) FAIL("the save didn't read back as sent");
	applied = 2;
done:
	if (applied == 1) {   /* a failure after the backup: put it back */
		int rr = restore_from(id, bdir);
		svc_log("commit %s failed (%s): restored the backup: %s", id, why, rr < 0 ? "FAILED" : "ok");
		if (rr < 0) { int l = len_(why); sceClibSnprintf(why + l, why_cap - l, "; RESTORING THE BACKUP FAILED (it's in %s)", bdir); }
	}
	if (mounted) unmount_save();
	if (applied == 2) {
		rm_tree(stage);
		save_set_base(pc_id, id, hash_hex);
		prune(id);
		int len = 0; svc_cat(out, cap, &len, "{\"backup\":\"%s\"}", name);
		svc_log("commit %s: %d files, %d slots, backup %s", id, want.n, want.nslots, name);
		return len;
	}
	return -1;
	#undef FAIL
}

int save_set_base(const char *pc_id, const char *id, const char *hash_hex) {
	char p[128]; sceClibSnprintf(p, sizeof p, DATA "/state/%s/%s.base", pc_id, id);
	mkdirs(p);
	SceUID f = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (f < 0) return f;
	sceIoWrite(f, hash_hex, 64);
	return sceIoClose(f);
}
static int read_base(const char *pc_id, const char *id, char hex[65]) {
	char p[128]; sceClibSnprintf(p, sizeof p, DATA "/state/%s/%s.base", pc_id, id);
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0);
	if (f < 0) return f;
	int n = sceIoRead(f, hex, 64); sceIoClose(f); hex[n == 64 ? 64 : 0] = 0;
	return n == 64 ? 0 : -1;
}

/* ---- BACKUPS / RESTORE ---- */
int save_backups(const char *id, char *out, int cap) {
	char dir[128]; int len = 0, first = 1;
	sceClibSnprintf(dir, sizeof dir, DATA "/backups/%s", id);
	svc_cat(out, cap, &len, "{\"backups\":[");
	SceUID d = sceIoDopen(dir);
	if (d >= 0) {
		SceIoDirent e;
		while (sceIoDread(d, &e) > 0) if (e.d_name[0] != '.' && SCE_S_ISDIR(e.d_stat.st_mode)) {
			svc_cat(out, cap, &len, first ? "" : ","); svc_jstr(out, cap, &len, e.d_name); first = 0;
		}
		sceIoDclose(d);
	}
	svc_cat(out, cap, &len, "]}");
	return len;
}
int save_restore(const char *id, const char *name, char *why, int why_cap) {
	char bdir[128], now[128], stampname[20];
	why[0] = 0;
	for (const char *c = name; *c; c++) if (!((*c >= '0' && *c <= '9') || *c == '-')) { sceClibSnprintf(why, why_cap, "no such backup"); return -1; }
	sceClibSnprintf(bdir, sizeof bdir, DATA "/backups/%s/%s", id, name);
	SceIoStat st; if (sceIoGetstat(bdir, &st) < 0) { sceClibSnprintf(why, why_cap, "no such backup"); return -1; }
	int r = mount_save(id);
	if (r < 0) { sceClibSnprintf(why, why_cap, "mount 0x%08X", r); return r; }
	stamp(stampname, sizeof stampname);
	sceClibSnprintf(now, sizeof now, DATA "/backups/%s/%s", id, stampname);
	if (strcmp_(now, bdir) && (r = backup_to(id, now)) < 0) sceClibSnprintf(why, why_cap, "backing up first 0x%08X", r);
	else if ((r = restore_from(id, bdir)) < 0) sceClibSnprintf(why, why_cap, "restoring 0x%08X", r);
	unmount_save();
	svc_log("restore %s from %s: %s", id, name, why[0] ? why : "ok");
	return r;
}

/* ---- LIST ---- */
static unsigned u32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
int sfo_read(const char *path, char *title, int title_cap, char *version, int version_cap) {
	static uint8_t b[8192];
	SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (f < 0) return f;
	int len = sceIoRead(f, b, sizeof b); sceIoClose(f);
	if (len < 20 || u32le(b) != 0x46535000) return -1;
	unsigned keys = u32le(b + 8), data = u32le(b + 12), count = u32le(b + 16);
	for (unsigned i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)len; i++) {
		const uint8_t *e = b + 20 + i * 16;
		unsigned ko = e[0] | e[1] << 8, dl = u32le(e + 4), dof = u32le(e + 12);
		if (keys + ko >= (unsigned)len || data + dof + dl > (unsigned)len) continue;
		const char *k = (const char *)b + keys + ko;
		char *dst = !strcmp_(k, "TITLE") ? title : !strcmp_(k, "APP_VER") ? version : 0;
		int cap = dst == title ? title_cap : version_cap;
		if (!dst || !cap) continue;
		int n = 0; for (; n < cap - 1 && n < (int)dl && b[data + dof + n]; n++) dst[n] = (char)b[data + dof + n];
		dst[n] = 0;
	}
	return 0;
}
static char ids[300][12]; static int nids;
static void add_ids(const char *dir) {
	SceUID d = sceIoDopen(dir); if (d < 0) return;
	SceIoDirent e;
	while (sceIoDread(d, &e) > 0 && nids < 300) {
		if (!SCE_S_ISDIR(e.d_stat.st_mode) || !svc_valid_id(e.d_name)) continue;
		int dup = 0; for (int i = 0; i < nids; i++) if (!strcmp_(ids[i], e.d_name)) dup = 1;
		if (!dup) cpy(ids[nids++], e.d_name, 12);
	}
	sceIoDclose(d);
}
int saves_list(char *out, int cap, const char *pc_id) {
	nids = 0; add_ids(SAVES); add_ids("ux0:app");
	int len = 0;
	svc_cat(out, cap, &len, "{\"games\":[");
	for (int i = 0; i < nids; i++) {
		char p[96], name[128] = "", ver[16] = "", dummy[2], base[65];
		sceClibSnprintf(p, sizeof p, "ux0:app/%s/sce_sys/param.sfo", ids[i]); sfo_read(p, name, sizeof name, ver, sizeof ver);
		sceClibSnprintf(p, sizeof p, "ux0:patch/%s/sce_sys/param.sfo", ids[i]); sfo_read(p, dummy, 0, ver, sizeof ver);
		sceClibSnprintf(p, sizeof p, SAVES "/%s", ids[i]); SceIoStat st; int has = sceIoGetstat(p, &st) >= 0;
		svc_cat(out, cap, &len, "%s{\"id\":\"%s\",\"name\":", i ? "," : "", ids[i]); svc_jstr(out, cap, &len, name);
		svc_cat(out, cap, &len, ",\"version\":"); svc_jstr(out, cap, &len, ver);
		svc_cat(out, cap, &len, ",\"save\":%s,\"base\":", has ? "true" : "false");
		if (read_base(pc_id, ids[i], base) == 0) svc_cat(out, cap, &len, "\"%s\"", base); else svc_cat(out, cap, &len, "null");
		svc_cat(out, cap, &len, ",\"guard\":\"%s\"}", svc_guard_name(ids[i]));
	}
	svc_cat(out, cap, &len, "]}");
	return len;
}
