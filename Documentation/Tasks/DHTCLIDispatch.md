# Simplify DHT CLI Dispatch

## Outcome

DurinHeaderTool lets argparse dispatch its three active commands directly,
without an unused command-manager abstraction or placeholder command.

This is a bounded CLI cleanup, not a command syntax or behavior redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

The CLI wraps three real commands in `Command`, `CommandManager`, and a
pass-through `run` method while also registering an unused `EmptyCommand`.
Argparse already owns command validation and can dispatch directly.

## Required Changes

1. Dispatch active argparse commands directly to their implementations.
2. Remove `Command`, `CommandManager`, `EmptyCommand`, and package exports that
   no longer have an active caller.
3. Preserve the current command names, arguments, validation, output, and exit
   behavior.

## Protected Invariants

- Preserve all active command syntax and generated outputs.
- Preserve argparse help and invalid-command diagnostics.
- Do not add a replacement dispatch framework or change reflection behavior.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cli/`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- Each active command reaches its existing implementation through direct
  argparse dispatch.
- Removed CLI scaffolding has no remaining imports or package exports.
- Existing CLI and generation tests continue to pass.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
