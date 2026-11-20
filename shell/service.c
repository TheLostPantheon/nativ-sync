/* service.c: Nativ Sync's background service, a plugin in SceShell (PROTOCOL.md, DESIGN.md).
 *
 * Threads: the listener (TCP 1340: pairing and sessions, a thread per connection), discovery (UDP 1340), and the
 * watcher (the kernel module's launch events, and which games are running). Requests run one at a time under a lock;
 * EVENTS waits outside it. The shell brings the network up itself, and drops our sockets when the Vita sleeps: both
 * listeners wait for it and start again. Light by design: idle, the threads sleep in accept, recvfrom and a 1-second
 * wait for kernel events. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/appmgr.h>
#include <psp2/registrymgr.h>
#include <stdarg.h>
#include "svc.h"

#define VERSION "0.1"
#define MAX_CONN 3
#define PAIR_TRIES 3

void *memset(void *d, int c, unsigned int n) { return sceClibMemset(d, c, n); }   /* (no libc: the compiler's calls land here) */
void *memcpy(void *d, const void *s, unsigned int n) { return sceClibMemcpy(d, s, n); }
void *memmove(void *d, const void *s, unsigned int n) { return sceClibMemmove(d, s, n); }

static volatile int running = 1;
static SceUID cmd_lock = -1, log_lock = -1, ev_lock = -1;
static char console_id[17], console_name[64];

/* ---- small things ---- */
static int len_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void cpy(char *d, const char *s, int cap) { int i = 0; for (; i < cap - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
void svc_log(const char *fmt, ...) {
	char line[300]; va_list ap; va_start(ap, fmt);
	int n = sceClibVsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof line - 2) n = sizeof line - 2;
	line[n++] = '\n';
	sceKernelLockMutex(log_lock, 1, NULL);
	SceIoStat st;
	int big = sceIoGetstat(DATA "/service.log", &st) >= 0 && st.st_size > 128 * 1024;
	SceUID f = sceIoOpen(DATA "/service.log", SCE_O_WRONLY | SCE_O_CREAT | (big ? SCE_O_TRUNC : SCE_O_APPEND), 0666);
	if (f >= 0) { sceIoWrite(f, line, n); sceIoClose(f); }
	sceKernelUnlockMutex(log_lock, 1);
}
void svc_cat(char *out, int cap, int *len, const char *fmt, ...) {
	if (*len >= cap - 1) return;
	va_list ap; va_start(ap, fmt);
	int n = sceClibVsnprintf(out + *len, cap - *len, fmt, ap); va_end(ap);
	if (n > 0) *len = *len + n < cap - 1 ? *len + n : cap - 1;
}
void svc_jstr(char *out, int cap, int *len, const char *s) {
	svc_cat(out, cap, len, "\"");
	for (const unsigned char *c = (const unsigned char *)s; *c && *len < cap - 8; c++) {
		if (*c == '"' || *c == '\\') { out[(*len)++] = '\\'; out[(*len)++] = (char)*c; }
		else if (*c < 0x20) svc_cat(out, cap, len, "\\u%04x", *c);
		else out[(*len)++] = (char)*c;
	}
	out[*len] = 0;
	svc_cat(out, cap, len, "\"");
}
int svc_valid_id(const char *t) {
	for (int i = 0; i < 9; i++) if (i < 4 ? !(t[i] >= 'A' && t[i] <= 'Z') : !(t[i] >= '0' && t[i] <= '9')) return 0;
	return t[9] == 0;
}
static int read_small(const char *p, char *b, int cap) {
	SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); if (f < 0) return f;
	int n = sceIoRead(f, b, cap - 1); sceIoClose(f); if (n < 0) return n;
	b[n] = 0; return n;
}
static int write_small(const char *p, const void *b, int n) {
	SceUID f = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (f < 0) return f;
	int w = sceIoWrite(f, b, n); sceIoClose(f); return w == n ? 0 : -1;
}
static void random_bytes(uint8_t *p, int n) {
	for (int i = 0; i < n; i += 64) sceKernelGetRandomNumber(p + i, n - i < 64 ? n - i : 64);
}

