# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Pulse is a Windows-only C++20 file manager (Win32, Direct2D, DirectComposition; MSVC is required, CMake refuses other compilers). `README.md` / `README.en.md` describe the product; this file covers how to work in the code.

`AGENTS.md` (Chinese) is the checked-in contributor guide and stays authoritative for process rules. Read it before changing anything; the points that matter most:
- Stay inside the user's request and this change; keep existing uncommitted edits; no drive-by refactors.
- Verify narrowly: build only the affected target and run only the tests tied to the change. Do not default to `build_release.bat`, the full `--selftest`, or every test exe. Widen only with a stated reason. A successful compile is not functional verification.
- Do not weaken or delete tests to get a pass; keep isolated fixtures in `bench_data/` and never touch the user's real files or preferences while testing.
- Commit subjects are imperative and scoped, e.g. `preview: decode UTF-16 BE text`.

## Build and test

Everything runs from an x64 MSVC environment with CMake 3.25+ and Ninja. The `.bat` scripts call `scripts\vcvars.bat`, which locates Visual Studio through `vswhere` when the shell is not already a developer prompt.

```powershell
.\build_release.bat                         # configure (Ninja, Release, build\) + build everything; /clean = fresh, /symbols = RelWithDebInfo
cmake --build build --target pulse          # iterate on one target once build\ is configured
.\build\pulse.exe
.\build_installer.bat                       # Release build + dist\PulseSetup-<version>.exe (needs Python 3); /skipbuild reuses build\
.\build_win81.bat / .\build_win81_installer.bat   # Windows 8.1 channel into build-win81\; needs a VS 2022 v143 toolset (PULSE_VC_ROOT or tools\prepare_win81_toolchain.py)
pwsh ./scripts/build_release_ci.ps1 -Channel windows   # what CI runs: build, regression tests, release payload checks
```

- `build_release.bat` configures with `PULSE_WITH_SELFTEST=OFF` and `PULSE_WITH_TEST_HOOKS=OFF`. The raw CMake defaults are `SELFTEST=OFF`, `TEST_HOOKS=ON`; configure a separate build dir if you need the GUI probes (`--shot-*` flags, `PULSE_TEST_*` env vars, `--picker-*-test` suites) compiled into `pulse.exe`.
- There is no CTest. Each test is its own console executable (`pulse_*_test`, ~100 targets in `CMakeLists.txt`) printing `[PASS]/[FAIL]`. Build one with `cmake --build build --target pulse_ops_test` and run `.\build\pulse_ops_test.exe`. There is no generic filter; many exes take a mode flag (`--update-shutdown`, `--delete-progress-only`, ...). `scripts/build_release_ci.ps1` is the reference for which exe runs with which flags.
- Where cases go: preview → `src/bench/preview_test_main.cpp`; operations → `src/bench/ops_test_main.cpp`; app model → `src/app/selftest_1b2.cpp` (only built into `pulse.exe` with `-DPULSE_WITH_SELFTEST=ON`, run via `pulse.exe --selftest` and wait for it to exit).
- To drive `pulse.exe` for testing without touching real settings, start it with `--test-instance` plus `PULSE_TEST_DATA_DIR=<scratch folder>` (see `isolatedTest` in `app_core.h` and `app_main.cpp`); persistence into that folder only happens in `PULSE_WITH_SELFTEST=ON` builds.
- Python helper checks run on Linux in CI (`.github/workflows/l10n.yml`) and are cheap to run locally:

```powershell
pip install -r tools/l10n/requirements.txt
python tools/l10n/gen_strings.py --check        # generated string files match catalog.tsv
python tools/l10n/check_service_text.py         # helper-process text has English
python tools/l10n/check_no_pick.py              # no new l10n::Pick(L"..", L"..") in product code
python tools/actions/check_actions.py --strict  # every action is registered everywhere
```

## Architecture

### Multi-process layout

Pulse is split into several executables so blocking or risky work never runs on the UI thread. Each has its own source directory and sources are listed explicitly in `CMakeLists.txt`, except the UI process.

| Process | Source | Role |
| --- | --- | --- |
| `pulse.exe` | `src/app`, `src/ui`, `src/fs`, `src/ops`, `src/ipc`, client side of `src/index` | UI, state, navigation, rendering, async operations |
| `Pulse.Index.exe` | `src/index` (`index_host`, `index_engine`, `index_mft`, `change_tracking`, `network_agent_main`) | Whole-disk MFT/USN filename index and query host; runs as a session helper or the optional `PulseIndex` Windows service |
| `Pulse.Content.exe`, `Pulse.Document.exe` | `src/index` (`content_*`, `document_*`) | Content search and document text extraction (PDFium); shipped in the content enhancement pack |
| `pulse_shell` | `src/shell_host` | Shell COM isolated in its own process: `IFileOperation`, recycle bin, third-party context-menu sessions on STA threads |
| `pulse_preview` | `src/preview_host` | Quick Look / thumbnail decoding (images, documents, archives, video); optional packs in `packs/` extend formats |
| `Pulse.AI.exe` | `src/ai_host` | MCP server for external AI apps, shipped in the AI pack; `pulse.exe --mcp-stdio` is only a forwarder |
| `pulse_elevated` | `src/elevated_host` | Elevated transfer/delete helper; validates parent process, session and nonce before serving |
| `pulse_integration` | `src/integration_host` | Elevated Windows shell-integration and system context-menu registry changes (one UAC per batch) |
| setup | `src/setup`, `tools/pack_installer` | Installer bootstrapper |

