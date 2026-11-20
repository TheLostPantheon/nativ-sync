# Design

How Nativ Sync decides what to copy, what it does when both sides changed, and what the player sees. The wire format is
in PROTOCOL.md.

## What crosses

A Vita save is a folder under `ux0:user/00/savedata/<TITLE ID>/`. Mounted decrypted (README, How it works), it holds:

- **The game's own files**, such as Shovel Knight's `saveData.bin` or Persona 4 Golden's `system.bin`, `data0001.bin` and
  `data0002.bin`. These are copied as they are. Nativ keeps the same files in its `savedata0` folder.
- **Slot details**: which slots exist and the texts the game wrote for the system save screen (title, subtitle, detail,
  time). The Vita keeps them in `sce_sys/sdslot.dat`; Nativ keeps one `.slot<N>` file per slot. Both hold the same
  0x34C-byte structure (SceAppUtilSaveDataSlotParam), so they are translated, not copied. A game that checks whether a
  slot exists before loading it needs this.
- **Everything else in `sce_sys` stays on its own side**: the keys, the account id, `param.sfo`.

`sdslot.dat` as read from a Shovel Knight save: `SDSL`, the slot count (256) at +8, a used byte per slot at +0x200 + N,
then from +0x400 one 0x400-byte record per slot, whose first 0x34C bytes are the structure above. The used bytes are to be
confirmed with a game that has several slots (Persona 4 Golden).

## Deciding what to copy

Each side remembers the save as it was at the last sync: its files and slots, each with a SHA-256. This is the **base**.
At each sync, both sides' current saves are compared with it:

| Vita since the base | PC since the base | What happens |
|---|---|---|
| unchanged | unchanged | nothing |
| changed | unchanged | the Vita's save is copied to the PC |
| unchanged | changed | the PC's save is copied to the Vita |
| changed | changed | a **conflict**: nothing is copied until the player chooses |

A first sync with saves on both sides and no base is a conflict too, unless they are identical.

Times are never compared to pick a side. The two clocks can disagree, and the later save isn't always the one with more
progress. A save is copied whole (all its files and slots together). Games keep shared state across their files, so taking
one side's changed slot and the other side's would make a save the game may reject.

Syncs happen when a game starts or stops in Nativ, when the Vita comes back onto the network, and on **Sync now**. So
the usual case, playing on the PC and then picking up the Vita, needs nothing from the player: the PC's save reached the
Vita when the game stopped in Nativ, or when the Vita woke on the network.

## Starting a game on the Vita

Players start games from the home screen, not through Nativ Sync. The kernel module watches every app start: SceProcessmgr's
ksceKernelCreateProcess (SceProcessmgrForKernel 0xEB1F8EF7, NID 0x68068618 from 3.63 on; 0x71CF71FD on 3.60), which gets
the title id and the path being started (`ux0:/patch/<ID>/eboot.bin` for an updated game). Checked on 3.65: a home screen
launch passes through it, and holding it there keeps the game's own start-up splash on screen, which is what the Vita
shows while a game loads anyway (a 3-second hold looked like a slower start).

So a game with a copy from the PC waiting syncs during that splash, with nothing extra on screen. A time limit (a few
seconds) starts the game anyway if the PC stops answering, and the sync is left for the next connection.

A game with an open conflict gets the choice on the spot, drawn over its splash while the launch is held: checked on
3.65 with a prototype screen. The kernel module allocates a framebuffer in kernel CDRAM (memblock type 0x40404006, 1024
pitch, A8B8G8R8), shows it on the primary display's overlay layer (ksceDisplaySetFrameBufInternal(head, 1, ...), the layer
the shell's own overlays use, alpha-blended over the splash), reads the buttons with ksceCtrlPeekBufferPositive, and on an
answer clears the layer to transparent and lets the launch go on; the game then starts normally. The hook runs on a stack
of its own (ksceKernelRunWithStack, 16 KB): the launch thread's is small, and without it the first try stopped launches
dead. BubbleLock (Rinnegatamante) locks games at launch the same way.

Two other routes were tried and dropped: starting Nativ Sync in the game's place (the swapped process takes the game's
identity: title id, app0) and refusing the launch (the shell shows "An error has occurred (C1-2758-2)").

