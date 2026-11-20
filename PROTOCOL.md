# Nativ Sync protocol, version 1

How Nativ's launcher on a computer (the **PC**) and the background service on a Vita talk. The PC decides what to
copy (DESIGN.md); the Vita serves its saves, applies what it is sent, and runs the launch guard. Nothing here is final
until the first release.

All of it is in `common/nsync_proto.c` (MIT licensed, so Nativ can use the same file), over
[Monocypher](https://monocypher.org) 4.0.2. Hashes are BLAKE2b; text is UTF-8; hex is lowercase.

## Finding the Vita

The PC sends the UDP datagram `NSYNC1?` to port 1340 on the network's broadcast address. A Vita running the service
answers from its address with `NSYNC1 <console id> <console name>`: the console id is 16 hex digits the service makes
once and keeps, the name is the Vita's own (Settings, System, Name).

## Pairing

The player opens pairing in the Vita app, which shows a six-digit code for five minutes, and types it into the
launcher. Pairing is a password-authenticated key exchange (CPace, over X25519 and Elligator 2), so the code never
crosses the network and can't be worked out by someone watching: an attacker gets one guess per attempt, and the Vita
drops the code after three wrong ones.

Over TCP port 1340, lines of text:

1. PC: `NSYNC1 PAIR <pc id> <pc nonce> <pc name>` (pc id: 16 hex digits the PC makes once; nonce: 16 random bytes, hex;
   the name runs to the end of the line).
2. Vita: `NSYNC1 PAIR <vita nonce>`, or `NSYNC1 ERR <reason>` (no code showing, too many tries) and it closes.
3. Both: `sid = pc nonce ‖ vita nonce`; `G = elligator_map(BLAKE2b-256("NativSync CPace v1" ‖ sid ‖ code))`, the code
   as its six ASCII digits; each picks a random scalar and sends `X25519(scalar, G)`: PC first (`<A>`), then the Vita
   (`<B>`), 32 bytes each in hex. A point of all zeros is refused.
4. Both: `K = X25519(own scalar, other's point)` (refused if all zeros), `transcript = sid ‖ A ‖ B`,
   `k = BLAKE2b-512(key K, "NativSync pair" ‖ transcript)`: the first 32 bytes are the **pairing key**, the last 32 the
   confirmation key.
5. Vita: `<tag>` with `tag = BLAKE2b-256(key confirmation key, "vita" ‖ transcript)`. The PC checks it; on a mismatch
   the code was wrong (or someone is in the middle) and it closes.
6. PC: its own tag over `"pc" ‖ transcript`. The Vita checks it and answers `OK <console id> <console name>`, keeping
   the pairing key under the PC's id and name, or `ERR wrong code` and counts a try.

Each side keeps the pairing key until the pairing is removed (the Vita app or the launcher). One Vita can pair with
several PCs, and each pairing has its own base for every game.

## Connecting

1. PC: `NSYNC1 HELLO <pc id> <pc nonce>` (a fresh nonce each connection).
2. Vita: `NSYNC1 HELLO <vita nonce>`, or `NSYNC1 ERR unpaired` (no such PC) and it closes.
3. Both: `k = BLAKE2b-512(key pairing key, "NativSync session" ‖ pc nonce ‖ vita nonce)`; the first 32 bytes key what
   the PC sends, the last 32 what the Vita sends.

From here every message is a frame: a 4-byte big-endian length, then the 16-byte MAC and the ciphertext of
`crypto_aead_lock` (XChaCha20-Poly1305), whose 24-byte nonce is the sender's frame count (from 0, 8 bytes little-endian)
followed by zeros. A frame that doesn't open ends the connection. A frame holds at most 128 KiB of plain text (the
service lives in the shell, whose memory is small).

A request is a line (`<command> <args...>`), and for some commands a newline and a payload. A reply is `OK`, or
`OK\n<payload>`, or `ERR <reason>`. Replies carry JSON objects; requests never do (the Vita parses no JSON). The first
request is `HELLO`, which proves the PC holds the key.

