## ChaseGuard 1.0

First release – fork of the MWCrashFix build with the exception handler (2025-02-15).

**Fixed**
- Failed instruction check / `VirtualProtect` now exits instead of falling through to the success path.
- Exception handler ignores faults that do not originate in the game image.
- Page-protection test accepts all executable protections.
- No instruction decoding at non-executable fault addresses.
- Crash dump goes to `chaseguard.log` instead of `stderr`.
- Unwritable log no longer disables the patches.

**Changed**
- Renamed MWCrashFix → ChaseGuard.

**Install:** download `ChaseGuard_v1.0.zip`, copy `scripts/ChaseGuard.asi` into `<game folder>/scripts`. Remove any old `MWCrashFix.asi`.

SHA-256 of `ChaseGuard.asi`: `d462c1b6e40d63f45212aad5275cbbe0179e1fa9c913aaa259cab12a767a4a77`
