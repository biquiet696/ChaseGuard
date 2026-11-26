ChaseGuard changelog
====================

2.2.0 - 2026-11-26

  The cause of R6025, and a fix that does not run foreign code

  * What R6025 really is. 0x890970 is the vtable of the game's interface base
    class (EA's COM-style interfaces): its constructor registers the interface
    with its owner object as a {type, pointer} pair, every destructor stores
    0x890970 back and unregisters it. The table has one slot (the destructor);
    slots 1-8 are floats and slot 9 onwards is _purecall. So R6025 is a
    virtual call on an interface that was already destroyed: some game code
    keeps a pointer after the owner deleted the object. It is not a window
    during construction, as earlier notes said.
  * PatchMode (new, default 2). MWCrashFix stores 0x8AA828, the table of an
    unrelated class, so a stale call runs a real method of that class on freed
    memory; slot 9 is a full function and others write into the object
    (byte flags at +0xB1/+0xC8), which can corrupt whatever reuses that
    memory. With PatchMode=2 the 434 sites store ChaseGuard's own table:
    slot 0 is the game's real destructor (as unpatched), every other slot logs
    the call and returns 0 without touching the object. The callee's arguments
    are removed (the same analysis as CorrectStack) and, if the caller uses a
    float result, 0.0 is pushed so the FPU stack stays balanced (5 % of the
    game's virtual calls return a float). When the argument size cannot be
    worked out, that one call goes to the MWCrashFix method instead.
    PatchMode=1 logs and always forwards, PatchMode=0 is plain MWCrashFix.
  * Interface registry fix (FixInterfaceRegistry=1). The game's unregister
    function 0x5D5A50 is v.erase(std::remove(v.begin(), v.end(), x)), the
    one-argument erase: an interface listed twice leaves one dangling pair in
    its owner (which hands out the dead pointer later); one that is not listed
    makes the copy loop run past the end of the vector and overwrite the heap.
    It is replaced with a correct version; both cases and re-registration of a
    listed interface are logged.
  * Root-cause log. Every unregistered interface is remembered (last 1024).
    A call on a destroyed interface, or an R6025, now logs what the object
    was: interface type, owner, the destructor that removed it, how long ago,
    and whether the owner still lists it (dangling registry entry) or the
    caller kept its own copy of the pointer. Send chaseguard.log after a long
    pursuit: these lines name the code that keeps the stale pointer.
  * SelfTest has a fifth test: a game call through ChaseGuard's table,
    checked for the return value and an exactly balanced stack.
  * Tests: float-result detection, registry removal (80 checks).

2.1.0 - 2026-11-26

  Added
  * Stack correction after recovered calls (CorrectStack=1). Game functions
    mostly use __thiscall/__stdcall: the called function removes its own
    arguments. When ChaseGuard skips such a call (call through a bad pointer,
    call into bad code, R6025), it now removes those arguments as well, so the
    caller continues with a correct ESP. Before (and in MWCrashFix) the stack
    stayed off by the argument size, which in frame-pointer-less game code
    usually ends in a crash a few instructions later.
    The size is worked out from the caller's code: the function is decoded
    from its start, and the argument pushes of the call are counted with the
    prologue (saved registers, SEH frame, locals), structures passed by value
    and nested calls taken into account; a call followed by "add esp, N" is
    __cdecl and needs nothing. Exact for 88.7 % of the 22,627 direct
    callee-cleaned calls in speed.exe (checked in tests/).
  * The log shows how many argument bytes were removed for each recovery.
  * SelfTest checks the stack balance exactly (entering game code with the
    same stack its own function would have).

2.0.2 - 2026-11-26

  Added
  * Quit diagnostics: the five places in speed.exe that end the game loop
    (main-menu Quit, two menu message handlers, WM_CLOSE, WM_QUIT) are watched.
    The log says which one closed the game, with the game call stack and the
    last window events before it (focus changes, Alt+F4, SC_CLOSE, WM_CLOSE and
    whether another program sent it). A "silent" close in the middle of a race
    can now be told apart from a menu quit or a crash.

2.0.1 - 2026-11-26

  Added
  * R6025 handling: ChaseGuard installs itself as the pure-call handler of the
    game's C runtime (the _set_purecall_handler slot). A pure virtual call made
    by game code returns 0 to its caller instead of closing the game
    (RecoverPureCalls=1); every one is logged with its call site and vtable.
  * Exit diagnostics: every exit is logged with its exit code and the game call
    stack (normal quit, C runtime error boxes R60xx, unhandled crashes with the
    module they happened in). Before, a crash and a normal quit looked the same
    in the log.
  * SelfTest covers the pure-call path (4 tests).

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