/* ---- the guards this service has set, by game (what LIST reports; restored after a sync guard) ---- */
#define MAX_GUARDS 32
static NsyncGuardInfo guards[MAX_GUARDS];
static NsyncGuardInfo *guard_of(const char *id) { for (int i = 0; i < MAX_GUARDS; i++) if (guards[i].state && !sceClibStrcmp(guards[i].titleid, id)) return &guards[i]; return 0; }
const char *svc_guard_name(const char *id) { NsyncGuardInfo *g = guard_of(id); return !g ? "none" : g->state == NSYNC_GUARD_CONFLICT ? "conflict" : "sync"; }
static void guard_store(const NsyncGuardInfo *g) {
	NsyncGuardInfo *s = guard_of(g->titleid);
	if (!s && g->state) for (int i = 0; i < MAX_GUARDS && !s; i++) if (!guards[i].state) s = &guards[i];
	if (s) *s = *g;
	nsyncGuardSet(g);
}

/* ---- events for the PCs (a ring; each connection remembers the last it saw) ---- */
typedef struct { int seq; char kind[10], id[12], choice[8]; } Event;
static Event ring[64]; static volatile int ev_seq;
static char held_for_pc[12];   /* a launch held because the player chose the PC's save: the PC may write now */
static void event(const char *kind, const char *id, const char *choice) {
	sceKernelLockMutex(ev_lock, 1, NULL);
	Event *e = &ring[ev_seq % 64]; e->seq = ev_seq + 1;
	cpy(e->kind, kind, sizeof e->kind); cpy(e->id, id, sizeof e->id); cpy(e->choice, choice ? choice : "", sizeof e->choice);
	ev_seq++;
	sceKernelUnlockMutex(ev_lock, 1);
	svc_log("event %s %s %s", kind, id, choice ? choice : "");
}

/* ---- which games run ---- */
static char run_now[20][12]; static int nrun;
static int is_running(const char *id) { for (int i = 0; i < nrun; i++) if (!sceClibStrcmp(run_now[i], id)) return 1; return 0; }
static void poll_running(void) {
	SceInt32 appids[20]; char now[20][12]; int n = 0;
	int k = sceAppMgrGetRunningAppIdListForShell(appids, 20);   /* (SceShell always asks for 20) */
	for (int i = 0; i < k && i < 20; i++) {
		SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(appids[i]); char t[16] = "";
		if (pid >= 0 && sceAppMgrGetNameById(pid, t) >= 0 && svc_valid_id(t)) cpy(now[n++], t, 12);
	}
	for (int i = 0; i < n; i++) if (!is_running(now[i])) event("started", now[i], 0);
	for (int i = 0; i < nrun; i++) { int still = 0; for (int j = 0; j < n; j++) if (!sceClibStrcmp(run_now[i], now[j])) still = 1; if (!still) event("stopped", run_now[i], 0); }
	sceKernelLockMutex(ev_lock, 1, NULL);
	for (int i = 0; i < n; i++) cpy(run_now[i], now[i], 12);
	nrun = n;
	sceKernelUnlockMutex(ev_lock, 1);
}

static int watcher(SceSize a, void *p) {   /* the kernel's launch events, and the running list once a second */
	nsyncGuardClear();
	while (running) {
		NsyncEvent e; sceClibMemset(&e, 0, sizeof e);
		int r = nsyncGuardWait(&e, 1000000);
		if (r == 0) {
			if (e.kind == NSYNC_EVENT_CHOICE) {
				static const char *const names[] = {"later", "vita", "pc", "later"};
				const char *c = names[e.choice >= 0 && e.choice <= 3 ? e.choice : 0];
				if (e.choice == NSYNC_CHOICE_PC) cpy(held_for_pc, e.titleid, sizeof held_for_pc);
				if (e.choice == NSYNC_CHOICE_VITA || e.choice == NSYNC_CHOICE_PC) { NsyncGuardInfo *g = guard_of(e.titleid); if (g) g->state = NSYNC_GUARD_NONE; }
				event("choice", e.titleid, c);
			} else if (e.kind == NSYNC_EVENT_LAUNCH) event("waiting", e.titleid, 0);
		} else if (r != (int)NSYNC_ERROR_TIMEOUT) sceKernelDelayThread(1000000);
		poll_running();
	}
	return sceKernelExitDeleteThread(0);
}

