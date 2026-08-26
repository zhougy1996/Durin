# Create-Only Graybox Build

`scene graybox-build` creates one new project Level through the normal
DurinEditor asset system. It does not compile a temporary target, emit scene
JSON, edit `.dasset` bytes, connect to Echo SceneBox, or control an already
running Editor.

Run it from the repository root after building the Editor:

```powershell
.\DevTool.bat scene graybox-build `
  --project Sandbox\Sandbox.dproject `
  --output /Game/Levels/GrayboxArena
```

The default `open-arena` layout contains a floor, four connected walls, a
centered `PlayerStart`, and a `DirectionalLight`. It has no ceiling. Each
geometry piece is an ordinary `AStaticMeshActor` using
`/Engine/Models/Box`, so it remains selectable and editable after opening the
Level.

Dimensions use engine world units. `--width` and `--depth` are the clear
walkable distances between inner wall faces. Available options are:

```text
--width <0.1..10000>             default 20
--depth <0.1..10000>             default 20
--floor-thickness <0.1..10000>   default 0.5
--wall-height <0.1..10000>       default 4
--wall-thickness <0.1..10000>    default 0.5
--ceiling                        off unless explicitly supplied
--timeout <1..3600>              default 300 seconds
```

The output must be a complete mounted path in the selected project and must
not already exist. The initial command intentionally has no `--replace` mode.
Choose another path to preserve an existing Level.

The hidden Editor process creates a temporary candidate in the same project
mount, applies the complete StaticMesh batch, saves and reloads it, verifies
the Actor set, and then publishes it to the requested empty path. A failed
pre-publication build removes only its command-owned candidate. A failed
post-publication verification restores the relocation before cleanup.

Only one DurinEditor process may author a project at a time. Close a visible
Editor using the same project before running the command; an ownership conflict
is reported instead of allowing concurrent writes.

## Related code

- `Engine/Source/Editor/LevelEditor/Public/GrayboxSceneBuild.h`
- `Engine/Source/Editor/LevelEditor/Private/Operations/GrayboxSceneBuild.cpp`
- `Tools/DurinDevTool/durin_dev_tool/scene.py`
