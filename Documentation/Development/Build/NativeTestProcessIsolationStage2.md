# Native Test Process Isolation Stage 2 Evidence

Date: 2026-07-28

Baseline: `e82c4cd6`

## Result

The seven module-era native-test executables were replaced by 29
feature/lifecycle execution domains. `NativeTestIsolationProbeTests` remains a
separate characterization target and is not included in that count.

| Area | Functional targets |
| --- | --- |
| Core and object lifecycle | `CoreUtilityTests`, `CoreFileSystemTests`, `CoreConcurrencyTests`, `CoreObjectTests` |
| Assets | `AssetPackageTests`, `AssetCookTests`, `AssetDerivedDataTests`, `AssetDecodeTests`, `AssetImportTests` |
| Render contracts and shaders | `RenderShaderContractTests`, `RenderShaderCacheTests`, `RenderShaderServiceTests`, `RenderContractTests` |
| Engine and editor features | `EditorPropertyTests`, `EditorAssetWorkflowTests`, `MaterialTests`, `StaticMeshTests`, `TextureTests`, `ThumbnailTests`, `WorldTests`, `ViewportTests`, `SplineTests`, `SkyBoxTests`, `SkyBoxVulkanIntegrationTests`, `EditorRenderingTests`, `EditorShellTests`, `ExternalToolTests` |
| Dedicated integration lifecycles | `TextureCookIntegrationTests`, `VulkanRHIIntegrationTests` |

The superseded `CoreTests`, `CoreDObjectTests`, `AssetCoreTests`,
`RenderCoreTests`, `EngineTests`, `TextureCookTests`, and `VulkanRHITests`
executables are no longer declared.

## Enforced Ownership

`add_durin_test` records every `.cpp` below `Engine/Tests/Native` and rejects a
source assigned to two targets. After all native-test subdirectories are
configured, `durin_validate_native_test_source_ownership` compares the recorded
set with a `CONFIGURE_DEPENDS` recursive source inventory. Configuration fails
when a new test source is unowned, when one is duplicated, or when ownership
references a missing file.

The final configuration validated unique ownership for 92 native `.cpp`
sources: the 91-source Stage 0 baseline plus the characterization probe.
Because every owned source is compiled into exactly one discovered executable,
GoogleTest discovery preserves the existing case names without omission or
cross-domain duplicate ownership.

Every discovered target also registers
`Durin.NativeTestDirect.<target>`. These 30 direct-executable smoke tests run
all cases in one process and prove that each combined functional lifecycle is
coherent.

## Resource Policy and Cost

- All targets retain the Stage 1 default per-target compatibility lock until
  their Stage 4 sandbox migration is complete.
- The three Slang compiler/cache/service domains share `shader-compiler`.
- `MaterialTests`, `SkyBoxVulkanIntegrationTests`, and
  `TextureCookIntegrationTests` share `renderer-runtime`.
- Those three targets and `VulkanRHIIntegrationTests` share the irreducible
  `durin-gpu` resource.

The final 18-job aggregate accumulated 112.33 process-seconds under the
`native-test` label and completed in 12.22 wall-clock seconds. The 30
whole-executable smoke tests accounted for 18.72 process-seconds. These values
are the pre-sandbox serialization baseline for Stage 3 and Stage 4.

## Validation

- Configure: unique ownership for 92 native sources.
- Full build:
  `Build/.agent-state/logs/20260728-054705-346861-26288-cmake.log`.
- Aggregate:
  `Build/.agent-state/logs/20260728-054708-712222-26288-ctest.log`.
- Aggregate result: 680 of 680 passed, with three expected skips.
- Direct lifecycle result: 30 of 30 target smokes passed.
- Characterization:
  `Build/.agent-state/logs/20260728-054744-177516-11764-cmake.log`;
  the shared-root legacy mode still observes the collision and the isolated
  control passes both processes.