/* the launch guard around work on a save: a launch of that game waits until it's done */
static int op_begin(const char *id) {
	if (is_running(id) && sceClibStrcmp(held_for_pc, id)) return -1;
	NsyncGuardInfo g; NsyncGuardInfo *had = guard_of(id);
	if (had) g = *had; else { sceClibMemset(&g, 0, sizeof g); cpy(g.titleid, id, sizeof g.titleid); }
	g.state = NSYNC_GUARD_SYNC;
	nsyncGuardSet(&g);
	char t[12] = "";
	if (nsyncGuardLaunching(t) && !sceClibStrcmp(t, id)) { if (had) nsyncGuardSet(had); else { g.state = 0; nsyncGuardSet(&g); } return -1; }
	return 0;
}
static void op_end(const char *id, int ok, int final) {
	NsyncGuardInfo *had = guard_of(id);
	if (had) nsyncGuardSet(had);
	else { NsyncGuardInfo g; sceClibMemset(&g, 0, sizeof g); cpy(g.titleid, id, sizeof g.titleid); nsyncGuardSet(&g); }
	if (final || sceClibStrcmp(held_for_pc, id)) nsyncGuardRelease(id, ok);
	if (final && !sceClibStrcmp(held_for_pc, id)) held_for_pc[0] = 0;
}

/* ---- connections ---- */
static int send_all(int s, const void *b, int n) {
	for (int done = 0; done < n; ) { int w = sceNetSend(s, (const char *)b + done, n - done, 0); if (w <= 0) return -1; done += w; }
	return 0;
}
static int recv_all(int s, void *b, int n) {
	for (int done = 0; done < n; ) { int r = sceNetRecv(s, (char *)b + done, n - done, 0); if (r <= 0) return -1; done += r; }
	return 0;
}
static int recv_line(int s, char *b, int cap) {   /* (the clear-text lines before a session) */
	for (int n = 0; n < cap - 1; n++) {
		if (sceNetRecv(s, b + n, 1, 0) != 1) return -1;
		if (b[n] == '\n') { b[n] = 0; if (n && b[n - 1] == '\r') b[n - 1] = 0; return n; }
	}
	return -1;
}
static int send_line(int s, const char *t) { return send_all(s, t, len_(t)) < 0 || send_all(s, "\n", 1) < 0 ? -1 : 0; }

typedef struct {
	int fd, slot; NsyncSession ses; char pc_id[17], pc_name[64]; int last_seq;
	uint8_t *in, *out;   /* frame buffers: NSYNC_PLAIN_MAX + overhead each */
} Conn;
static Conn conns[MAX_CONN];
static uint8_t *plain_out;   /* (one request at a time, under cmd_lock) */

static int frame_send(Conn *c, const uint8_t *plain, int n) { size_t f = nsync_seal(&c->ses, c->out, plain, (size_t)n); return send_all(c->fd, c->out, (int)f); }
static int reply(Conn *c, const char *text) { return frame_send(c, (const uint8_t *)text, len_(text)); }
static int reply_err(Conn *c, const char *fmt, ...) {
	char b[300] = "ERR "; va_list ap; va_start(ap, fmt); sceClibVsnprintf(b + 4, sizeof b - 4, fmt, ap); va_end(ap);
	return reply(c, b);
}
static int reply_payload(Conn *c, const uint8_t *data, int n) {   /* "OK\n" + data, built in place in plain_out */
	if (data != plain_out + 3) sceClibMemmove(plain_out + 3, data, n);
	plain_out[0] = 'O'; plain_out[1] = 'K'; plain_out[2] = '\n';
	return frame_send(c, plain_out, n + 3);
}

static uint32_t num(const char **s) {   /* the next decimal argument */
	while (**s == ' ') (*s)++;
	uint32_t v = 0; while (**s >= '0' && **s <= '9') v = v * 10 + (uint32_t)(*(*s)++ - '0');
	return v;
}
static int arg(const char **s, char *out, int cap) {
	while (**s == ' ') (*s)++;
	int n = 0; while (**s && **s != ' ') { if (n < cap - 1) out[n++] = **s; (*s)++; }
	out[n] = 0; return n;
}

