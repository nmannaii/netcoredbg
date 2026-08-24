# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

NetCoreDbg — a .NET Core debugger (C++ native core + small C# managed helper). It drives the debuggee through the CoreCLR **ICorDebug** unmanaged debugging API (via `dbgshim`) and exposes three front-end protocols: GDB/MI, VSCode Debug Adapter Protocol, and an interactive CLI.

## Build

Requires clang (gcc will not work), cmake, and the .NET SDK. CoreCLR sources and the .NET SDK are auto-downloaded by `fetchdeps.cmake` into `.coreclr/` and `.dotnet/` unless `-DCORECLR_DIR=` / `-DDOTNET_DIR=` are given.

```sh
mkdir build && cd build
CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin
make && make install          # installs into ./bin — the tests expect it there
```

Full clean: `rm -rf build src/debug/netcoredbg/bin bin` (there is no uninstall target).

Common cmake options:

| Option | Effect |
|---|---|
| `-DCMAKE_INSTALL_PREFIX=$PWD/../bin` | required for the test-suite |
| `-DINTEROP_DEBUGGING=1` | mixed native/managed mode (Linux/Tizen only; needs `libunwind-dev`) |
| `-DBUILD_TESTING=ON` | build Catch2 unit tests (`make test`) |
| `-DCMAKE_BUILD_TYPE=Debug\|Release` | default is Release |
| `-DBUILD_MANAGED=OFF` | skip ManagedPart.dll (C# side) |
| `-DCLR_CMAKE_ENABLE_CODE_COVERAGE` | needed for `run_tests.sh -c` |
| `-DASAN=1`, `-DCMAKE_CXX_CLANG_TIDY=clang-tidy-N` | sanitizer / static analysis |
| `-DCORECLR_BRANCH=`, `-DDOTNET_CHANNEL=` | which runtime sources/SDK to fetch (default `release/10.0` / `10.0`) |

Windows: `cmake .. -G "Visual Studio 16 2019"` then `cmake --build . --target install` (run from `cmd.exe`, not cygwin).

## Tests

Two independent suites.

**Unit tests** (`src/unittests/`, Catch2, utility code only — streams, iosystem, span, string_view, escaped_string):
```sh
cd build && make test
./src/unittests/iosystem          # run one test binary directly for details
```

**Integration suite** (`test-suite/`, C#, needs .NET SDK 3.1 targets and an installed `./bin/netcoredbg`):
```sh
cd test-suite
./run_tests.sh                      # all tests
./run_tests.sh MITestBreakpoint     # single test (name = directory name)
NETCOREDBG=/path/to/netcoredbg ./run_tests.sh MITestBreak
./run_tests.sh -c <test>            # with coverage (needs the coverage build option)
TIMEOUT=300 ./run_tests.sh <test>   # per-test timeout, default 150s
```
Windows: `powershell.exe -executionpolicy bypass -File run_tests.ps1 [<test>...]`. On-device/Tizen: `sdb_run_tests.sh`.

Adding a test: create a `dotnet new console --framework netcoreapp3.1` project in `test-suite/`, reference `NetcoreDbgTest/NetcoreDbgTest.csproj`, `dotnet sln add` it, **and add its name to `ALL_TEST_NAMES` in all four of** `run_tests.sh`, `run_tests.ps1`, `sdb_run_tests.sh`, `sdb_run_tests.ps1` — a test not listed there never runs.

### How integration tests work

Each test is a normal C# program that is *both* the debuggee and the test script. Inline markers drive the run:
- `Label.Breakpoint("name")` — marks a source line; the harness resolves it to file:line.
- `Label.Checkpoint("from", "to", (ctx) => {...})` — a lambda executed by the *debugger side* when the debuggee reaches that point; it issues protocol requests and asserts responses.

`NetcoreDbgTest/ControlScript.cs` parses the test's syntax trees with Roslyn, collects the labels, and compiles the checkpoint bodies into a driver script. `TestRunner` launches `netcoredbg` (`--local` or `--tcp`) with `--proto mi|vscode` and runs that script; `NetcoreDbgTest/MI/` and `NetcoreDbgTest/VSCode/` implement the client sides. Test-name prefix selects the protocol: `MITest*` → MI, `VSCodeTest*` → VSCode, `CLITest*` → driven by `run_cli_test.sh` feeding `commands.txt` and diffing output. `CLITestInterop*` tests additionally compile a native `.so` with clang and are skipped unless the build has interop enabled.

## Architecture

```
main.cpp ── picks protocol ──▶ IProtocol (protocols/) ──▶ IDebugger (ManagedDebugger)
                                    ▲                              │
                                    └──── events (Emit*Event) ─────┘
                                                                   ▼
                                             ICorDebug / dbgshim ─ debuggee process
```

**`src/interfaces/`** — the seam. `idebugger.h` is everything a protocol can ask the debugger to do; `iprotocol.h` is everything the debugger can report back (`EmitStoppedEvent`, `EmitBreakpointEvent`, …). `types.h` holds the protocol-neutral value types. New debugger features almost always mean touching both interfaces plus all three protocol implementations.

**`src/protocols/`** — `miprotocol.cpp` and `vscodeprotocol.cpp` each dispatch via a `static std::unordered_map<std::string, CommandCallback> commands` near the bottom of the file; `cliprotocol.cpp` uses the `constexpr CommandInfo commands_list[]` table in `CLIProtocol::CommandsList` (also feeds `help` and tab-completion). Adding a command = one entry in that table plus the handler.

**`src/debugger/`** — the core. `ManagedDebugger` (`manageddebugger.h`) is a facade split into `ManagedDebuggerBase` (owns all the sub-components) / `ManagedDebuggerHelpers` (friend of the callback classes) / `ManagedDebugger` (implements `IDebugger`). Sub-components:
- `ManagedCallback` — implements `ICorDebugManagedCallback*`, called on CoreCLR's own threads. It does almost no work: it pushes into `CallbacksQueue`, which serializes events on its own worker thread and decides when to call `ICorDebugController::Continue`. **This queue is the concurrency backbone — read `callbacksqueue.h`'s header comment before touching event handling.**
- `Breakpoints` (`breakpoints.h`) — an aggregator over one class per breakpoint kind: `breakpoints_line`, `breakpoints_func`, `breakpoints_exception`, `breakpoint_break` (`Debugger.Break()`), `breakpoint_entry`, `breakpoint_hotreload`, plus `breakpoints_interop*` for native ones.
- `Steppers` — `stepper_simple` (ICorDebugStepper) and `stepper_async`. Async methods are compiled to state machines, so stepping there uses yield/resume-offset breakpoints and `ObjectIdForDebugger` instead of the stepper; see `docs/stepping.md`.
- Evaluation stack: `Evaluator` → `EvalStackMachine` (expression parsing/eval) → `EvalHelpers` (func-eval) → `EvalWaiter` (runs the func-eval and waits with cancellation). `Variables`/`valueprint` format results.
- `interop_*.cpp`, `sigaction.cpp` — the ptrace-based native side, only compiled with `INTEROP_DEBUGGING`.

**`src/metadata/`** — `Modules` (loaded module registry), `modules_sources` (source-file ↔ IL-offset resolution), `jmc` (Just My Code), `async_info`, `typeprinter`, `interop_libraries` (ELF/DWARF via `third_party/libelfin`).

**`src/managed/`** — `ManagedPart.dll`, the C# helper the native side calls through `managed/interop.cpp`: `SymbolReader.cs` reads portable PDBs, `Evaluation.cs`/`StackMachine.cs` use Roslyn scripting for expression evaluation. Note the warning in `interop.h`: CoreCLR can only be init/shutdown **once** per process, and `Shutdown()` must happen inside `Main()`'s scope. `src/ncdbhook/` is a separate startup-hook dll for Hot Reload.

**`src/utils/`** — platform abstraction. Files come in `*_unix.cpp` / `*_win32.cpp` pairs (`filesystem`, `iosystem`, `platform`, `dynlibs`, `interop`), but **both are always in the source list** and are `#ifdef`-guarded internally — add new platform code the same way.

**Generated/vendored:** `src/coreclr/` is a copy of CoreCLR PAL/cordebug headers; `errormessage.cpp` is generated at build time from `corerror.xml` by `tools/generrmsg`. Neither is edited by hand.

## Conventions

- C++ code returns `HRESULT` and uses `IfFailRet`/`ToRelease<T>` (`src/utils/torelease.h`) for COM lifetime — follow that, not raw `Release()`.
- Interop-only code goes behind `#ifdef INTEROP_DEBUGGING`, and new source files must be added to the `INTEROP_DEBUGGING` branch of `netcoredbg_SRC` in `src/CMakeLists.txt`, not the main list.
- Logging: `LOGI`/`LOGE`/etc. from `utils/logger.h`. Set `LOG_OUTPUT=/tmp/log.txt` to capture (Tizen logs to dlog instead).
- Version lives in `src/version.h` and `packaging/netcoredbg.spec`.

## Manual runs

```sh
./bin/netcoredbg --interpreter=cli -- dotnet /path/to/program.dll
./bin/netcoredbg --interpreter=vscode --engineLogging=/tmp/engine.log
./bin/netcoredbg --interpreter=cli --interop-debugging --attach <pid>
./bin/netcoredbg --server[=4711]        # TCP instead of stdin/out; not valid with CLI
```

Further docs: `docs/cli.md` (CLI commands), `docs/interop.md` (what interop mode supports and its restrictions), `docs/stepping.md` (async stepping internals + diagrams in `docs/files/`).
