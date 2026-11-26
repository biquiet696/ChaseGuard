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

**1. What causes R6025.** `0x890970` is the virtual table of the game's interface base class (EA's COM-style
interfaces). The constructor `0x401F10` registers an interface with its owner object as a `{type, pointer}` pair
(`0x5D7930`); every destructor stores `0x890970` back and unregisters it (`0x5D5A50`). That table has a single
slot, the destructor: slots 1-8 are floats and slot 9 onwards is `_purecall`. So R6025 is a **virtual call on an
interface that was already destroyed**: some game code keeps a pointer to it after its owner deleted it.

**2. The patch.** 434 instructions in `speed.exe` store `0x890970`. MWCrashFix stores `0x8AA828` instead, the table
of an unrelated class: the stale call then runs a real method of that class on freed memory (some of them write into
it). ChaseGuard stores **its own table** (`PatchMode=2`): slot 0 is the game's real destructor, every other slot logs
the call and returns `0` without touching the dead object, removes the callee's arguments and pushes `0.0` when the
caller expects a float. `PatchMode=0` is exactly MWCrashFix. Before the first byte is written, **all 434 locations are
verified**. On an unknown or modified `speed.exe` nothing is changed, and if writing fails halfway everything is
rolled back.

**3. The registry fix.** The game's unregister function is `v.erase(std::remove(v.begin(), v.end(), x))` with the
one-argument `erase`: an interface listed twice leaves a dangling pointer in its owner, one that is not listed makes
the copy loop overwrite the heap past the end of the vector. ChaseGuard replaces it with a correct version and logs
both cases. It also remembers the last 1024 destroyed interfaces, so every call on a dead one is logged with what
it was: type, owner, the destructor that removed it, how long ago, and whether the owner still lists it.

**4. The exception handler.** It handles an access violation only when it can prove from the instructions that
the fault comes from game code, and only in these cases:

| Situation | What ChaseGuard does |
|---|---|
| A game instruction reads/writes through a broken pointer (`mov eax, [ecx+8]`, `ecx` dangling) | skips the instruction; a load clears the target register, so the game's own null checks take over |
| `call [bad pointer]` in game code | skips the call, the call "returns" `0` in `EAX`, its arguments are removed |
| A game `call` jumped into non-executable memory (corrupted function pointer) | returns to the caller with `EAX = 0`, but only if the bytes before the return address decode to a `CALL` whose operand really points to the faulting address |
| `push`/`pop`/`ret`/`jmp`/anything that changes `ESP` faults | **not** touched (skipping would corrupt the stack); the game crashes as without the plugin |
| A fault anywhere else (drivers, other mods, Windows) | **not** touched and not logged |
| A game `call` reaches a pure virtual function (`R6025`) | returns `0` to the caller instead of closing the game (`RecoverPureCalls`), its arguments are removed |
| The same place faults more than `LoopLimit` times per second | stops recovering there, so the game crashes instead of freezing |

When ChaseGuard does not recover, it passes the exception on. Windows and other crash handlers see the original
crash, and `chaseguard.log` gets a register dump.

**5. Exit diagnostics.** Every way the game ends is logged with the game call stack: a normal quit (exit code 0),
any C runtime error box (`R6025`, `R6018`, ...; `speed.exe` uses the debug CRT, so these appear as "Microsoft
Visual C++ Debug Library - Debug Error!"), and unhandled crashes with the module they happened in (game, driver,
another mod). If the game closes itself without an error, the log names the path that asked it to quit
(menu, `WM_CLOSE` / Alt+F4, `WM_QUIT`) and the window events right before it.

## Settings (`ChaseGuard.ini`)

| Key | Default | Meaning |
|---|---|---|
| `EnablePatch` | 1 | apply the 434-location patch |
| `PatchMode` | 2 | `2` = ChaseGuard table, calls on destroyed interfaces return `0`; `1` = ChaseGuard table, forwarded to the MWCrashFix method; `0` = MWCrashFix table `0x8AA828`, no logging |
| `FixInterfaceRegistry` | 1 | replace the game's broken interface unregister function |
| `EnableHandler` | 1 | install the exception handler |
| `RecoverFaults` | 1 | skip game instructions that access a broken pointer |
| `RecoverBadCalls` | 1 | recover from calls through / into broken pointers |
| `RecoverPureCalls` | 1 | `R6025 pure virtual function call` from game code returns `0` instead of closing the game |
| `CorrectStack` | 1 | after a recovered call, also remove the arguments the called function would have removed |
| `ZeroLoadedRegister` | 1 | clear the register a skipped load would have written |
| `LoopLimit` | 500 | recoveries per second at one address before giving up there |
| `MaxLogKB` | 1024 | log size limit |
| `SelfTest` | 0 | `1` = 10 s after start, provoke five harmless faults in game code and log PASS/FAIL, including an exact stack-balance check |

## Reading the log

```
recovered #1 at 00402510: load from bad pointer skipped, register = 0 (skip-instruction read 0000001A, register cleared) [1 at this address]
Faulting address 0061234A in game code, not recovered: control-flow/stack instruction cannot be skipped (read 00000004)
```

```
call on a destroyed interface: slot 9 (+24) from 004FA2AD (speed.exe+FA2AD), this=0A3B1C40: returned 0, arguments removed [1 at this address]
  object 0A3B1C40 was interface type 6C1E2A10 of owner 0A3B1000, destroyed by the code at 0041A2B7 1834 ms ago; the owner no longer lists it, so the caller kept its own copy of the pointer
interface registry: interface was listed more than once (the original code would have left a dangling pointer in the owner) (owner 0A3B1000, interface 0A3B1C40, called from 0041A2B7) [1 so far]
pure virtual function call (R6025) from 0058A123 (speed.exe+18A123), this=0A3B1C40 vtable=00890970: returned 0 to the caller [1 at this address]
game exits with code 3 after R6025 pure virtual function call
UNHANDLED EXCEPTION C0000005 at 6A2B1F00 (amdxx32.dll+1F00), read 00000010 - the game crashes here
```

`recovered` / `returned 0` lines mean a crash was prevented. `not recovered`, `game exits with code ...` and
`UNHANDLED EXCEPTION` lines are followed by registers and the game call stack. Please attach `chaseguard.log` to bug
reports.

## Known limitations

- `speed.exe` v1.3 only.
- After a recovered *call*, the caller gets `0` instead of a real result.
- Stack correction (`CorrectStack`): a `__thiscall`/`__stdcall` function removes its own arguments, so after
  skipping it ChaseGuard has to remove them too. Their size is worked out from the calling code (the argument
  pushes before the call, prologue and nested calls taken into account). Measured against all 22,627 direct
  callee-cleaned calls in `speed.exe` it is exact for 88.7 %; for the rest the stack can still be off. MWCrashFix
  and ChaseGuard 2.0 never corrected it.
- A skipped x87 load (`fld [bad]`) leaves the FPU stack one entry short, as in MWCrashFix.
- `PatchMode=2` stops the dead object from being used, it does not find the code that keeps the stale pointer by
  itself. The log does: the `object ... was interface type ...` lines name the destructor and the call site.
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
| `tests/` | 69 checks on real `speed.exe` code, a decoder cross-check over all 1.8 M instructions of `.text` and an argument-size check over all 22,627 callee-cleaned direct calls |
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