static int cmd_hello(Conn *c, const char *rest) {
	cpy(c->pc_name, rest, sizeof c->pc_name);
	char fw[8] = ""; nsyncFirmware(fw);
	uint8_t acc[8] = {0}; char acchex[17]; sceRegMgrGetKeyBin("/CONFIG/NP", "account_id", acc, 8); nsync_hex(acchex, acc, 8);
	int len = 0; char *o = (char *)plain_out + 3; int cap = NSYNC_PLAIN_MAX - 3;
	svc_cat(o, cap, &len, "{\"protocol\":1,\"service\":\"%s\",\"console\":", VERSION); svc_jstr(o, cap, &len, console_name);
	svc_cat(o, cap, &len, ",\"console_id\":\"%s\",\"firmware\":\"%s\",\"account_id\":\"%s\",\"running\":[", console_id, fw, acchex);
	for (int i = 0; i < nrun; i++) svc_cat(o, cap, &len, "%s\"%s\"", i ? "," : "", run_now[i]);
	svc_cat(o, cap, &len, "]}");
	c->last_seq = ev_seq;
	svc_log("hello from %s (%s)", c->pc_name, c->pc_id);
	return reply_payload(c, (uint8_t *)o, len);
}
static int cmd_guard(Conn *c, const char *id, const char *body) {   /* key<TAB>value lines */
	NsyncGuardInfo g; sceClibMemset(&g, 0, sizeof g); cpy(g.titleid, id, sizeof g.titleid); cpy(g.pc_name, c->pc_name, sizeof g.pc_name);
	for (const char *p = body; p && *p; ) {
		const char *nl = p; while (*nl && *nl != '\n') nl++;
		const char *tab = p; while (tab < nl && *tab != '\t') tab++;
		char key[16] = "", val[100] = "";
		int kl = (int)(tab - p) < 15 ? (int)(tab - p) : 15; sceClibMemcpy(key, p, kl); key[kl] = 0;
		if (tab < nl) { int vl = (int)(nl - tab - 1) < 99 ? (int)(nl - tab - 1) : 99; sceClibMemcpy(val, tab + 1, vl); val[vl] = 0; }
		if (!sceClibStrcmp(key, "state")) g.state = !sceClibStrcmp(val, "conflict") ? NSYNC_GUARD_CONFLICT : NSYNC_GUARD_NONE;
		else if (!sceClibStrcmp(key, "game")) cpy(g.game, val, sizeof g.game);
		else if (!sceClibStrcmp(key, "pc_when")) cpy(g.pc_when, val, sizeof g.pc_when);
		else if (!sceClibStrcmp(key, "pc_detail")) cpy(g.pc_detail, val, sizeof g.pc_detail);
		else if (!sceClibStrcmp(key, "vita_when")) cpy(g.vita_when, val, sizeof g.vita_when);
		else if (!sceClibStrcmp(key, "vita_detail")) cpy(g.vita_detail, val, sizeof g.vita_detail);
		p = *nl ? nl + 1 : nl;
	}
	if (!g.game[0]) { char p[96], v[8]; sceClibSnprintf(p, sizeof p, "ux0:app/%s/sce_sys/param.sfo", id); sfo_read(p, g.game, sizeof g.game, v, sizeof v); }
	guard_store(&g);
	return reply(c, "OK");
}
static int cmd_events(Conn *c, uint32_t ms) {   /* (outside cmd_lock) waits for events this connection hasn't seen */
	for (uint32_t waited = 0; c->last_seq >= ev_seq && waited < ms && running; waited += 100) sceKernelDelayThread(100000);
	char o[3072]; int len = 0, first = 1;   /* (EVENTS runs beside requests and other connections' EVENTS: buffers of its own) */
	sceKernelLockMutex(ev_lock, 1, NULL);
	svc_cat(o, sizeof o - 64, &len, "{\"events\":[");
	int from = c->last_seq > ev_seq - 64 ? c->last_seq : ev_seq - 64;
	for (int s = from; s < ev_seq; s++) {
		Event *e = &ring[s % 64];
		svc_cat(o, sizeof o - 64, &len, "%s{\"kind\":\"%s\",\"id\":\"%s\"", first ? "" : ",", e->kind, e->id);
		if (e->choice[0]) svc_cat(o, sizeof o - 64, &len, ",\"choice\":\"%s\"", e->choice);
		svc_cat(o, sizeof o - 64, &len, "}"); first = 0;
	}
	c->last_seq = ev_seq;
	sceKernelUnlockMutex(ev_lock, 1);
	svc_cat(o, sizeof o, &len, "]}");
	uint8_t b[3072 + 4];
	b[0] = 'O'; b[1] = 'K'; b[2] = '\n'; sceClibMemcpy(b + 3, o, len);
	return frame_send(c, b, len + 3);
}

