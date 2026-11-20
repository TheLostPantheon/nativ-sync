# Nativ Sync protocol (draft, version 0)

How the Nativ launcher on a computer and the Vita's background service talk. Nothing here is final until the first
release; the version in HELLO changes when it does.

## Finding the Vita

The launcher sends a UDP datagram `NSYNC?` to port 1340 on the local network's broadcast address. A paired or pairable
Vita answers `NSYNC <version> <console name>` from its address.

## Pairing

The app shows a six-digit code. The launcher connects (TCP 1340), sends `PAIR <launcher name>`, and the player types the
code into the launcher. Both sides derive a 32-byte key from the code and a nonce each sends (HKDF-SHA256); the Vita
keeps the key and the launcher's name until the pairing is removed in the app. A wrong code is refused after three tries
for a minute.

## Requests

Every request after pairing is `<command> <args...>\n` followed by an HMAC-SHA256 of the request (and any payload) under
the pairing key; a reply is `OK <length>\n<payload>` or `ERR <reason>\n`. Replies are signed the same way.

| Command | What it does |
|---|---|
| `HELLO` | protocol version, firmware, free space, the app running in front (a game's title id, or none) |
| `LIST` | the games with saves: title id, name, installed version, the save's last change, and its state as the Vita last knew it (DESIGN.md) |
| `MANIFEST <title>` | the save as it is now: its own files (not `sce_sys`, `sce_pfs`) with size and SHA-256, its used slots with their texts, the manifest's own hash, and the base's hash |
| `GET <title> <path>` | one file's plain bytes; `slot/<N>` is slot N's 0x34C-byte parameter (from `sdslot.dat`) |
| `PUT <title> <path> <length>` | one file, or `slot/<N>`, into a staging area for that title (nothing is applied yet) |
| `COMMIT <title> <manifest hash>` | back the save up, apply the staged files and slots, read them back and check them against the manifest; on any failure the backup is restored. The applied manifest becomes the base |
| `BASE <title> <manifest hash>` | record the base after a sync that copied nothing to the Vita (the PC took the Vita's save, or both were equal) |
| `STATE <title> <pc state> <summary>` | what the PC has since the base (unchanged, changed, its version and its slot texts), so the app can show a conflict while the PC is away |
| `CHOICES` | conflict choices made in the app since the last connection (title, which side to keep); the launcher carries them out |
| `BACKUPS <title>` / `RESTORE <title> <backup>` | the backups the Vita keeps, and putting one back |

The Vita keeps one base per paired launcher, in `ux0:data/nativ-sync/state/`.

A save is never read or written while its game is running (`HELLO`'s front app); such a request answers `ERR busy`.