The screen shows the two saves (when, and the game's own description), and ✕ Keep the Vita's, □ Use the PC's, ○ Play
without syncing. No answer within a time limit plays without syncing. Any other game starts as it would without Nativ
Sync.

Playing on the Vita away from the PC after playing on the PC can't be caught: the Vita doesn't know what the PC did, so it
plays its own save, and the conflict appears at the next connection.

## Conflicts

The player picks a side, seeing both saves:

- when each was saved, by that device's own clock;
- the game's own description, from the slot texts ("Chapter 5 · 32:10 played"), where it writes one;
- which files and slots differ ("slot 2 changed on the Vita, slot 1 on this PC").

The choices are **Keep the Vita's**, **Keep this PC's** and **Decide later**. The save not kept goes into the backups on
its own side, so a wrong choice is undone with **Restore a backup**.

Either side can settle it, and the first choice wins:

- **The launcher** asks when Play is pressed on a conflicted game, before the game starts. Decide later plays the PC's
  save and leaves the conflict open.
- **The Vita app** lists the game under **Needs a choice**. A choice made on the Vita while the PC is away is kept and
  carried out at the next connection.
- **Starting the game on the Vita** brings the choice up too, through the launch guard below. A conflict found while the
  Vita is idle also raises a system notification ("Shovel Knight: progress on both your Vita and PC").

The Vita learns what the PC has at each connection, so its view of a conflict is as of the last contact, and says so.

## Versions

A game updated on the Vita may save in a format its older version can't read: Shovel Knight on the Vita (patched) writes
save version 8, Nativ's 1.00 writes version 5. Each side reports the game's installed version (`ux0:patch/<ID>` first,
then `ux0:app/<ID>`, on the Vita). When they differ, nothing is copied and the game shows **Version differs** with both
versions. The fix is on the player's side: the same update in Nativ (its packs take updates), or the same version on the
Vita.

## Save origin

Three things about where a save came from decide whether the other side can use it:

- **The game's version.** Versions, above.
- **The region.** One game has a title id per region (Uncharted is PCSA00029 in the US, PCSF00001 in Europe), and
  saves are synced between equal title ids. Regional editions of one build usually share a save format, but not always,
  so a save can cross between two title ids only where Nativ's pack lists them as variants of the same game, and the
  player links them once. The version check still applies.
- **The account.** A Vita save belongs to one console and one PSN account. The console part lives in `sce_sys`, which
  stays on its own side. But some games also write the account id into their own save data, to stop saves being
  shared, and Nativ gives each install a random account id of its own. So pairing makes Nativ use the Vita's account
  id (from the Vita's settings, read by the service), and to every game both sides are the same account.

## Safety

- A save is never read or written while its game is running on the Vita.
- Writing goes through a staging folder: the new files arrive there first, the current save is backed up, the new files
  replace it, and they are read back and checked against the manifest. Any failure puts the backup back.
- Backups: the last 10 per game on each side, and every save a conflict choice replaced, kept until the player removes it.
- The kernel module refuses a firmware it has no table entry for, and accepts only a title id as a path.

## The Vita app

One app, for setup and status. Saves are managed only as far as safety needs (restore a backup); browsing, exporting and
importing are left to VitaShell and Vita Save Manager, because each extra way to write a save is another way to lose one.
Touch and buttons both work. The look follows Nativ's launcher: a dark ground with its amber accent.

1. **First run**: what it does in two lines, one **Install** button, and the note that it needs one restart.
2. **Home**: a status line ("Running · paired with Desk PC · synced 2 min ago"), then each game with its icon, its name
   and its state: Needs a choice, Newer on Vita, Newer on PC, In sync, Version differs, Off. Games needing a choice are
   listed first.
3. **A game**: its recent syncs, sync on or off for it, **Sync now**, the conflict choice when there is one, and **Restore
   a backup** (by date).
4. **Settings**: pair or unpair, uninstall, firmware and app version, and the log for bug reports.

Names come from each game's `sce_sys/param.sfo` under `ux0:app`, which is readable as it is; icons from the home
screen's copies in `ur0:appmeta/<ID>/icon0.png`. A save whose game was deleted takes its name from the save's own
`param.sfo`, read through the mount.

## The launcher

Settings gains **Vita sync**: pairing (the code the app shows), sync on or off per game, and the backups. Each game's page
shows its sync state, and Play asks the conflict choice when there is one.