static int handle(Conn *c, uint8_t *msg, int n) {
	msg[n] = 0;
	char *body = 0; for (int i = 0; i < n; i++) if (msg[i] == '\n') { msg[i] = 0; body = (char *)msg + i + 1; break; }
	int body_len = body ? n - (int)(body - (char *)msg) : 0;
	const char *s = (const char *)msg; char cmd[12], id[12], path[200], why[200];
	arg(&s, cmd, sizeof cmd);
	if (!sceClibStrcmp(cmd, "EVENTS")) return cmd_events(c, num(&s));
	sceKernelLockMutex(cmd_lock, 1, NULL);
	int r = 0;
	if (!sceClibStrcmp(cmd, "HELLO")) { while (*s == ' ') s++; r = cmd_hello(c, s); goto out; }
	if (!sceClibStrcmp(cmd, "LIST")) { char only[12]; arg(&s, only, sizeof only); int len = saves_list((char *)plain_out + 3, NSYNC_PLAIN_MAX - 3, c->pc_id, only); r = reply_payload(c, plain_out + 3, len); goto out; }
	arg(&s, id, sizeof id);
	if (!svc_valid_id(id)) { r = reply_err(c, "not a title id"); goto out; }
	if (!sceClibStrcmp(cmd, "MANIFEST")) {
		if (op_begin(id) < 0) { r = reply_err(c, "busy"); goto out; }
		int len = save_manifest(id, (char *)plain_out + 3, NSYNC_PLAIN_MAX - 3);
		op_end(id, 1, 0);
		r = len < 0 ? reply_err(c, "manifest 0x%08X", len) : reply_payload(c, plain_out + 3, len);
	} else if (!sceClibStrcmp(cmd, "GET")) {
		arg(&s, path, sizeof path); uint32_t off = num(&s), len = num(&s);
		if (len > NSYNC_PLAIN_MAX - 3) len = NSYNC_PLAIN_MAX - 3;
		if (op_begin(id) < 0) { r = reply_err(c, "busy"); goto out; }
		int got = save_get(id, path, off, len, plain_out + 3);
		op_end(id, 1, 0);
		r = got < 0 ? reply_err(c, "get 0x%08X", got) : reply_payload(c, plain_out + 3, got);
	} else if (!sceClibStrcmp(cmd, "PUT")) {
		arg(&s, path, sizeof path); uint32_t off = num(&s), total = num(&s);
		int w = save_put(id, path, off, total, (const uint8_t *)body, body ? (uint32_t)body_len : 0);
		r = w < 0 ? reply_err(c, "put 0x%08X", w) : reply(c, "OK");
	} else if (!sceClibStrcmp(cmd, "COMMIT")) {
		char hash[80]; arg(&s, hash, sizeof hash);
		if (op_begin(id) < 0) { r = reply_err(c, "busy"); goto out; }
		int len = save_commit(id, c->pc_id, hash, body ? body : "", body_len, (char *)plain_out + 3, NSYNC_PLAIN_MAX - 3, why, sizeof why);
		op_end(id, len >= 0, 1);
		r = len < 0 ? reply_err(c, "%s", why) : reply_payload(c, plain_out + 3, len);
	} else if (!sceClibStrcmp(cmd, "BASE")) {
		char hash[80]; arg(&s, hash, sizeof hash); uint8_t h[32];
		r = nsync_unhex(h, 32, hash) ? reply_err(c, "bad hash") : save_set_base(c->pc_id, id, hash) < 0 ? reply_err(c, "can't write") : reply(c, "OK");
	} else if (!sceClibStrcmp(cmd, "GUARD")) r = cmd_guard(c, id, body);
	else if (!sceClibStrcmp(cmd, "RELEASE")) {
		int ok = (int)num(&s);
		nsyncGuardRelease(id, ok);
		if (!sceClibStrcmp(held_for_pc, id)) held_for_pc[0] = 0;
		r = reply(c, "OK");
	} else if (!sceClibStrcmp(cmd, "BACKUPS")) { int len = save_backups(id, (char *)plain_out + 3, NSYNC_PLAIN_MAX - 3); r = reply_payload(c, plain_out + 3, len); }
	else if (!sceClibStrcmp(cmd, "RESTORE")) {
		char name[32]; arg(&s, name, sizeof name);
		if (op_begin(id) < 0) { r = reply_err(c, "busy"); goto out; }
		int rr = save_restore(id, name, why, sizeof why);
		op_end(id, rr >= 0, 0);
		r = rr < 0 ? reply_err(c, "%s", why) : reply(c, "OK");
	} else r = reply_err(c, "unknown request");
out:
	sceKernelUnlockMutex(cmd_lock, 1);
	return r;
}

