/* draw.c: see draw.h. Shapes are anti-aliased by 4x4 supersampling at their edges; text uses the baked faces of font.h. */
#include "draw.h"
#include "font.h"

static void blend(uint32_t *d, uint32_t s, int cover) {   /* "over", straight alpha; cover 0..255 scales the source's */
	int sa = (int)(s >> 24) * cover / 255;
	if (sa <= 0) return;
	if (sa >= 255) { *d = s | 0xFF000000u; return; }
	uint32_t dv = *d; int da = dv >> 24;
	int oa = sa + da * (255 - sa) / 255;
	if (oa <= 0) { *d = 0; return; }
	uint32_t out = (uint32_t)oa << 24;
	for (int sh = 0; sh < 24; sh += 8) {
		int sc = (s >> sh) & 0xFF, dc = (dv >> sh) & 0xFF;
		int v = (sc * sa + dc * da * (255 - sa) / 255) / oa;
		out |= (uint32_t)(v > 255 ? 255 : v) << sh;
	}
	*d = out;
}
static void put(Canvas *c, int x, int y, uint32_t color, int cover) {
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
	blend(&c->px[y * c->pitch + x], color, cover);
}

void draw_fill(Canvas *c, uint32_t color) {
	for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) c->px[y * c->pitch + x] = color;
}
void draw_rect(Canvas *c, int x0, int y0, int x1, int y1, uint32_t color) {
	for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) put(c, x, y, color, 255);
}

/* coverage (0..16) of the pixel at (x, y) by a shape, sampled 4x4; inside(px16, py16) takes 1/16-pixel coordinates */
#define COVER(expr) ({ int n_ = 0; for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) { \
	int px16 = x * 16 + sx * 4 + 2, py16 = y * 16 + sy * 4 + 2; (void)px16; (void)py16; if (expr) n_++; } n_; })

static int in_round(int px16, int py16, int x0, int y0, int x1, int y1, int r) {   /* (all in 1/16 px) */
	if (px16 < x0 || py16 < y0 || px16 >= x1 || py16 >= y1) return 0;
	int cx = px16 < x0 + r ? x0 + r : px16 >= x1 - r ? x1 - r : px16;
	int cy = py16 < y0 + r ? y0 + r : py16 >= y1 - r ? y1 - r : py16;
	int dx = px16 - cx, dy = py16 - cy;
	return dx * dx + dy * dy <= r * r;
}
void draw_round_rect(Canvas *c, int x0, int y0, int x1, int y1, int radius, uint32_t fill, uint32_t edge) {
	int X0 = x0 * 16, Y0 = y0 * 16, X1 = x1 * 16, Y1 = y1 * 16, R = radius * 16;
	for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
		int corner = (x < x0 + radius || x >= x1 - radius) && (y < y0 + radius || y >= y1 - radius);
		int outer = corner ? COVER(in_round(px16, py16, X0, Y0, X1, Y1, R)) : 16;
		if (!outer) continue;
		if (edge >> 24) {   /* a 2-pixel edge: what's outside the inset shape */
			int inner = (x < x0 + 2 || x >= x1 - 2 || y < y0 + 2 || y >= y1 - 2) ? 0 :
				corner ? COVER(in_round(px16, py16, X0 + 32, Y0 + 32, X1 - 32, Y1 - 32, R - 32)) : 16;
			put(c, x, y, fill, inner * 255 / 16);
			if (outer > inner) put(c, x, y, edge, (outer - inner) * 255 / 16);
		} else put(c, x, y, fill, outer * 255 / 16);
	}
}
void draw_disc(Canvas *c, int cx, int cy, int r, uint32_t color) {
	int CX = cx * 16, CY = cy * 16, R2 = r * 16 * r * 16;
	for (int y = cy - r - 1; y <= cy + r; y++) for (int x = cx - r - 1; x <= cx + r; x++) {
		int n = COVER((px16 - CX) * (px16 - CX) + (py16 - CY) * (py16 - CY) <= R2);
		if (n) put(c, x, y, color, n * 255 / 16);
	}
}
void draw_ring(Canvas *c, int cx, int cy, int r, int t, uint32_t color) {
	int CX = cx * 16, CY = cy * 16, RO = r * 16 * r * 16, RI = (r - t) * 16 * (r - t) * 16;
	for (int y = cy - r - 1; y <= cy + r; y++) for (int x = cx - r - 1; x <= cx + r; x++) {
		int n = COVER(({ int d2 = (px16 - CX) * (px16 - CX) + (py16 - CY) * (py16 - CY); d2 <= RO && d2 >= RI; }));
		if (n) put(c, x, y, color, n * 255 / 16);
	}
}
void draw_line(Canvas *c, int x0, int y0, int x1, int y1, int t, uint32_t color) {   /* a thick segment, square ends */
	int X0 = x0 * 16, Y0 = y0 * 16, dx = (x1 - x0) * 16, dy = (y1 - y0) * 16;
	long long len2 = (long long)dx * dx + (long long)dy * dy, half = t * 8;
	int lo_x = (x0 < x1 ? x0 : x1) - t, hi_x = (x0 > x1 ? x0 : x1) + t, lo_y = (y0 < y1 ? y0 : y1) - t, hi_y = (y0 > y1 ? y0 : y1) + t;
	for (int y = lo_y; y <= hi_y; y++) for (int x = lo_x; x <= hi_x; x++) {
		int n = COVER(({ long long ux = px16 - X0, uy = py16 - Y0, along = ux * dx + uy * dy, across = ux * dy - uy * dx;
			along >= 0 && along <= len2 && across * across <= half * half * len2; }));
		if (n) put(c, x, y, color, n * 255 / 16);
	}
}

