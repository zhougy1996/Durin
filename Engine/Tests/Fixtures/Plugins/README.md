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
false before an all-target build: all eligible plugin targets and inventory
entries must still exist. Then enable/disable overrides and launch fresh processes
without configuring, building, or republishing inventories. The build catalog is
independent of startup selection; stale DLL scans never grant loading permission.

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
is unchanged. A fresh process in disabled mode must reject every fixture module
without rebuilding, despite the inventory still containing their native files.
Restore the original descriptor and success mode must work without a build.
Version 1 inventories are rejected; mutate version 2 selected descriptor Inputs
to verify stale-build rejection instead of changing a selection fingerprint.
The probe also rejects explicit registration outside the frozen path set, not
only name-based loading.

The probe exercises the production Projects/Core boundary without a window or
renderer. It must be supplemented by bounded Editor and Game host startup/exit
runs to qualify the actual Launch, content, and workspace insertion points.

## Resource Integration Probe

The fixture Runtime and consumer plugins declare read-only Content and Configs;
Runtime also declares a Slang source directory. Build `PluginFixtureResourcesProbe`
in Editor alongside the inventory. Run with the absolute fixture project and
`seed`, `direct`, `load`, `disabled`, or `reject`. Set the project's
Configs/Project.yaml PluginFixture.Order to `project`; plugin defaults supply
BaseOnly. Create the fixture project's Content directory before seeding.

Seed writes test-only serialized packages under the plugin mount and `/Game/`;
ordinary `SavePackage` must still reject the read-only plugin destination.
Direct mode explicitly admits the mounted package without a catalog scan; load
mode discovers it via the registry. Rename the plugin's physical directory and
repeat load: virtual package identity and Shader source fingerprint stay stable.
Disable every override and run disabled: roots/defaults disappear, the saved
project-owned plugin-class package fails with a provider/enable/restart diagnostic,
and source bytes and resident state remain unchanged. Reject mode covers
colliding mount definitions or invalid descriptor resource paths.

The ShaderBuilder regression compiles this fixture's Shader via a plugin virtual
root, checks source invalidation, and verifies caches stay outside source files.
For actual Editor/Game host runs set `DURIN_PLUGIN_FIXTURE_EXPECT_CONFIG` to the
expected PluginFixture.UserWins value. Runtime provider startup then verifies
consumer defaults override Runtime defaults, project ProjectWins is true, and
UserWins matches. This assertion is opt-in so native lifecycle probes retain
their existing trace and bootstrap contract.

## Management Integration Probe

Build `PluginFixtureManagementProbe` with the matching Editor or Game inventory
and deploy it beside that inventory's Runtime binaries. Pass the absolute project
file and one mode: `readonly`, `stale`, `ownership-parent`, `disable`, `enable`,
`observe-enabled`, `observe-disabled`, `save-blocked`, `observe-blocked`, or
`missing-build`. Use a disposable fixture
copy with its Content directory present. The probe links ordinary engine modules
only; it does not directly link plugin DLLs.

Run disable/observe-disabled and enable/observe-enabled as separate processes.
Saving must retain the current process's frozen module selection and mount
revision; the next process must consume the saved overrides without rebuilding.
Readonly and stale modes check failed-save isolation. Ownership-parent holds the
project's edit lease while its child verifies failed-save isolation. Missing-build temporarily moves
the inventory after startup admission: valid overrides still save, but restart
readiness fails. Save-blocked unchecks Runtime while Consumer stays checked;
observe-blocked verifies the next process remains usable, keeps Consumer's saved
request and loads neither native plugin. Restore the original descriptor after qualification, and compare
inventory/build-graph hashes across the toggle cycle to exclude republication.

The actual Plugins window additionally needs an interactive check of checkbox
dependency warnings, pending-versus-running state and the normal unsaved-document
restart workflow. The headless probe does not qualify presentation or relaunch.
