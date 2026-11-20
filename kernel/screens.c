/* screens.c: see screens.h. 960x544, Nativ's dark ground and amber accent, faces from font.h. */
#include "screens.h"
#include "font.h"

#define BACKDROP RGBA(8, 9, 12, 184)   /* dims the splash behind */
#define CARD     RGBA(22, 24, 31, 255)
#define EDGE     RGBA(52, 56, 68, 255)
#define PANEL    RGBA(30, 33, 42, 255)
#define INK      RGBA(233, 230, 223, 255)
#define DIM      RGBA(144, 151, 163, 255)
#define FAINT    RGBA(96, 102, 116, 255)
#define AMBER    RGBA(240, 180, 76, 255)
#define KNOB     RGBA(40, 44, 56, 255)

static void cat(char *d, int n, const char *a, const char *b, const char *c) {   /* d = a b c, cut to fit */
	const char *s[3] = {a, b, c}; int k = 0;
	for (int i = 0; i < 3; i++) for (const char *p = s[i]; p && *p && k < n - 1; p++) d[k++] = *p;
	d[k] = 0;
}
static const char *or(const char *s, const char *dflt) { return s && s[0] ? s : dflt; }

enum { CROSS, SQUARE, CIRCLE };
static void button(Canvas *c, int kind, int cx, int cy, int on) {
	uint32_t col = !on ? FAINT : kind == CROSS ? RGBA(122, 166, 255, 255) : kind == SQUARE ? RGBA(240, 140, 200, 255) : RGBA(255, 110, 110, 255);
	draw_disc(c, cx, cy, 15, KNOB);
	if (kind == CROSS) { draw_line(c, cx - 7, cy - 7, cx + 7, cy + 7, 3, col); draw_line(c, cx - 7, cy + 7, cx + 7, cy - 7, 3, col); }
	else if (kind == SQUARE) { draw_rect(c, cx - 7, cy - 7, cx + 8, cy - 4, col); draw_rect(c, cx - 7, cy + 5, cx + 8, cy + 8, col);
		draw_rect(c, cx - 7, cy - 7, cx - 4, cy + 8, col); draw_rect(c, cx + 5, cy - 7, cx + 8, cy + 8, col); }
	else draw_ring(c, cx, cy, 8, 3, col);
}

void screen_clear(Canvas *c) { draw_fill(c, 0); }

void screen_choice(Canvas *c, const NsyncGuardInfo *g, int pc_ok) {
	const char *pc = or(g->pc_name, "your PC");
	char line[160];
	draw_fill(c, BACKDROP);
	draw_round_rect(c, 70, 50, 890, 494, 18, CARD, EDGE);
	draw_text(c, &FACE_STRONG, 104, 92, AMBER, "Nativ Sync", 0);
	draw_text(c, &FACE_TITLE, 104, 136, INK, or(g->game, g->titleid), 752);
	cat(line, sizeof line, "Your Vita and ", pc, " both have new progress since the last sync.");
	draw_text(c, &FACE_BODY, 104, 172, DIM, line, 752);
	draw_text(c, &FACE_BODY, 104, 196, DIM, "Which save should this game continue from?", 752);
	const char *who[2] = {"On this Vita", 0}, *when[2] = {g->vita_when, g->pc_when}, *what[2] = {g->vita_detail, g->pc_detail};
	char on_pc[80]; cat(on_pc, sizeof on_pc, "On ", pc, 0); who[1] = on_pc;
	for (int i = 0; i < 2; i++) {
		int x = 104 + i * 382;
		draw_round_rect(c, x, 222, x + 366, 330, 12, PANEL, 0);
		draw_text(c, &FACE_STRONG, x + 20, 252, AMBER, who[i], 326);
		draw_text(c, &FACE_STRONG, x + 20, 282, INK, or(when[i], "Changed since the last sync"), 326);
		draw_text(c, &FACE_BODY, x + 20, 310, DIM, or(what[i], ""), 326);
	}
	button(c, CROSS, 119, 376, 1);  draw_text(c, &FACE_STRONG, 144, 383, INK, "Keep the Vita's", 0);
	button(c, SQUARE, 375, 376, pc_ok); draw_text(c, &FACE_STRONG, 400, 383, pc_ok ? INK : FAINT, "Use the PC's", 0);
	button(c, CIRCLE, 605, 376, 1); draw_text(c, &FACE_STRONG, 630, 383, INK, "Play without syncing", 0);
	if (pc_ok) draw_text(c, &FACE_SMALL, 104, 430, DIM, "The save you don't keep is backed up, so you can change your mind later.", 752);
	else { cat(line, sizeof line, pc, " isn't connected right now, so its save can't be fetched.", 0); draw_text(c, &FACE_SMALL, 104, 430, DIM, line, 752); }
	draw_text(c, &FACE_SMALL, 104, 456, FAINT, "With no answer in a minute, the game starts without syncing.", 752);
}

void screen_progress(Canvas *c, const char *game, const char *line1, const char *line2) {
	draw_fill(c, BACKDROP);
	draw_round_rect(c, 180, 170, 780, line2 && line2[0] ? 384 : 360, 18, CARD, EDGE);
	draw_text(c, &FACE_STRONG, 214, 212, AMBER, "Nativ Sync", 0);
	draw_text(c, &FACE_TITLE, 214, 256, INK, game, 532);
	draw_text(c, &FACE_BODY, 214, 300, DIM, line1, 532);
	if (line2 && line2[0]) draw_text(c, &FACE_BODY, 214, 326, DIM, line2, 532);
}
