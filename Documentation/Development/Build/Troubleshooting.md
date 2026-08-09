# Troubleshooting Setup, Builds, And Runs

Use this guide to identify common Durin development-environment failures by
symptom. The complete command behavior and recovery contract remain in
[Build And Run](BuildAndRun.md).

## Setup And Prerequisites

### Python is not found

Install Python 3.10 or newer with `venv`. Confirm that either
`py -3 --version` or `python --version` succeeds, then rerun:

```powershell
.\DevTool.bat setup
```

### Win32 long paths are disabled

Enable **Win32 long paths** under
`Computer Configuration > Administrative Templates > System > Filesystem`,
then restart Windows. This sets
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` to
`REG_DWORD 1`. DurinDevTool reports the missing policy but does not change
machine state.

### CMake or Visual Studio cannot be detected

Rerun interactive setup and confirm the detected paths, or edit the relevant
override in `.agents/DevTool.user.json`. Setup preserves existing local
configuration and validates the selected CMake executable and Visual Studio
environment script before saving them.

For unattended environments, `setup --non-interactive` accepts valid saved or
automatically detected settings and otherwise exits with an actionable error.

### The MSVC toolset is too old or diagnostics are localized

Update Visual Studio 2022 to 17.14 or newer, install MSVC Build Tools 14.44 or
newer, and add the English language pack through Visual Studio Installer.
DurinDevTool sets `VSLANG=1033` because CMake and Ninja dependency parsing
depends on English MSVC diagnostics.

### Vulkan SDK files are missing

Ensure `VULKAN_SDK` names a LunarG Vulkan SDK containing:

- `Include/vulkan/vulkan.h`;
- `Include/vma/vk_mem_alloc.h`;
- `Lib/vulkan-1.lib`.

Current SDK releases include Vulkan Memory Allocator. With an older SDK, update
it or place VulkanMemoryAllocator's `vk_mem_alloc.h` under the SDK's
`Include/vma` directory, then rerun setup.

### The virtual environment or libclang is missing or inconsistent

Rerun setup first. The root `requirements.txt` pins both the `clang.cindex`
bindings and the matching native `libclang` package. Do not install unrelated
system site-packages into `.venv`.

If `.venv` was created by an unsupported Python version and setup cannot repair
it, remove only that repository-local `.venv` after confirming no process is
using it, then rerun setup.

### A third-party source directory is incomplete

Setup reuses prepared dependencies and will not silently replace an unexpected
partial source tree. Repair or move aside the exact directory named by the
error, restore network access if needed, and rerun setup. See
[Third-Party Bootstrap](ThirdPartyBootstrap.md) for dependency locations and
ownership.

## Linked Worktrees

### Setup refuses to run in a linked worktree

Setup initializes only the main checkout. Prepare a linked worktree from a main
checkout where setup already succeeded:

```powershell
.\DevTool.bat worktree prepare
```

Preparation links `.agents`, `.vscode`, `.venv`, and `Engine/External` from the
prepared source worktree. If dependencies live in a different prepared
worktree, pass `--source <prepared-worktree>`.

### A linked worktree reports a missing virtual environment

Run `worktree prepare` in that worktree. Do not run dependency-backed commands
against an arbitrary system Python or manually copy `.venv` between worktrees.

### A worktree cannot be removed safely

Preview the repository-owned removal path:

```powershell
.\DevTool.bat worktree remove <path> --dry-run
```

Do not recursively delete a prepared worktree or call `git worktree remove`
directly. Prepared Windows worktrees contain NTFS junctions, and an unsafe
recursive deletion can traverse into shared directories in the main worktree.

## Builds And Tests

### A build was interrupted

Do not start another build until the old CMake, Ninja, compiler, and linker
process tree has exited. Then inspect the affected preset:

```powershell
.\DevTool.bat status
```

- `Recovery state: clean`: rerun the original command if needed.
- `Recovery state: recover required`: run the reported `recover` command to
  resume the recorded target incrementally.
- `Recovery state: rebuild required`: run the exact rebuild command reported by
  `status`.

For a non-default preset, pass `--preset <affected-preset>` to `status` and to
its reported recovery command. Do not substitute `rebuild --target all` for an
interrupted native-test target: native-test targets are excluded from `all`.

### A compiler, linker, configure, clean, or test command failed normally

Fix the reported error and rerun the same command. Ordinary command failures,
failed assertions, test crashes, test timeouts, and application exits do not
leave build recovery state and do not require a rebuild.

### Another DurinDevTool operation owns the checkout

A checkout permits one build writer across all presets. Wait for the active
operation, or stop it from a second terminal if cancellation is intentional:

```powershell
.\DevTool.bat stop
```

After stopping it, wait for the process tree to exit and follow the interrupted
build procedure above. Do not mix direct CMake or IDE builds with DurinDevTool
in the same checkout.

### Terminal output omits the underlying failure

Compact output prints a bounded diagnostic excerpt. Open the complete retained
command logs with:

```powershell
.\DevTool.bat open logs
```

Use `--output full` to stream all child output on the next run, or `--plain` to
disable styling. Logs are stored under `Build/.agent-state/logs/`.

### The checkout lock cannot be opened on Windows

The persistent lock file may remain after a successful command; the OS byte
lock, not the file's presence or recorded PID, determines ownership. Follow
manual ACL or lock-file removal commands printed by DurinDevTool only after
confirming that DurinDevTool, DurinEditor, CMake, Ninja, compiler, and linker
processes have exited for the checkout. See the
[recovery section](BuildAndRun.md#recovery) for the lock contract.

## Running The Application

### The runtime executable is missing

Build the complete runtime for the selected preset, then run it:

```powershell
.\DevTool.bat build --target all
.\DevTool.bat run
```

Use `status` to confirm the selected preset and `path runtime` to print its
resolved runtime directory.

### The application exits or behaves unexpectedly

Application exits do not require build recovery. Open the runtime logs with:

```powershell
.\DevTool.bat open runtime-logs
```

Diagnose the reported runtime error, then rerun `run`. Use `open runtime` to
inspect the selected preset's executable and runtime files.
