/* screens.h: what the kernel module shows over a game's start-up splash while it holds the launch. */
#pragma once
#include "draw.h"
#include "nsync_kernel.h"

/* the conflict choice for one game; pc_ok: the PC can be asked for its save now (else that option is greyed out) */
void screen_choice(Canvas *c, const NsyncGuardInfo *g, int pc_ok);
/* a game's name and one line of what's happening ("Getting your save from Desk PC..."), or two for a note */
void screen_progress(Canvas *c, const char *game, const char *line1, const char *line2);
void screen_clear(Canvas *c);   /* fully transparent: the layer gets out of the way */
