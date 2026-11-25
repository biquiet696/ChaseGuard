ChaseGuard changelog
====================

2.0.0 - 2026-11-25  (rewrite from source)

  The plugin is now built from C source with LLVM instead of binary-patching the
  MWCrashFix build. The patch table and both values (0x890970 -> 0x8AA828) are
  unchanged.

  Patch
  * All 434 locations are verified before anything is written. An unknown or
    modified speed.exe is left untouched (1.x could patch part of the table and
    then stop), and a failed write rolls back everything already written.
  * Detects a speed.exe that is already patched (e.g. MWCrashFix loaded first)
    and warns when MWCrashFix.asi is installed as well.

  Exception handler
  * The game code range is read from speed.exe's PE header (.text,
    0x401000-0x88F2A5) instead of the hard-coded 0x401000-0xFFFFFF, which also
    covered game data and memory outside the image.
  * "Return from a bad call" now requires proof: an execute fault exactly at
    EIP, a return address in .text, a CALL right before it, and that CALL's
    operand must really point to EIP. Before, any return-like value on the stack
    was accepted, or the game was closed.
  * Instructions that change control flow or ESP (push, pop, ret, jmp, ...) are
    never skipped.
  * "call [bad]" is skipped as a whole and returns 0 in EAX.
  * A skipped load ("mov reg, [bad]") clears the destination register
    (ZeroLoadedRegister), so the game's own null checks take over instead of
    using a stale value.
  * Loop protection: if one address needs recovery more than LoopLimit times per
    second, ChaseGuard stops recovering there instead of freezing the game.
  * Unrecoverable faults are passed on (EXCEPTION_CONTINUE_SEARCH) instead of
    calling exit() inside the handler, so the game crashes normally and other
    crash handlers still work.
  * Faults inside the handler itself are never handled again (no recursion).
  * Thread-safe (spin lock around the handler).

  Other
  * No C runtime: imports only kernel32 and user32; the Visual C++
    Redistributable is no longer required.
  * ChaseGuard.ini with all settings; SelfTest=1 provokes three harmless faults
    in game code and logs PASS/FAIL.
  * chaseguard.log is always written to the game folder (not the current
    directory), starts over above MaxLogKB, timestamps every line and ends with
    a session summary.
  * Version resource, Zydis license included, reproducible build, tests against
    the real game code, GitHub Actions build.

1.0 - 2026-11-19  (first release; fork of the MWCrashFix build dated 2025-02-15)

  Fixed
  * A failed instruction check or a failed VirtualProtect no longer falls
    through to the success path.
  * The exception handler only acts on faults whose address (or return
    address) is in 0x00401000-0x00FFFFFF.
  * The "is the page executable" test accepts every executable protection.
  * No instruction decoding at a non-executable fault address.
  * The register dump goes to chaseguard.log instead of stderr.
  * If the log file cannot be created, the patches are still applied.

  Changed
  * Renamed MWCrashFix -> ChaseGuard.