The wire protocols are in `src/ipc/` and are documented in their headers (framing, message types, cancellation, session lifetimes): `protocol.h` (UI ↔ shell host, named pipe `pulse_shell_<ui-pid>`), `preview_protocol.h`, `ai_tool_protocol.h`, `elevated_transfer_*.h`. Change a protocol on both sides in the same commit; the executables of one build are always deployed together.

Build-system gotchas:
- `pulse.exe` sources are `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` over `src/app src/ui src/fs src/ops src/ipc src/index`. A new `.cpp` there is picked up automatically, but files that belong to a helper process (in `src/index` especially) must be added to the matching `list(FILTER ... EXCLUDE REGEX ...)` and to that target's explicit source list. The same applies to files reused by tests: tests list the few `.cpp` files they need, so a new dependency in shared code can break unrelated test targets at link time.
- `pulse.exe` uses two precompiled headers (`src/common/pch_system.h`, `src/app/pch_app_state.h`); settings/dialog sources are compiled with `/Os`. Sources that redefine `NTDDI_VERSION` or use `INITGUID` are listed in `_pulse_no_pch`.
- Text uses the LumaText SDK (prebuilt in `third_party/lumatext`, delay-loaded) with DirectWrite as fallback. `cmake/lumatext-sdk.json` pins its hash.
- `version.txt` is the single version source; `docs/releases/<version>.md` (+ optional `.en.md`) are embedded into `pulse.exe` by `cmake/release_notes.cmake`. Update manifests are ECDSA-signed (`cmake/update-public-key.txt`); see `docs/automatic-updates.md`.

### Inside the UI process

- `AppCore` (`src/app/app_core.h`) holds process-wide services and data (snapshot store, worker pool, watchers, ops manager, index clients, prefs, update checker); `AppState` (`src/app/app_state.h`) is the per-window UI state and reaches shared things through `AppState::core`. `AppState` still carries same-named reference aliases (`s.store`) for old call sites; new code should write `s.core.store`.
- `src/fs` produces directory snapshots (`SnapshotStore`) off the UI thread; `src/ops` runs asynchronous copy/move/delete through `OpsManager`, delegating Shell-only work to the shell host over the pipe. Navigation and loading use generation counters so stale results are dropped; follow that pattern for anything async.
- `src/ui` draws with Direct2D on a DirectComposition surface. The app builds plain view models (`src/ui/ui_view_model.h`) and `MainRenderer` draws them. Keep new state in view-model headers rather than widening `ui_renderer.h`, which rebuilds many sources. Reuse the existing Fluent controls and `FluentTokens`.
- `src/app/app_main.cpp` (window procedure, startup, `--shot-*` test modes) and `app_commands.cpp` are very large; put new features in their own `.h/.cpp` pair.

### One action table drives menus, shortcuts, palette, assistant and MCP

Every user-visible command is an "action" registered once; the menus, keyboard shortcuts, command palette (`>`), the built-in Pulse assistant and the external MCP tools (`find_actions` / `run_actions`) all read it. The pieces live in `src/app/actions/` (`action_inventory.h` ids and risk classes, `action_registry.cpp` rows, `action_args*.cpp` runners, `action_params.cpp` parameter specs, `action_policy.cpp`, `action_files.cpp` plan cards). `docs/ai/actions.md` is the step-by-step guide; `tools/actions/check_actions.py --strict` reports anything missed and gates CI. Action ids are public (MCP clients and saved "always allow" rules key on `pulse.<id>`), so never rename a released id. Pick the risk class by the worst thing the action can do. MCP-facing behavior is documented in `docs/ai/mcp.md`.

### UI strings (l10n)

`tools/l10n/catalog.tsv` (name / id_name / id / zh / en / tw) is the only source of UI text. After editing it run `python tools/l10n/gen_strings.py`, which regenerates `string_ids.h`, the `StringId` enum in `localization.h`, `strings.rcinc`, `strings_zh_tw.rcinc` and `zh_hant_table.inc`; never hand-edit those. Product code uses `l10n::Get(l10n::StringId::X)`, not `l10n::Pick`. Existing ids are never renumbered (tests depend on id ranges). New messages from background processes also need an English entry in `src/common/service_text.cpp`.

### Runtime data and diagnostics

Crash logs and the shell-host log are written to `%LOCALAPPDATA%\Pulse` (`pulse_crash.log`, `pulse_shell_host.log`); structured runtime logs go to `%LOCALAPPDATA%\Pulse\Diagnostics\Runtime` (index service: `%PROGRAMDATA%\Pulse\Diagnostics\Runtime`) and deliberately contain no file names, paths or search terms (`docs/diagnostics.md`). Index storage and migration are covered in `docs/index-migration.md`.

## Further reading

`docs/` holds many per-feature design and audit notes (index memory/CPU work, preview packs, folder-size tiles, change tracking, Markdown math). Search there by topic before redesigning an area; `docs/windows-compatibility.md` covers the Windows 8.1 channel.
