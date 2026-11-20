# Trying things on a Vita

`guardtest` stands in for the background service, to try the launch guard without a computer. Load
`nsync_kernel.skprx` (the app does, or taiHEN's `*KERNEL`), then `guardtest.suprx` into SceShell (any loader for
shell modules), and write a scenario to `ux0:data/nativ-sync/test/scenario.txt`:

    <seq> <conflict|sync|none> <TITLE ID> <answer after ms> <ok 0|1> <pc online 0|1>

for example `1 conflict PCSE00640 2000 1 1`. guardtest picks up a changed file within a second, sets that guard with
sample save details, answers the guard's events as told, and logs to `ux0:data/nativ-sync/test/guardtest.txt`. Then
start the game from the home screen.

What each should do:

| Scenario | Expected |
|---|---|
| `conflict ... 1` and □ | "Getting your save from Test PC..." until the answer, then the game |
| `conflict ...` and ✕ or ○ | the game at once |
| `conflict ... 0` (PC offline) | □ greyed out and ignored |
| `sync ... 300 1` | nothing over the splash, the game starts |
| `sync ... 3000 0` | progress after a second, a note for 4 seconds, then the game |

The screens themselves can be previewed on a computer: `tools/preview.c` (its comment has the command).
