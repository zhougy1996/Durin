# Source Plugin Build Fixture

Copy `PluginFixture` into an ignored workspace directory and temporarily add its
`PluginFixture.dproject` to `Durin.dworkspace`. Restore the workspace declaration
after validation. The fixture is intentionally absent from ordinary builds.

Build `PluginFixturePluginBuild` and `PluginFixtureProbe` through the host
DevTool launcher for both runtime variants. The inventory is under
`Build/<preset>/PluginBuild/PluginFixture/<configuration>/`. Copy the probe into
its `Runtime` directory and run it there; this uses exactly the generated native
dependency closure. The probe calls an exported function across two plugin DLLs
and accesses a reflected asset subclass inheriting from the other plugin.

The fixture includes Runtime, Editor companion, dependent, and content-only
plugins. Game must omit `PluginFixtureEditor`. Set both project overrides to
false, rebuild `PluginFixturePluginBuild`, and verify plugin targets disappear
from the generated graph and the inventory has no binaries despite stale DLLs.
Re-enable the overrides and repeat the build/probe to verify regeneration.

## Startup Integration Probe

The startup probe exercises the frozen source-plugin admission path. Build
`PluginFixtureStartupProbe` and refresh `PluginFixturePluginBuild` from the same
configuration after Core changes. Deploy the probe with its ordinary dependency
closure; it deliberately does not link a plugin DLL. Run with the fixture's
absolute project file and one mode: `success`, `browser`, `reject`,
`provider-failure`, `save-disable`, or `disabled`. Each invocation needs a fresh
process because admission is immutable.

Set `DURIN_PLUGIN_FIXTURE_TRACE` to a writable log path to observe start/stop
ordering. For provider failure, set `DURIN_PLUGIN_FIXTURE_FAIL` to
`PluginFixtureRuntime`; its startup throws and its shutdown records rollback.
The dependent Runtime module uses EngineReady, the base uses Providers, and the
Editor companion uses EditorReady. The probe checks reflection after each
Runtime phase, rejects early plugin loads, verifies edits leave admitted modules
unchanged, and verifies that process-resident modules cannot unload or restart
after shutdown. Browser mode admits no project and rejects project plugin loads.
Reject mode expects admission failure after an inventory/file/identity mutation.
Restore every negative fixture before starting the next case. In save-disable
mode the probe saves the disabled consumer override after Providers and still
starts the original EngineReady/EditorReady set, while checking the mount revision
is unchanged. A fresh process must reject the stale inventory; rebuild
PluginFixturePluginBuild, then disabled mode must reject every fixture module
despite stale native files. Restore the original descriptor and rebuild before
the next variant. The probe also rejects explicit registration outside the
frozen path set, not only name-based loading.

The probe exercises the production Projects/Core boundary without a window or
renderer. It must be supplemented by bounded Editor and Game host startup/exit
runs to qualify the actual Launch, content, and workspace insertion points.