/* ---- pairing ---- */
static int pair_tries;
static int do_pair(int fd, const char *rest) {   /* "<pc id> <pc nonce> <pc name>" */
	char pc_id[17], nonce_hex[40], code[16], line[200], hex[65];
	const char *s = rest; arg(&s, pc_id, sizeof pc_id); arg(&s, nonce_hex, sizeof nonce_hex); while (*s == ' ') s++;
	uint8_t pcn[16], vn[16], rnd[32], other[32], pk[32], cf[32], tr[96], tag[32];
	if (len_(pc_id) != 16 || nsync_unhex(pcn, 16, nonce_hex) || nsync_unhex(pk, 8, pc_id)) return send_line(fd, "NSYNC1 ERR bad request");
	int n = read_small(DATA "/pairing.txt", code, sizeof code);
	if (n < 6) return send_line(fd, "NSYNC1 ERR pairing isn't open on the Vita");
	if (pair_tries >= PAIR_TRIES) return send_line(fd, "NSYNC1 ERR too many tries: open pairing again");
	random_bytes(vn, 16); random_bytes(rnd, 32);
	nsync_hex(hex, vn, 16); sceClibSnprintf(line, sizeof line, "NSYNC1 PAIR %s", hex);
	if (send_line(fd, line) < 0 || recv_line(fd, line, sizeof line) < 0 || nsync_unhex(other, 32, line)) return -1;
	NsyncPair p; nsync_pair_start(&p, pcn, vn, code, rnd);
	crypto_wipe(rnd, sizeof rnd);
	nsync_hex(hex, p.mine, 32);
	if (nsync_pair_finish(&p, 1, other, pk, cf, tr) < 0) return send_line(fd, "NSYNC1 ERR bad point");
	if (send_line(fd, hex) < 0) return -1;
	nsync_pair_tag(tag, cf, "vita", tr); nsync_hex(hex, tag, 32);
	if (send_line(fd, hex) < 0 || recv_line(fd, line, sizeof line) < 0) return -1;
	uint8_t theirs[32], expect[32]; nsync_pair_tag(expect, cf, "pc", tr);
	if (nsync_unhex(theirs, 32, line) || crypto_verify32(theirs, expect)) {
		pair_tries++;
		if (pair_tries >= PAIR_TRIES) sceIoRemove(DATA "/pairing.txt");
		svc_log("pairing from %s: wrong code (%d of %d)", s, pair_tries, PAIR_TRIES);
		return send_line(fd, "ERR wrong code");
	}
	char path[96]; uint8_t rec[32 + 64]; sceClibMemset(rec, 0, sizeof rec); sceClibMemcpy(rec, pk, 32); cpy((char *)rec + 32, s, 64);
	sceIoMkdir(DATA "/pairs", 0777);
	sceClibSnprintf(path, sizeof path, DATA "/pairs/%s", pc_id);
	int w = write_small(path, rec, sizeof rec);
	crypto_wipe(pk, sizeof pk); crypto_wipe(rec, 32); crypto_wipe(cf, sizeof cf);
	if (w < 0) return send_line(fd, "ERR can't save the pairing");
	sceIoRemove(DATA "/pairing.txt"); pair_tries = 0;
	svc_log("paired with %s (%s)", s, pc_id);
	sceClibSnprintf(line, sizeof line, "OK %s %s", console_id, console_name);
	return send_line(fd, line);
}

