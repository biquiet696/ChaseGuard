<p align="center"><img src="assets/banner.png" alt="ChaseGuard" width="100%"></p>

# ChaseGuard

Pursuit crash protection for **Need for Speed: Most Wanted (2005), v1.3**: an ASI plugin that prevents the
`Debug Error: R6025 - pure virtual function call` crash in long pursuits and adds a careful crash-recovery layer.

ChaseGuard started as a fork of [MWCrashFix](https://github.com/x0reaxeax/MWCrashFix) by x0reaxeax
([release-1.2-veh-experimental](https://github.com/x0reaxeax/MWCrashFix/releases/tag/release-1.2-veh-experimental)).
Version 2.0 is a rewrite from source: the same patch table, a new exception handler.

> Like the original, ChaseGuard is provided *as is* - use at your own risk.

## Install

Requirements: NFS Most Wanted (2005) `speed.exe` **v1.3** and an ASI loader that loads `*.asi` from `scripts`
(for example [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll`).
No Visual C++ Redistributable is needed.

1. Delete `MWCrashFix.asi` from `<game folder>\scripts` if you have it. Do **not** keep both.
2. Copy `ChaseGuard.asi` and `ChaseGuard.ini` from the release zip into `<game folder>\scripts`.
3. Start the game. To uninstall, delete both files.

`chaseguard.log` is written to the game folder. It starts over when it is larger than `MaxLogKB` at game start.

## What it does

**1. The patch.** 434 instructions in `speed.exe` store `0x890970` into an object. That value is the virtual table
of the base class of the game's event-subscriber objects. An object carries it only while its constructor registers
it (`0x5D7930`) or its destructor unregisters it (`0x5D5A50`). If a virtual call reaches the object in that window,
the game dies with R6025. Like MWCrashFix, ChaseGuard stores `0x8AA828` instead, a table whose entries are harmless
accessors. Before the first byte is written, **all 434 locations are verified**. On an unknown or modified
`speed.exe` nothing is changed, and if writing fails halfway everything is rolled back.

**2. The exception handler.** It handles an access violation only when it can prove from the instructions that
the fault comes from game code, and only in these cases:

| Situation | What ChaseGuard does |
|---|---|
| A game instruction reads/writes through a broken pointer (`mov eax, [ecx+8]`, `ecx` dangling) | skips the instruction; a load clears the target register, so the game's own null checks take over |
| `call [bad pointer]` in game code | skips the call, the call "returns" `0` in `EAX` |
| A game `call` jumped into non-executable memory (corrupted function pointer) | returns to the caller with `EAX = 0`, but only if the bytes before the return address decode to a `CALL` whose operand really points to the faulting address |
| `push`/`pop`/`ret`/`jmp`/anything that changes `ESP` faults | **not** touched (skipping would corrupt the stack); the game crashes as without the plugin |
| A fault anywhere else (drivers, other mods, Windows) | **not** touched and not logged |
| A game `call` reaches a pure virtual function (`R6025`) | returns `0` to the caller instead of closing the game (`RecoverPureCalls`) |
| The same place faults more than `LoopLimit` times per second | stops recovering there, so the game crashes instead of freezing |

When ChaseGuard does not recover, it passes the exception on. Windows and other crash handlers see the original
crash, and `chaseguard.log` gets a register dump.

**3. Exit diagnostics.** Every way the game ends is logged with the game call stack: a normal quit (exit code 0),
any C runtime error box (`R6025`, `R6018`, ...; `speed.exe` uses the debug CRT, so these appear as "Microsoft
Visual C++ Debug Library - Debug Error!"), and unhandled crashes with the module they happened in (game, driver,
another mod). If the game closes itself without an error, the log names the path that asked it to quit
(menu, `WM_CLOSE` / Alt+F4, `WM_QUIT`) and the window events right before it.

## Settings (`ChaseGuard.ini`)

| Key | Default | Meaning |
|---|---|---|
| `EnablePatch` | 1 | apply the 434-location patch |
| `EnableHandler` | 1 | install the exception handler |
| `RecoverFaults` | 1 | skip game instructions that access a broken pointer |
| `RecoverBadCalls` | 1 | recover from calls through / into broken pointers |
| `RecoverPureCalls` | 1 | `R6025 pure virtual function call` from game code returns `0` instead of closing the game |
| `ZeroLoadedRegister` | 1 | clear the register a skipped load would have written |
| `LoopLimit` | 500 | recoveries per second at one address before giving up there |
| `MaxLogKB` | 1024 | log size limit |
| `SelfTest` | 0 | `1` = 10 s after start, provoke four harmless faults in game code and log PASS/FAIL |

## Reading the log

```
recovered #1 at 00402510: load from bad pointer skipped, register = 0 (skip-instruction read 0000001A, register cleared) [1 at this address]
Faulting address 0061234A in game code, not recovered: control-flow/stack instruction cannot be skipped (read 00000004)
```

```
pure virtual function call (R6025) from 0058A123 (speed.exe+18A123), this=0A3B1C40 vtable=00890970: returned 0 to the caller [1 at this address]
game exits with code 3 after R6025 pure virtual function call
UNHANDLED EXCEPTION C0000005 at 6A2B1F00 (amdxx32.dll+1F00), read 00000010 - the game crashes here
```

`recovered` / `returned 0` lines mean a crash was prevented. `not recovered`, `game exits with code ...` and
`UNHANDLED EXCEPTION` lines are followed by registers and the game call stack. Please attach `chaseguard.log` to bug
reports.

## Known limitations

- `speed.exe` v1.3 only.
- After a recovered *call*, the caller gets `0` instead of a real result. Arguments that a `__thiscall`/`__stdcall`
  callee would have removed stay on the stack. The game usually copes; this is the same trade-off as in MWCrashFix,
  and the reason ChaseGuard only does it for verified call sites.
- A skipped x87 load (`fld [bad]`) leaves the FPU stack one entry short, as in MWCrashFix.
- Recovery keeps the game running, it does not make broken game state correct. If a career save matters to you,
  keep a backup.

## Building

On Linux (or WSL) with LLVM. No Windows SDK, no MSVC and no C runtime are needed:

```sh
sudo apt install clang lld llvm
./build.sh                       # -> build/ChaseGuard.asi
tests/run_tests.sh /path/to/speed.exe   # decision-logic tests against the real game code
```

The build is reproducible: the same sources produce the same `ChaseGuard.asi` byte for byte. GitHub Actions builds
every push (`.github/workflows/build.yml`).

| Path | Content |
|---|---|
| `src/chaseguard.c` | entry point, patcher, exception handler, log, settings, self-test |
| `src/core.c` | platform-independent decision logic (also compiled into the tests) |
| `src/sites.inc` | the 434 patch locations, generated by `tools/gen_sites.py` from MWCrashFix |
| `tests/` | 57 checks on real `speed.exe` code, plus a decoder cross-check over all 1.8 M instructions of `.text` |
| `third_party/zydis/` | Zydis 4.1 instruction decoder (MIT) |

## Credits

- **MWCrashFix**: original discovery, patch table and the idea of the exception handler, by
  [x0reaxeax](https://github.com/x0reaxeax/MWCrashFix) ([NFSMods page](https://nfsmods.xyz/mod/5970))
- Related: [mwfixes](https://github.com/GrimMaple/mwfixes) by GrimMaple
- [Zydis](https://github.com/zyantific/zydis) by zyantific (MIT License, see `third_party/zydis/LICENSE`)

## License

[GPL-3.0](LICENSE), like the original project. This is a modified version of MWCrashFix (first modified release
2026-11-19, rewritten 2026-11-25). Not affiliated with or endorsed by Electronic Arts. "Need for Speed" is a
trademark of Electronic Arts Inc.
