# IDE Code Model And Debugging

This guide configures Visual Studio Code or CLion as a code browser, editor, and
debugger while keeping DurinDevTool as the only build owner for the checkout.

## Shared Workflow

All configuration, builds, and tests that produce Durin outputs must run through
DurinDevTool:

```powershell
.\DevTool.bat build <Target>
.\DevTool.bat test CoreConcurrencyTests
```

An IDE may read an isolated CMake tree or a compilation database, and it may
launch an executable that DurinDevTool already produced. It must not invoke CMake
build, Ninja, Clean, Rebuild, install, or a test build. Do not run IDE CMake
Configure/Reload while DurinDevTool owns the checkout.

For the first editor launch, build the complete runtime through DurinDevTool:

```powershell
.\DevTool.bat build all
```

For later debugging, rebuild only changed modules and affected consumers when
the existing runtime is complete. Build any newly required runtime modules too;
use `all` when the required runtime closure cannot be bounded reliably. Code
model use alone does not require building the runtime. The commands above are
alternatives, not a recurring validation sequence.

The editor executable is
`Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.

## Visual Studio Code

Visual Studio Code does not need CMake Tools to provide the C++ code model.
DurinDevTool's default `Win64-Debug-DurinEditor` preset generates:

```text
Build/Win64-Debug-DurinEditor/compile_commands.json
```

`DevTool setup` copies the tracked `settings.json` and `extensions.json`
templates under `Templates/VSCode` into the ignored local `.vscode` directory
without overwriting existing files. The generated `settings.json` points clangd
at this database, leaves the language server disabled until requested, and
disables automatic configure/build behavior if CMake Tools happens to be
installed. Linked worktrees share the prepared source worktree's `.vscode`
directory through `DevTool worktree prepare`. Run `DurinDevTool configure`
after creating a fresh build tree or changing CMake inputs so the database
remains current. Run a normal DurinDevTool build when generated DHT headers or
sources are missing or stale.

### clangd

Install the `llvm-vs-code-extensions.vscode-clangd` extension. The extension and
the clangd language server are separate. Opening a C or C++ file activates the
extension, but the workspace setting `clangd.enable: false` prevents the server
from starting or indexing automatically. When code navigation is needed, run
**clangd: Manually activate extension** from the command palette and accept
**Enable**. The extension enables `clangd.enable` persistently, so set it back
to `false` in `.vscode/settings.json` after the session to keep later launches
manual. **clangd: Shutdown language server** stops the current session. If the
server is unavailable, run **clangd: Download language server**, then
**clangd: Restart language server**.

clangd and Microsoft C/C++ IntelliSense must not both provide language features.
The workspace template disables `ms-vscode.cpptools` IntelliSense so it does not
start an alternative automatic code model while clangd is off:

```json
{
  "C_Cpp.intelliSenseEngine": "disabled"
}
```

Developers who prefer Microsoft C/C++ IntelliSense can change this workspace
setting to `default` and configure the extension to read the same
`compile_commands.json` file.

### Debugging

When `.vscode/launch.json` is missing, `DevTool setup` generates one launch
configuration for every CMake preset registered to the selected Agent Build
Profile. Each configuration derives its executable from the preset's platform,
build type, profiling role, and runtime variant. Code-model-only, hidden, and
other-host presets are excluded because they are not registered to that
profile. Existing `launch.json` files are never overwritten.

Generated launch configurations intentionally have no `preLaunchTask` or build
task. A launch configuration names the preset that defined its executable path,
but it does not select or build that preset. Build or refresh the executable
from a terminal with DurinDevTool before starting the debugger.

## CLion

CLion uses the IDE-only `Win64-Debug-DurinEditor-FastConfigure` CMake profile for
its code model. This preset has an isolated CMake/Ninja tree and disables PCH
artifact generation while retaining the forced project PCH includes needed by
the code model. DurinDevTool intentionally does not own this preset. The preset is
also marked `DURIN_IDE_CODE_MODEL_ONLY=ON`: every generated build target depends
on a guard that prints an actionable error and fails before DHT, compilation, or
linking can start.

Configure CLion as follows:

1. Open the workspace root.
2. In **Settings | Build, Execution, Deployment | CMake**, enable only
   `Win64-Debug-DurinEditor-FastConfigure` for the Durin code model.
3. Set the profile environment to `VSLANG=1033`.
4. Allow CMake Configure/Reload only while DurinDevTool is idle.
5. Create a **Native Application** run configuration for
   `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.
6. Remove **Build** and every other compilation step from **Before launch**.

The FastConfigure profile is for CMake configuration, project discovery, and
CLion indexing only. Its generated targets intentionally cannot build because
compiling without PCH would be unnecessarily slow. Never use CLion's Build,
Rebuild, Clean, target, or test-build actions. Build from a terminal with
DurinDevTool, then use CLion only to read code, launch, or attach the debugger.

An accidental CLion Build action should fail immediately with this message and
must not start any compiler or DHT process:

```text
ERROR: This IDE preset is code-model-only and cannot build. Use DevTool.bat with a registered build preset.
```

FastConfigure shares final binaries and generated DHT metadata with the normal
DurinEditor presets. Its separate CMake tree prevents object-file collisions but
does not make concurrent IDE configuration or IDE builds safe.

## Troubleshooting

If clangd reports missing project includes or widespread incorrect macros,
confirm that its output names
`Build/Win64-Debug-DurinEditor/compile_commands.json`, rerun
`DurinDevTool configure`, and restart the language server. A newly prepared checkout
may also need one successful DurinDevTool build to create generated DHT files.

If CLion prints localized MSVC `/showIncludes` lines such as
`注意: 包含文件:`, confirm `VSLANG=1033`, then reset only the CLion CMake cache or
remove `Build/Win64-Debug-DurinEditor-FastConfigure` while DurinDevTool is idle and
let CLion configure it again. Do not remove a DurinDevTool-owned build tree as IDE
maintenance.

If an IDE accidentally starts a build, stop it and wait for its complete process
tree to exit. Follow the interruption recovery procedure in `BuildAndRun.md`
before the next DurinDevTool build.

## Related Docs

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/RuntimeVariants.md`
