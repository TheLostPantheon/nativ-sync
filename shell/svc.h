/* svc.h: the background service's parts (service.c: network, sessions, events; saves.c: the saves themselves). */
#pragma once
#include <stdint.h>
#include "nsync_kernel.h"
#include "nsync_proto.h"

#define DATA "ux0:data/nativ-sync"
#define SAVES "ux0:user/00/savedata"
#define SHELL_TITLE "NPXS19999"   /* the shell's own title id: what it mounts saves as */

/* ---- service.c ---- */
void svc_log(const char *fmt, ...);
int svc_valid_id(const char *t);
/* text building: appends to out (cap bytes, *len so far); jstr writes a JSON string with quotes and escapes */
void svc_cat(char *out, int cap, int *len, const char *fmt, ...);
void svc_jstr(char *out, int cap, int *len, const char *s);
const char *svc_guard_name(const char *id);   /* "none", "sync", "conflict": what the service set for a game */

/* ---- saves.c (each call mounts and unmounts the save itself; the caller holds the sync guard around it) ---- */
#define SAVE_BUSY  0x80F10101   /* (as errors to report: these come with a reason in *why) */
int saves_list(char *out, int cap, const char *pc_id);   /* LIST's JSON; returns its length */
int save_manifest(const char *id, char *out, int cap);    /* MANIFEST's JSON (length), or < 0 */
int save_get(const char *id, const char *path, uint32_t off, uint32_t len, uint8_t *out);   /* bytes read, or < 0 */
int save_put(const char *id, const char *path, uint32_t off, uint32_t total, const uint8_t *data, uint32_t n);
/* COMMIT: text is the manifest's canonical text, hash_hex its hash; on success *why is empty and out holds the reply */
int save_commit(const char *id, const char *pc_id, const char *hash_hex, const char *text, int text_len, char *out,
                int cap, char *why, int why_cap);
int save_set_base(const char *pc_id, const char *id, const char *hash_hex);
int save_backups(const char *id, char *out, int cap);    /* BACKUPS' JSON */
int save_restore(const char *id, const char *name, char *why, int why_cap);
/* the TITLE and APP_VER of a param.sfo (stored plain under ux0:app and ux0:patch) */
int sfo_read(const char *path, char *title, int title_cap, char *version, int version_cap);
