# Nativ Sync

The PlayStation Vita side of [Project Nativ](https://github.com/TheLostPantheon/nativ)'s save sync: play a game on your
Vita, carry on in Nativ on your computer, and back again.

**Status: in development.** Running on a 3.65 Vita: the app installs the service (taiHEN config backed up first), the
service starts with the Vita, pairing works from the app's code screen, and Nativ (merged: Settings, Vita sync) syncs
with it, including the conflict choice at launch on either side.

## How it works

A Vita save is encrypted with a key that never leaves the console, so a computer can't read it on its own. The Vita can:
its own file system driver decrypts a save for a game that reads it and re-encrypts what the game writes. Nativ Sync
asks it to do the same for its own process (the way VitaShell and Vita Save Manager do), so the save's plain files can be
read and written while the keys stay where they are. Only the game's own files cross between the two; each side keeps
its own `sce_sys` (keys, account id, slot details).

Planned shape:
- **The app** (this repository's `.vpk`): installs the background service, shows a pairing code and the sync status, and
  removes everything again.
- **A background service** in the Vita's shell: always on and light (one listening socket, idle otherwise), it answers
  the paired Nativ launcher on your network. It never touches the save of a game that is running.
- **A small kernel module** with one job: mounting a save decrypted for the calling process. A firmware it doesn't
  know, it refuses.

Each side's save is compared with how it was at the last sync: a save changed on one side is copied to the other, and a
save changed on both is a question for you, answered in the launcher or the app. Whatever is replaced is backed up
first, on the side it's replaced on. DESIGN.md has the rules, the conflict choice and the screens.

## Installing

1. Install `nativ_sync.vpk`, with VitaDB Downloader or VitaShell.
2. Open **Nativ Sync**, press □ for Settings, choose **Install the background service**, and restart the Vita. Installing
   adds two lines to your taiHEN config, backed up first as `config.txt.nativ-sync-backup`.
3. In the app, press △ to **Pair a computer**: it shows a six-digit code. In Nativ, open Settings, Vita sync, Find my
   Vita, and type it.

From then on games sync on their own. A game whose save changed on both sides shows **Needs a choice** in the app (✕
opens it), and the Vita asks too when you start it.

**Removing:** Settings, Remove the background service, then restart. Your pairings and backups stay in
`ux0:data/nativ-sync`.

**If it ever fails to start:** the Vita counts boots the service didn't get through. After two in a row its launch hook
stays off, after three the service does too, and the Vita starts as if Nativ Sync weren't installed. The app says so,
and Settings turns it back on.

## Supported firmware

3.55 to 3.74, every retail firmware a Vita can be modded on. Checked on 3.65; the others use VitaShell's tables, 3.74
from RealYoti's maintained fork, checked against 3.74's own SceAppMgr.

## Building

VitaSDK, then:

    cmake -S . -B build && make -C build

`build/nativ_sync.vpk` installs with VitaShell.

## Credits

- VitaShell (TheOfficialFloW): the save mount's method and its per-firmware table; RealYoti's fork for 3.74's entry.
- BubbleLock (Rinnegatamante): how a kernel module puts its own screen up while a launch waits.
- Vita Save Manager (d3m3vilurr): the mount ids it tries, the one that works from the shell among them.

## Licence

GPL-3.0-or-later (LICENSE).