/* ---- a connection ---- */
static int conn_thread(SceSize args, void *argp) {
	Conn *c = &conns[*(int *)argp];
	char line[256];
	int fd = c->fd, ok = 0;
	int tmo = 10 * 1000 * 1000; sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof tmo);
	if (recv_line(fd, line, sizeof line) < 0) goto done;
	if (!sceClibStrncmp(line, "NSYNC1 PAIR ", 12)) { do_pair(fd, line + 12); goto done; }
	if (sceClibStrncmp(line, "NSYNC1 HELLO ", 13)) { send_line(fd, "NSYNC1 ERR unknown"); goto done; }
	const char *s = line + 13; char nonce_hex[40]; uint8_t pcn[16], vn[16], rec[96], pk[32];
	arg(&s, c->pc_id, sizeof c->pc_id); arg(&s, nonce_hex, sizeof nonce_hex);
	char path[96]; sceClibSnprintf(path, sizeof path, DATA "/pairs/%s", c->pc_id);
	SceUID f = len_(c->pc_id) == 16 && !nsync_unhex(pk, 8, c->pc_id) ? sceIoOpen(path, SCE_O_RDONLY, 0) : -1;
	if (f < 0 || nsync_unhex(pcn, 16, nonce_hex)) { if (f >= 0) sceIoClose(f); send_line(fd, "NSYNC1 ERR unpaired"); goto done; }
	int rn = sceIoRead(f, rec, sizeof rec); sceIoClose(f);
	if (rn < 32) { send_line(fd, "NSYNC1 ERR unpaired"); goto done; }
	random_bytes(vn, 16);
	char hex[40]; nsync_hex(hex, vn, 16); sceClibSnprintf(line, sizeof line, "NSYNC1 HELLO %s", hex);
	if (send_line(fd, line) < 0) goto done;
	nsync_session(&c->ses, rec, pcn, vn, 1);
	crypto_wipe(rec, sizeof rec);
	tmo = 0; sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof tmo);
	nsyncGuardPcOnline(1);
	for (int first = 1; running; first = 0) {
		uint8_t head[4];
		if (recv_all(fd, head, 4) < 0) break;
		uint32_t blen = nsync_frame_length(head);
		if (blen < 16 || blen > NSYNC_PLAIN_MAX + 16 || recv_all(fd, c->in, (int)blen) < 0) break;
		/* (the request's plain text goes into the connection's own buffer: EVENTS runs beside other requests) */
		long n = nsync_open(&c->ses, c->in + 16 + 0, c->in, blen);   /* in place: plain lands after the MAC */
		if (n < 0) { svc_log("a frame from %s didn't open: closing", c->pc_id); break; }
		uint8_t *msg = c->in + 16;
		if (first && sceClibStrncmp((char *)msg, "HELLO", 5)) break;
		if (handle(c, msg, (int)n) < 0) break;
		ok = 1;
	}
done:
	(void)ok;
	sceNetSocketClose(fd);
	c->fd = -1;
	int any = 0; for (int i = 0; i < MAX_CONN; i++) if (conns[i].fd >= 0) any = 1;
	if (!any) nsyncGuardPcOnline(0);
	return sceKernelExitDeleteThread(0);
}

/* ---- listeners ---- */
static void wait_network(void) {
	for (int tries = 0; running; tries++) {
		int state = 0;
		if (sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED) return;
		sceKernelDelayThread(2000 * 1000);
	}
}
static int open_socket(int type) {
	for (;;) {
		wait_network();
		if (!running) return -1;
		int fd = sceNetSocket("nativ_sync", SCE_NET_AF_INET, type, 0), one = 1;
		sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof one);
		if (type == SCE_NET_SOCK_DGRAM) sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof one);
		SceNetSockaddrIn a; sceClibMemset(&a, 0, sizeof a);
		a.sin_family = SCE_NET_AF_INET; a.sin_port = sceNetHtons(NSYNC_PORT); a.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
		if (fd >= 0 && sceNetBind(fd, (SceNetSockaddr *)&a, sizeof a) >= 0 && (type == SCE_NET_SOCK_DGRAM || sceNetListen(fd, 4) >= 0)) return fd;
		if (fd >= 0) sceNetSocketClose(fd);
		sceKernelDelayThread(3000 * 1000);
	}
}
static volatile int tcp_fd = -1, udp_fd = -1;
static int listener(SceSize a, void *p) {
	int errors = 0;
	tcp_fd = open_socket(SCE_NET_SOCK_STREAM);
	svc_log("listening on %d", NSYNC_PORT);
	while (running) {
		SceNetSockaddrIn peer; unsigned int plen = sizeof peer;
		int fd = sceNetAccept(tcp_fd, (SceNetSockaddr *)&peer, &plen);
		if (fd < 0) {
			if (!running) break;
			if (++errors >= 3) { sceNetSocketClose(tcp_fd); tcp_fd = open_socket(SCE_NET_SOCK_STREAM); errors = 0; }   /* (after sleep) */
			else sceKernelDelayThread(200 * 1000);
			continue;
		}
		errors = 0;
		int slot = -1; for (int i = 0; i < MAX_CONN && slot < 0; i++) if (conns[i].fd < 0) slot = i;
		if (slot < 0) { sceNetSocketClose(fd); continue; }
		conns[slot].fd = fd; conns[slot].slot = slot;
		SceUID t = sceKernelCreateThread("nativ_sync_conn", conn_thread, 160, 0x6000, 0, 0, NULL);
		if (t < 0 || sceKernelStartThread(t, sizeof slot, &slot) < 0) { sceNetSocketClose(fd); conns[slot].fd = -1; }
	}
	return sceKernelExitDeleteThread(0);
}
static int discovery(SceSize a, void *p) {   /* "NSYNC1?" -> "NSYNC1 <console id> <name>" */
	udp_fd = open_socket(SCE_NET_SOCK_DGRAM);
	char b[64], out[128];
	while (running) {
		SceNetSockaddrIn peer; unsigned int plen = sizeof peer;
		int n = sceNetRecvfrom(udp_fd, b, sizeof b - 1, 0, (SceNetSockaddr *)&peer, &plen);
		if (n < 0) { if (!running) break; sceNetSocketClose(udp_fd); sceKernelDelayThread(1000 * 1000); udp_fd = open_socket(SCE_NET_SOCK_DGRAM); continue; }
		b[n] = 0;
		if (sceClibStrncmp(b, "NSYNC1?", 7)) continue;
		int ol = sceClibSnprintf(out, sizeof out, "NSYNC1 %s %s", console_id, console_name);
		sceNetSendto(udp_fd, out, ol, 0, (SceNetSockaddr *)&peer, plen);
	}
	return sceKernelExitDeleteThread(0);
}

