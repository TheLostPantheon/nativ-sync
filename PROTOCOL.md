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
| `LIST` | the games with saves: title id, the save's last change, its param.sfo title and detail, the game's installed version |
| `MANIFEST <title>` | the save's own files (not `sce_sys`, `sce_pfs`): path, size, SHA-256 |
| `GET <title> <path>` | one file's plain bytes |
| `PUT <title> <path> <length>` | one file into a staging area for that title (nothing is applied yet) |
| `COMMIT <title> <manifest hash>` | back the save's current files up, apply the staged ones, read them back and check them against the manifest; on any failure the backup is restored |
| `BACKUPS <title>` / `RESTORE <title> <backup>` | the backups the Vita keeps, and putting one back |

A save is never read or written while its game is running (`HELLO`'s front app); such a request answers `ERR busy`.
