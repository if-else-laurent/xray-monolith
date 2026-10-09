# gamma-may-mt-cform-fix

Notes on this branch: what it is built from, what it changes, how it is built
and installed, and what is known but not fixed. The upstream README and its
changelog are left untouched so that merges from upstream stay clean; anything
specific to this branch goes here.

## What the branch is

The multithreaded (MT) engine source of 15 May 2026, the build that ships with
G.A.M.M.A. 0.9.5 (merge `3d4dcca5` of `all-in-one-vs2022-wpo` into
`all-in-one-vs2022-wpo-mt`), plus the fixes listed below. Only the DX11
executable is built.

| Remote | Repository | Role |
|---|---|---|
| `origin` | `themrdemonized/xray-monolith` | upstream, main branch `all-in-one-vs2022-wpo` |
| `fork` | `if-else-laurent/xray-monolith` | where this branch is pushed and built |

The engine is used on macOS through CrossOver (Wine with D3DMetal), which is
where two of the three fixes come from.

## Changes on top of the May 15 MT source

### 1. CFORM arrays are published before the async tree build

Commit `8639f35f`, files `src/xrCDB/xrCDB.cpp`, `xrCDB.h`, `xr_area.cpp`.

`CObjectSpace::Load` had moved the whole CFORM build to a background task, so
`GetStaticVerts()` and `GetStaticTris()` returned null until it finished, and
callers that read the arrays without a preceding CDB query crashed. The build
is split in two: `build_arrays` runs on the loading thread, only `build_tree`
is queued. The commit message has the details, including the `status` field
widened from a bool to a three-valued atomic.

### 2. The process is terminated at the end of WinMain

Commit `0913c773`, file `src/xrEngine/x_ray.cpp`.

After a normal quit the process never exited under CrossOver. A sample of the
hung process showed the main thread in an infinite `NtWaitForSingleObject` and
all 16 PPL worker threads blocked inside Wine's `NtFlushProcessWriteBuffers`
(in `thread_get_state`), at zero CPU. By then the level is unloaded,
`user.ltx` is saved and the log is closed, so `WinMain` now ends with
`TerminateProcess(GetCurrentProcess(), 0)` instead of running the runtime
teardown. The call sits after the block that restores the sticky keys
settings, launches the "on exit" application and releases the single instance
mutex.

Verified in game on 2026-10-07: the process exits at once.

### 3. A close request during the game opens the main menu

Commit `0f556b73`, file `src/xrEngine/Device_wndproc.cpp`.

`WM_CLOSE` queued `KERNEL:disconnect` and `KERNEL:quit` unconditionally. Under
CrossOver the game window received it when the game was minimized, and the
session ended without asking; with fix 2 in place that looked like a crash
with no error in the log. Now, while a level is loaded and the main menu is
not open, `WM_CLOSE` opens the main menu and writes
`* Close request during the game, opening the main menu instead of quitting`
to the log. With the main menu open, or with no level loaded, it quits as
before.

Not verified in game yet. What sends the close request on minimize is not
known; this only stops it from ending the session.

### 4. A crash is reported before anything that can hang, and a hung report ends the process

File `src/xrCore/xrDebugNew.cpp`.

On 2026-10-08 the game froze for good in the middle of a session. The log
ended with `stack trace:`, the two StackWalker lines `SymInit:` and
`OS-Version:`, and nothing else: an unhandled exception had reached
`UnhandledFilter`, which started the stack walk first, and the walk never
returned from dbghelp. No address, no stack and no minidump were written,
because all of that came after the walk. Earlier crashes on 4 and 5 October did
get their minidumps, so the walk does not hang every time.

Three changes:

- `log_exception_record` runs first and uses no dbghelp. It writes the
  exception code, the address, the module with its base and the offset inside
  it, and for an access violation whether it was a read or a write and of what
  address:
  `! Unhandled exception 0xC0000005 at address 0x..., thread ...`,
  `! Module ...\AnomalyDX11.exe, base 0x..., offset 0x...`,
  `! Access violation reading address 0x...`.
  The offset is what to look up in `AnomalyDX11.pdb` if nothing else survives.
- The minidump is written before the stack walk instead of after it.
- A watchdog thread (`crash_report_begin` / `crash_report_end`) covers the
  minidump and the walk. If they are not done in 60 seconds it writes
  `! Crash report did not finish in 60 seconds, terminating the process` and
  calls `TerminateProcess`, so a hung report ends as a closed game and not as a
  dead window. The same watchdog covers the stack walk of a `FATAL ERROR`
  (`xrDebug::gather_info`), which goes through the same code.

The stack that `LogStackTrace` prints is that of the handler itself, not of
the faulting code, as before; this change does not touch that. The minidump
carries the real context.

Not built and not verified: written on macOS, where the project does not
build. It has to pass the workflow build first.

### 5. `run_string` keeps the case of its argument and takes a long string

`src/xrGame/console_commands.cpp`, `src/xrServerEntities/script_thread.cpp`

The console lowercased the argument of every command, `run_string` included,
so `run_string printf(SIMBOARD ~= nil)` ran as `printf(simboard ~= nil)`:
any Lua name with a capital letter was out of reach. `CCC_ScriptCommand` now
sets `bLowerCaseArgs = false`. The string is wrapped into a function in a
buffer that was 256 bytes; it is 4096 now.

