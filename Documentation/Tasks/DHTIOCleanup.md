# Remove Unused DHT I/O Helpers

## Outcome

DurinHeaderTool exposes only the file and JSON helpers used by active
generation paths, with no misleading process-randomized fingerprint utility.

This is a bounded dead-code removal task, not a cache or file-I/O redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

`io/file_helper.py` and `io/json_helper.py` expose unused helpers including
`FileCacheEntry`, `get_file_fingerprint`, `verify_file_fingerprint`,
`is_file_changed`, `calculate_file_hash`, `parse_json_content`,
`_pascal_to_snake`, and `dict_from_dataclass`. `calculate_file_hash` uses
Python's process-randomized `hash`, so it is unsuitable for a persistent
fingerprint if accidentally reused.

## Required Changes

1. Confirm the listed helpers have no active imports or package consumers.
2. Remove unused helper implementations and their package exports.
3. Keep active cache fingerprints and JSON serialization paths unchanged.

## Protected Invariants

- Preserve atomic generated-file replacement, unchanged-content write elision,
  paired export/manifest cache invalidation, corrupt-cache regeneration,
  missing-output regeneration, pending stale-output cleanup, and output locks.
- Do not introduce a replacement fingerprint format or change generated data.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/file_helper.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/json_helper.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/__init__.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- Removed helpers have no remaining implementations, imports, or package
  exports.
- Existing cache, file-I/O, locking, and generation tests continue to pass.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
