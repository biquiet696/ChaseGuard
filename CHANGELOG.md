ChaseGuard changelog
====================

1.0 - 2026-11-19  (first release; fork of the MWCrashFix build dated 2025-02-15)

  Fixed
  * A failed instruction check or a failed VirtualProtect no longer falls
    through to the success path. Previously the plugin showed the error box
    and then still logged "Patched offending memory references" and installed
    the exception handler. Now it shows the box and exits cleanly.
  * The exception handler no longer acts on exceptions that do not come from
    the game. It now requires the fault address (or, for calls through a bad
    pointer, the return address on the stack) to lie inside the game image
    (0x00401000-0x00FFFFFF). Everything else is passed on untouched with
    EXCEPTION_CONTINUE_SEARCH and is not logged. Previously any access
    violation in the whole process (drivers, other mods, code that catches
    its own faults) could be skipped or could terminate the game.
  * The "is the page executable" test accepted only PAGE_EXECUTE_READ. It now
    accepts every executable protection, so code pages left RWX by other ASI
    mods are no longer mistaken for non-executable memory.
  * The handler no longer decodes an instruction at a fault address that is
    not executable memory (this could raise a second fault inside the
    disassembler).
  * The register/stack dump written on an unrecoverable fault went to stderr,
    which a game does not have. It is now written to chaseguard.log.
  * If the log file cannot be created, the plugin no longer gives up without
    patching; the log is redirected to NUL and the patches are still applied.

  Changed
  * Renamed MWCrashFix -> ChaseGuard: file name, internal DLL name, log file
    (chaseguard.log), message box titles and the log prefix.

  Unchanged
  * The 434 patch sites, their verification bytes and the replacement value.
  * Game support: speed.exe v1.3 only.