Not changed: strings queued in the same frame (typed while the game is
paused) all define the same wrapper function before any of them runs, so the
last one runs once for each. Enter them one at a time in a running game.

## Known and not fixed

- **PPL workers can deadlock under Wine on macOS.** Fix 2 avoids the deadlock
  at shutdown only. The same mutual block of worker threads in
  `NtFlushProcessWriteBuffers` is possible in the middle of a game, as a freeze
  without a crash. It has not been observed so far. To check a frozen process:
  `sample <pid> 2 -file out.txt` and look for the worker threads sitting in
  `NtFlushProcessWriteBuffers`. The frames of the game code itself are not
  readable in such a sample because of Rosetta.
- **`alife():teleport_object` and objects without AI locations.** In
  `CALifeGraphRegistry::add` the level registry is updated while the object
  still carries its old game vertex, so an object teleported from another
  level is not added to the current level until the save is loaded again.
  Scripts work around it by calling `teleport_object` twice. Not changed here.

### 6. Script sandbox

Files `src/xrServerEntities/script_storage.cpp`, `script_sandbox.h`,
`script_ini_file.cpp`, `src/xrGame/fs_registrator_script.cpp`,
`src/Layers/xrRenderDX10/dx10ResourceManager_Scripting.cpp`, LuaJIT
(`lauxlib.h`, `lib_aux.c`, `lib_io.c`, `lj_load.c`) and `lfs.c`.

A script of the game or of a mod had every file the player can reach: `io` is
open, and under Wine the bottle maps the whole disk of the host (`Z:` is `/`).
The modpack runs the scripts of some six hundred mods. Scripts are now
confined to the game:

- they read under `$fs_root$` only;
- they write under `$app_data_root$` and `$game_data$` only, so that they
  cannot replace the executables, the libraries or `commandline.txt` and come
  back unconfined at the next start;
- `ffi`, `package.loadlib` and the loaders of C modules are taken away: each
  runs native code, which no rule about paths can hold.

The rule is one function, `script_path_allowed`. LuaJIT asks it through a
guard (`luaL_setpathguard`) in `io.open`, `io.lines`, `io.input`, `io.output`,
`loadfile`, `dofile` and `require`; `lfs` asks the same guard; the engine asks
it in the file functions of `getFS()` given to scripts (`r_open`, `w_open`,
`file_delete`, `dir_delete`, `file_rename`, `file_copy`) and in `ini_file`
(a refused file reads as empty, `save_as` returns false). A refused `io.open`
returns `nil, "<path>: Permission denied"`, as for any file that cannot be
opened. Paths are compared absolute, without `..`, in lower case.

Command line (read from `commandline.txt`, which scripts cannot write):
`-lua_sandbox_audit` lets everything through and logs what would have been
refused, `-lua_sandbox_off` switches the sandbox off. The log has one line at
start, `* Script sandbox: ...`, and a line `! [script sandbox] ...` for each
of the first 200 refusals.

Known gaps:

- Bytecode is still loaded. Saves keep functions as bytecode (`lmarshal.c`),
  so it cannot simply be refused, and a crafted chunk can break out of LuaJIT.
  This needs an exploit written for this LuaJIT, not one line of Lua.
- Console commands that take a file name (`cfg_save`, `cfg_load`) are not
  checked; scripts can run console commands.
- `getFS():append_path` can still move a root alias.
- With `lua_debug 1` the sockets are opened for the debugger; a script can
  then talk to the network, though only about what it can read.

Not verified in game yet.

## Building

Workflow `.github/workflows/gamma-mt-fix.yml` ("GAMMA MT fix build"), based on
the `build_mt` job of `msbuild.yml`. It runs on every push to this branch and
can be started by hand. Every push starts a full build, a documentation-only
one included.

Artifacts, kept for 7 days:

| Artifact | Content |
|---|---|
| `DX11_mt_exe` | `AnomalyDX11.exe` |
| `DX11_mt_pdb` | `STALKER-Anomaly-modded-exes_DX11_mt_pdb.zip`, which holds `AnomalyDX11.pdb` |

The project does not build on macOS.

## Installing a build

Game folder of this setup:
`~/Library/Application Support/CrossOver/Bottles/Steam/drive_d/ANOMALY - GAMMA/ANOMALY/bin`.

1. Close the game.
2. Keep the current files next to the new ones as
   `AnomalyDX11.exe.bak-<date>` and `AnomalyDX11.pdb.bak-<date>`.
3. Put `AnomalyDX11.exe` from `DX11_mt_exe` and `AnomalyDX11.pdb` from the
   inner archive of `DX11_mt_pdb` into the folder. The pdb is read only when
   the game crashes, but a stale one makes the stack in the log wrong.

Installed builds so far:

| Date | Commit | Notes |
|---|---|---|
| 2026-10-05 | `6f9b2413` | first build of the branch, exe md5 `6bec99a5…` |
| 2026-10-07 | `0913c773` | exit fix, exe md5 `5a2edd8c…` |

The game log starts with the build date of the executable
(`'xrCore' build …`, `Modded Exes MT-TEST version …`), which tells which
build a log comes from.
