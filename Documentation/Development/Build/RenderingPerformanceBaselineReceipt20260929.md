# RHI/RDG Performance Baseline Diagnostic Receipt — 2026-09-29

Summary: Preserve the first unified Tracy-off `render-performance` Release run and its Apple M4 observations without treating them as frozen budgets.

Last reviewed: 2026-09-29

## Environment and Outcome

- Runtime source revision: `18a83e52326f3959f31e47150d68ed7702134db6`, with the baseline catalog and measurement-reporting changes from this handoff applied.
- Host: macOS 27.0 (`26A428`), Apple M4, 10 CPU cores, 16 GB unified memory; Apple clang 21.0.0 (`clang-2100.1.1.101`); Ninja generator.
- Preset: `MacOS-arm64-Release-DurinEditor`; configuration: Release; `DURIN_WITH_TRACY=0`; Vulkan validation off by the Release `auto` policy; RHI execution threaded; graphics and compute share queue family/index `(0,0)`.
- Device: Apple M4 integrated GPU, Vulkan API `1.3.334`, driver `0x28a1` (`10401`).
- Command: `./DevTool test "@domain=render-performance" --mode qualification --preset MacOS-arm64-Release-DurinEditor --test-jobs 1 --report Build/Baselines/RHIRDG/2026-09-29-M4-Release-TracyOff.xml`.
- Outcome: 6 of 7 selected targets passed in 25.38 seconds. `DirectionalShadowBaselineVulkanTests` failed its high-motion output assertion after publishing preparation observations. `VulkanCreationQualificationTests` was absent because Release macOS configures application tests off.
- Authority: local diagnostic. This run established discovery, build, execution, and reporting, but the host was not certified as an exclusive quiet GPU lane. No regression threshold is frozen from these values.

The local aggregate report is `Build/Baselines/RHIRDG/2026-09-29-M4-Release-TracyOff.xml`; the scheduler log is `Build/.agent-state/logs/20260929-210844-041003-42592-ctest.log`, and detailed target output was captured in the preset's `Testing/Temporary/LastTest.log`. Build-local logs are disposable, so the normalized observations are retained below.

## Normalized Observations

All durations are nanoseconds unless a row says otherwise. Median and p95 use each fixture's declared sample population.

| Workload | Median | p95 | Memory/shape notes |
| --- | ---: | ---: | --- |
| RDG synthetic compile, run 1 | 37 us | 45 us | 128 passes; 30 warm-ups; 120 samples |
| RDG synthetic compile, run 2 | 37 us | 44 us | same workload |
| RDG synthetic compile, run 3 | 36 us | 37 us | same workload |
| Static-mesh preparation, run 1 | 105,000 | 121,583 | 64 primitives; 256 draws; 47,120 peak owned bytes |
| Static-mesh preparation, run 2 | 105,500 | 116,708 | 47,120 peak owned bytes |
| Static-mesh preparation, run 3 | 105,958 | 116,084 | 47,120 peak owned bytes |
| Material first visible frame | 12,868,541 | 15,855,208 | 30 cold rounds |
| Material following frames | 621,000 | 2,286,750 | 120 frames per round; 3,600 samples |
| Directional shadow: single/cascade | 54,542 / 160,791 | 70,667 / 201,750 | failed target; observation is not accepted |
| Directional shadow: discovery/static | 5,792 / 153,332 | 6,375 / 194,168 | 128 unique primitives; 384 memberships; failed target |
| HDR copy / FXAA | 32,792 / 315,375 | 153,958 / 402,542 | 24,883,200 retained target bytes; 8,294,400 output bytes |
| GBuffer / deferred / combined | 157,812 / 1,020,083 / 1,176,083 | 246,292 / 1,147,250 / 1,305,084 | 107,827,200 active-route bytes |
| GTAO raw / filter / combined | 1,704,604 / 674,125 / 2,380,062 | 3,356,500 / 994,750 / 4,351,250 | 4,147,200 active bytes |
| Hybrid production total | 1,602,707 | 2,122,290 | 69,984,000 active bytes; 120,315,648 with shadow |
| Hybrid production full/half GTAO feature | 2,383,708 / 1,314,395 | 4,335,292 / 1,644,000 | half-resolution resolve median 404,521 |

Volumetric-cloud spatial observations:

| Resolution/route | Compute median / p95 | Fragment median / p95 | Retained bytes |
| --- | ---: | ---: | ---: |
| 1280x720, reversed Z | 1,630,334 / 1,895,625 | 1,853,834 / 3,343,584 | 22,118,400 |
| 1919x1079, viewport 1601x901, reversed Z | 2,592,167 / 4,605,750 | 2,796,250 / 4,361,042 | 49,694,424 |
| 1920x1080, reversed Z | 3,703,250 / 5,659,958 | 3,880,083 / 5,582,625 | 49,766,400 |
| 1279x719, viewport 1101x623, forward Z | 1,212,667 / 2,864,833 | 1,397,000 / 1,436,708 | 22,070,424 |

Volumetric-cloud temporal observations:

| Quality/resolution | Route median / p95 | Shadow median / p95 | Retained bytes | MAE |
| --- | ---: | ---: | ---: | ---: |
| Reference, 3840x2160 | 19,677,875 / 22,998,500 | 18,945,375 / 22,181,417 | 215,654,400 | reference |
| Performance, 1920x1080 | 4,162,334 / 6,314,708 | 2,952,042 / 3,880,500 | 149,299,200 | 0.0348199 |
| High, 1920x1080 | 5,717,291 / 6,543,250 | 4,568,542 / 6,730,875 | 149,299,200 | 0.0162229 |
| Epic, 1920x1080 | 7,761,584 / 10,027,500 | 6,687,083 / 8,386,375 | 149,299,200 | 0.00318364 |

## Open Qualification Items

- Re-run the same workload and a reference revision on an exclusive quiet GPU lane before assigning any acceptance budget.
- Investigate the repeatable directional-shadow high-motion mismatch: observed `(33,53)`, expected `(32,190)`. The preparation timings from that target are informational only.
- Run the application-hosted Vulkan creation target under an explicitly configured Release preset when that workload is required.
- Add a supported native-event Vulkan device for split/full barrier and dedicated-compute comparisons; Apple M4/MoltenVK supplies only the full-barrier shared-queue fallback.
