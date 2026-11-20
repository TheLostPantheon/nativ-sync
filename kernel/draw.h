/* draw.h: the little drawing the launch screens need, over a 32-bit pixel buffer (A8B8G8R8, the Vita display's
 * format). Freestanding: no kernel calls and no libc, so tools/preview.c renders the same screens on a computer. */
#pragma once
#include <stdint.h>

#define RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))

typedef struct { uint16_t cp; int8_t x, y; uint8_t w, h; uint16_t adv; uint32_t off; } Glyph;   /* adv in 1/16 px */
typedef struct { const Glyph *g; int n; int ascent, descent; } Face;
typedef struct { uint32_t *px; int w, h, pitch; } Canvas;

void draw_fill(Canvas *c, uint32_t color);
void draw_rect(Canvas *c, int x0, int y0, int x1, int y1, uint32_t color);
void draw_round_rect(Canvas *c, int x0, int y0, int x1, int y1, int radius, uint32_t fill, uint32_t edge);
void draw_disc(Canvas *c, int cx, int cy, int r, uint32_t color);
void draw_ring(Canvas *c, int cx, int cy, int r, int thickness, uint32_t color);
void draw_line(Canvas *c, int x0, int y0, int x1, int y1, int thickness, uint32_t color);
int draw_text_width(const Face *f, const char *utf8);
/* text from its left end on a baseline; past max_w (0: no limit) it ends in an ellipsis. Returns the width drawn. */
int draw_text(Canvas *c, const Face *f, int x, int baseline, uint32_t color, const char *utf8, int max_w);