/* ---- start and stop ---- */
static SceUID threads[3] = {-1, -1, -1}, mem = -1;
void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
	cmd_lock = sceKernelCreateMutex("nativ_sync_cmd", 0, 0, NULL);
	log_lock = sceKernelCreateMutex("nativ_sync_log", 0, 0, NULL);
	ev_lock = sceKernelCreateMutex("nativ_sync_ev", 0, 0, NULL);
	sceIoMkdir(DATA, 0777);
	/* buffers: a frame in and out per connection, and the shared reply text (about 900 KiB in all) */
	int fsz = NSYNC_PLAIN_MAX + 64, total = (MAX_CONN * 2 + 1) * fsz;
	total = (total + 0xFFF) & ~0xFFF;
	mem = sceKernelAllocMemBlock("nativ_sync", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, total, NULL);
	uint8_t *base = 0;
	if (mem < 0 || sceKernelGetMemBlockBase(mem, (void **)&base) < 0) { svc_log("no memory: 0x%08X", mem); return SCE_KERNEL_START_SUCCESS; }
	for (int i = 0; i < MAX_CONN; i++) { conns[i].fd = -1; conns[i].in = base + (2 * i) * fsz; conns[i].out = base + (2 * i + 1) * fsz; }
	plain_out = base + (2 * MAX_CONN) * fsz;
	char b[40];
	if (read_small(DATA "/console.txt", b, sizeof b) >= 16 && !nsync_unhex((uint8_t *)b + 20, 8, b)) cpy(console_id, b, 17);
	else { uint8_t r[8]; random_bytes(r, 8); nsync_hex(console_id, r, 8); write_small(DATA "/console.txt", console_id, 16); }
	if (sceRegMgrGetKeyStr("/CONFIG/SYSTEM", "username", console_name, sizeof console_name) < 0 || !console_name[0]) cpy(console_name, "PS Vita", sizeof console_name);
	svc_log("Nativ Sync service %s starting: console %s '%s'", VERSION, console_id, console_name);
	threads[0] = sceKernelCreateThread("nativ_sync_watch", watcher, 191, 0x2000, 0, 0, NULL);
	threads[1] = sceKernelCreateThread("nativ_sync_listen", listener, 160, 0x2000, 0, 0, NULL);
	threads[2] = sceKernelCreateThread("nativ_sync_find", discovery, 191, 0x1000, 0, 0, NULL);
	for (int i = 0; i < 3; i++) if (threads[i] >= 0) sceKernelStartThread(threads[i], 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) {
	running = 0;
	if (tcp_fd >= 0) sceNetSocketClose(tcp_fd);
	if (udp_fd >= 0) sceNetSocketClose(udp_fd);
	for (int i = 0; i < MAX_CONN; i++) if (conns[i].fd >= 0) sceNetSocketClose(conns[i].fd);
	for (int i = 0; i < 3; i++) if (threads[i] >= 0) { SceUInt t = 3000000; sceKernelWaitThreadEnd(threads[i], NULL, &t); }
	nsyncGuardClear();
	return SCE_KERNEL_STOP_SUCCESS;
}