## Manifests

A save's manifest names what the game keeps there, and its hash is what the sync compares:

- **files**: everything under the save's folder except `sce_sys` and `sce_pfs`, by path (`/` separated, no leading
  `/`), with size and BLAKE2b-256 hash. On the Vita that is the mounted save folder; in Nativ, the game's `savedata0`
  folder except its `.slot<N>` files.
- **slots**: each used slot's 0x34C-byte SceAppUtilSaveDataSlotParam with its BLAKE2b-256 hash. On the Vita they come
  from `sce_sys/sdslot.dat` (a used byte per slot at 0x200 + N, then 0x400-byte records from 0x400, the parameter
  first); in Nativ from the `.slot<N>` files.

The manifest hash is BLAKE2b-256 of its canonical text: for each file in byte order of path,
`F <path>\t<size>\t<hash>\n`, then for each slot in order of number, `S <n>\t<hash>\n`. An empty save has the hash of
the empty text.

## Requests

| Request | Reply |
|---|---|
| `HELLO <pc name>` | `{"protocol":1, "console":…, "firmware":"3.65", "account_id":<16 hex>, "running":[<title ids>], "free_kb":…}` |
| `LIST [<id>]` | `{"games":[{"id", "name", "version", "save":true/false, "base":<hash or null>, "guard":"none"/"sync"/"conflict"}]}`: every game with a save or installed, or just that one |
| `MANIFEST <id>` | `{"hash", "files":[{"path", "size", "hash", "mtime"}], "slots":[{"slot", "hash", "title", "subtitle", "detail", "time"}]}`; times are the Vita's local time, `YYYY-MM-DD HH:MM` |
| `GET <id> <path> <offset> <length>` | `OK\n` and the bytes (at most 128 KiB less that); `slot/<n>` is slot n's parameter |
| `PUT <id> <path> <offset> <total>` + bytes | `OK`: into the staging folder for that game; nothing is applied yet |
| `COMMIT <id> <hash>` + the manifest's canonical text | the text must hash to `<hash>`. Each file and slot in it comes from the staging folder, or must already be on the Vita as it is; files and slots missing from it are removed. The save is backed up first, everything is read back and checked after, and any failure puts the backup back. On success the manifest becomes the base: `{"backup":<name>}` |
| `BASE <id> <hash>` | records the base after a sync that sent nothing to the Vita |
| `GUARD <id>` + `key<TAB>value` lines: `state` (`none` or `conflict`), `game`, `pc_when`, `pc_detail`, `vita_when`, `vita_detail` | sets what the launch guard shows for the game |
| `RELEASE <id> <ok>` | answers a launch the Vita is holding for this PC (see Events) |
| `EVENTS <ms>` | waits up to that long for events: `{"events":[{"kind":"started"/"stopped"/"choice"/"waiting", "id", "choice":"vita"/"pc"/"later"}]}` |
| `BACKUPS <id>` / `RESTORE <id> <name>` | the Vita's backups of a save, and putting one back (the current save is backed up first) |

A save is never read or written while its game is running on the Vita: those requests answer `ERR busy`. The one
exception is a launch the Vita is holding for this PC (below), which is exactly when the PC is meant to write.

## Events

The PC keeps one `EVENTS` request waiting while it's connected. Events:

- `started` / `stopped`: a game started or stopped on the Vita. After `stopped`, the PC compares that game's save.
- `choice`: the player answered the conflict choice at launch. `vita` and `later` need nothing at once (`vita`: take
  the Vita's save once the game stops). `pc`: the launch is being held; the PC sends its save (`PUT`, `COMMIT`), and the
  commit releases the launch. If it can't, `RELEASE <id> 0` lets the game start with the Vita's save.
- `waiting`: a game was started while this PC is syncing it; the launch is held until the commit (or `RELEASE`).
