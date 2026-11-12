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
