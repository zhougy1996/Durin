# Native Test Process Isolation Stage 3

Date: 2026-07-28

Baseline: `10aeb1de8369ad5a295e36aeab08d29645046ebe`

## Outcome

All native-test executables now use a common Core-only process harness instead
of `GTest::gtest_main`. Before GoogleTest starts, the harness creates one
canonical writable directory below the target-owned
`Work/Runs/run-p<PID>-<32 lowercase hex nonce>` container.

The public test API is:

- `Durin::Testing::GetTestWorkDirectory()`
- `Durin::Testing::CreateTestWorkSubdirectory(RelativePath)`
- `Durin::Testing::IsTestWorkDirectoryKept()`

Subdirectory creation rejects empty, absolute, and lexically escaping paths.
Initialization returns the same directory for the lifetime of one process.
Directory allocation uses atomic `create_directory`; a deterministic regression
pre-occupies the first PID/nonce candidate and verifies retry without modifying
the existing directory.

## Lifecycle Policy

- Passed runs are removed by the GoogleTest program listener.
- The common entry point also cleans successful `--gtest_list_tests` runs,
  because GoogleTest does not emit the program-listener event during discovery.
- Failed runs are retained and their path is printed.
- `--durin-keep-test-work` or `DURIN_TEST_KEEP_WORK=1` retains a successful
  run and prints its path.
- Abrupt termination bypasses listeners, so the already-created directory
  remains available for diagnosis.
- Cleanup errors retain the directory and print the path plus the error.
- Harness setup exceptions are converted to a diagnostic and exit code 2.

The abrupt-exit characterization uses `std::_Exit(3)`. An initial `abort()`
implementation was rejected because the MSVC Debug CRT displayed an
interactive dialog; the final probe is unattended and produces no window.

## Validation

- Configure validated unique ownership for all 92 native-test `.cpp` sources.
- `NativeTestIsolationProbeCharacterization` passed:
  - two concurrent CTest processes wrote the same logical filename below
    distinct process sandboxes;
  - keep-work retained the reported directory;
  - a controlled cleanup failure retained and reported the directory;
  - abrupt exit retained the directory without an interactive CRT dialog.
- `FNativeTestProcessSandboxTests.*`: 4/4 passed, covering canonical/idempotent
  lookup, PID reuse, Unicode and long paths, and containment rejection.
- Full `all` build passed.
- Aggregate test: 684/684 CTest entries passed at 18 jobs in 16.90 seconds.
- All 30 target `Runs` roots contained zero leftover successful sandboxes after
  the aggregate.

Logs:

- Full build:
  `Build/.agent-state/logs/20260728-062146-079422-38652-cmake.log`
- Aggregate:
  `Build/.agent-state/logs/20260728-062212-941714-39880-ctest.log`
- Sandbox characterization:
  `Build/.agent-state/logs/20260728-062202-174983-34552-cmake.log`
- Focused API regression:
  `Build/.agent-state/logs/20260728-061634-929275-15576-NativeTestIsolationProbeTests.log`