static uint32_t next_cp(const char **s) {   /* UTF-8; a malformed byte reads as itself */
	const unsigned char *p = (const unsigned char *)*s; uint32_t cp = *p++;
	if (cp >= 0xE0 && cp < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { cp = (cp & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
	else if (cp >= 0xC0 && cp < 0xE0 && (p[0] & 0xC0) == 0x80) { cp = (cp & 0x1F) << 6 | (p[0] & 0x3F); p += 1; }
	else if (cp >= 0xF0) { while ((*p & 0xC0) == 0x80) p++; cp = '?'; }
	*s = (const char *)p;
	return cp;
}
static const Glyph *glyph(const Face *f, uint32_t cp) {
	int lo = 0, hi = f->n - 1;
	while (lo <= hi) { int m = (lo + hi) / 2; if (f->g[m].cp == cp) return &f->g[m]; if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1; }
	return cp == '?' ? &f->g[0] : glyph(f, '?');
}
static void draw_glyph(Canvas *c, const Glyph *g, int x16, int baseline, uint32_t color) {
	int gx = (x16 + 8) / 16 + g->x, gy = baseline + g->y, stride = (g->w + 1) / 2;
	const unsigned char *a = FONT_DATA + g->off;
	for (int r = 0; r < g->h; r++) for (int i = 0; i < g->w; i++) {
		int v = a[r * stride + i / 2]; v = i & 1 ? v & 15 : v >> 4;
		if (v) put(c, gx + i, gy + r, color, v * 17);
	}
}
int draw_text_width(const Face *f, const char *s) {
	int w = 0;
	while (*s) w += glyph(f, next_cp(&s))->adv;
	return (w + 8) / 16;
}
int draw_text(Canvas *c, const Face *f, int x, int baseline, uint32_t color, const char *s, int max_w) {
	int x16 = x * 16, limit = max_w > 0 ? max_w * 16 : 0x7FFFFFFF;
	const Glyph *dots = glyph(f, 0x2026);
	if (max_w > 0 && draw_text_width(f, s) > max_w) limit -= dots->adv; else limit = 0x7FFFFFFF;
	while (*s) {
		const Glyph *g = glyph(f, next_cp(&s));
		if (x16 - x * 16 + g->adv > limit) { draw_glyph(c, dots, x16, baseline, color); x16 += dots->adv; break; }
		draw_glyph(c, g, x16, baseline, color); x16 += g->adv;
	}
	return (x16 - x * 16 + 8) / 16;
}
