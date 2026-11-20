/* preview: renders the launch screens on a computer, over a background picture (a 960x544 PPM, P6) if one is given, to
 * PPM files in the current directory. Built from the kernel module's own drawing code:
 *     cc -O2 -Ikernel tools/preview.c kernel/draw.c kernel/screens.c kernel/font.c -o build/preview && build/preview [splash.ppm] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screens.h"

static uint32_t bg[960 * 544];
static void load_bg(const char *path) {
	for (int i = 0; i < 960 * 544; i++) bg[i] = RGBA(60, 60, 60, 255);
	FILE *f = path ? fopen(path, "rb") : 0; int w, h, m;
	if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &m) != 3 || w != 960 || h != 544) { if (f) fclose(f); return; }
	fgetc(f);
	for (int i = 0; i < 960 * 544; i++) { int r = fgetc(f), g = fgetc(f), b = fgetc(f); bg[i] = RGBA(r, g, b, 255); }
	fclose(f);
}
static void save(Canvas *c, const char *name) {   /* the overlay composited over the background, as the display would */
	FILE *f = fopen(name, "wb"); fprintf(f, "P6 960 544 255\n");
	for (int y = 0; y < 544; y++) for (int x = 0; x < 960; x++) {
		uint32_t o = c->px[y * c->pitch + x], b = bg[y * 960 + x]; int a = o >> 24;
		for (int sh = 0; sh < 24; sh += 8) fputc((((o >> sh) & 255) * a + ((b >> sh) & 255) * (255 - a)) / 255, f);
	}
	fclose(f); printf("%s\n", name);
}
int main(int argc, char **argv) {
	load_bg(argc > 1 ? argv[1] : 0);
	Canvas c = { calloc(1024 * 544, 4), 960, 544, 1024 };
	NsyncGuardInfo g; memset(&g, 0, sizeof g);
	strcpy(g.titleid, "PCSE00640"); strcpy(g.game, "Shovel Knight: Treasure Trove"); strcpy(g.pc_name, "Desk PC");
	strcpy(g.vita_when, "Today, 21:40"); strcpy(g.vita_detail, "Profile 1 \xC2\xB7 2:13:05 played");
	strcpy(g.pc_when, "Today, 18:02"); strcpy(g.pc_detail, "Profile 1 \xC2\xB7 1:58:40 played");
	screen_choice(&c, &g, 1); save(&c, "choice.ppm");
	screen_choice(&c, &g, 0); save(&c, "choice_offline.ppm");
	strcpy(g.game, "Uncharted\xE2\x84\xA2: Golden Abyss \xE2\x80\x93 a very long name that needs cutting short at the edge"); g.vita_detail[0] = g.pc_when[0] = 0;
	screen_choice(&c, &g, 1); save(&c, "choice_long.ppm");
	screen_progress(&c, "Shovel Knight: Treasure Trove", "Getting your save from Desk PC\xE2\x80\xA6", 0); save(&c, "progress.ppm");
	screen_progress(&c, "Shovel Knight: Treasure Trove", "Desk PC didn't answer, so the game starts with the save on this Vita.", "Your choice stays open for next time."); save(&c, "note.ppm");
	return 0;
}
