# Task Node And Retraction Plan

Summary: Rebuild task execution ownership and dependency retraction around task nodes while preserving Durin result, cancellation, executor, and shutdown contracts.

Last reviewed: 2026-10-14

Status: Completed
Completed: 2026-10-14

## Current Status

All stages are complete. The working tree started clean at `c6d8336ef`.
The UE 5.8 task implementation has been inspected locally: retraction consumes
backlinks, attempts execution and nested work, and waits on state changes.
The rejected wait-level graph cache prototype has been removed.
Stage 1 passed 166 Core concurrency cases, isolated queue cases, the workspace
`all` build and validation of 163 documentation files.
Stage 2 passed 167 Core concurrency cases and the workspace `all` build.
Retraction phase counters reuse existing weak graph metadata; they avoid a
second dependency vector or per-wait graph revisions.
Stage 3 passed all five affected native targets (including the full Core
concurrency suite), all seven new cases in separate processes, 189 asset
package cases, 150 texture cases and 14 RHI pipeline creation cases. The final
workspace `all` build passed. Documentation and all-plan validation passed.
The implementation retains the platform thread backend and typed result storage;
no timing or GPU qualification was requested or claimed.

## Goal

Make a task node the execution and lifetime authority. Remove task-specific
queue wrapper allocations, separate execution prerequisites from completion
prerequisites, retire completed retraction phases, and replace repeated graph scans
and polling with notified progress. Preserve existing public task/event APIs,
unique result claims, shared immutable views, failure identity, executor
restrictions, generation/coalescing policy, and callable retirement before
terminal visibility. Module admission and unload policy remain with owners.

## Selected Design

- Use an internal queued execution-node interface shared by ordinary queued
  callbacks and scheduler task nodes. A task node participates directly in the
  priority lanes; generic queued callbacks use an adapter.
- Keep queue ownership and node execution ownership explicit. Queue removal and
  cancellation cannot release user captures under queue or scheduler locks.
- Preserve distinct execution and nested-completion counts before considering
  compact atomic state encoding. Avoid a wait-level graph revision cache.
- Keep immutable dependency relationships for cycle/authority checks and
  diagnostics. Separately retire retraction backlinks when execution or nested
  completion no longer needs them. Failed attempts retain unfinished backlinks:
  unlike UE, Durin must support late binding and mixed-executor progress without
  discarding the route a later waiter needs to reach new work.
- Audit dynamic binding consumers, then enforce one-time binding with checked
  rejection before mutation. Preserve unique-inner cancellation forwarding and
  shared observer isolation.
- Retraction performs a complete postorder attempt with a reusable local stack.
  A helped body exposes its newly added nested dependencies before returning to
  the wait loop. Other-executor and external progress use notifications.
- Evaluate typed callable/result co-location against the existing producer and
  unique/shared ownership interfaces in Stage 0; record the selected allocation
  boundary before changing their layout. Correctness and observable lifetime
  guarantees take precedence over speculative lock-free state compression.
- Retain the current `FRunnableThread` backend and existing executor authority.

## Required References

### Allocation and validation decisions

The scheduler node owns the erased callable until execution/cancellation claims
it. Result storage remains separately owned: unique claim reservations, shared
views and completion sources outlive execution independently. Co-locating that
storage would require an unrelated result API migration and could retain the
whole task through shared results. This plan removes the queue wrapper and its
callable allocation; it preserves the existing callable/result boundary and
its retained-byte accounting. No platform thread replacement is required.

All workspace source/test consumers have been searched. Dynamic binding has no
replacement consumer. Regression gates include admission allocation rollback,
queue discard/idle retirement, unique claim rollback, shared result and failure
identity, nested completion, late dynamic binding, executor restrictions,
timed waits and scheduler shutdown. Each implementation stage requires Core
concurrency coverage and the workspace build; final validation additionally
includes affected targets, asset/RHI integration and isolated new cases.

- [Task system contract](../Runtime/Core/TaskSystem.md)
- [Build workflow](../Agents/BuildAndRun.md)
- [Test workflow](../Agents/Testing.md)
- [Documentation workflow](../Agents/Documentation.md)
- [C++ standards](../Development/Standards/CodingStandards.md)

## Implementation Stages

### Stage 0: Establish ownership and migration contracts

Outcome: allocation ownership, dynamic-binding consumers, and publication
boundaries have explicit, implementable migration decisions.

- [x] Audit queue, scheduler, callable, result, producer and terminal-hook ownership.
- [x] Search consumers across every project in `Durin.dworkspace`.
- [x] Select and record the typed payload allocation boundary and migration scope.
- [x] Define regression gates for cancellation, claim rollback, retained results,
      terminal hooks, executor restrictions and shutdown.
- [x] Validate the new plan with the all-plan validator.

### Stage 1: Unify execution nodes and ownership

Depends on Stage 0. Outcome: scheduler tasks enter the queue as execution nodes
without separate task-specific queued callables or weak queue handles.

- [x] Introduce the internal execution-node interface and generic callback adapter.
- [x] Integrate queue links and execution dispatch with task nodes.
- [x] Implement the selected typed callable/result ownership migration.
- [x] Preserve outside-lock destruction, allocation rollback and diagnostic accounting.
- [x] Pass owning concurrency tests, relevant isolated lifecycle tests and `all` build.
- [x] Update the runtime contract and commit with exact Plan and Stage trailers.

### Stage 2: Define consumable dependency and publication protocols

Depends on Stage 1. Outcome: execution, nested completion and one-time dynamic
binding have explicit invariants; retraction phases are retired independently of
persistent graph metadata.

- [x] Use phase counters to retire backlink traversal without removing cycle/authority metadata.
- [x] Enforce one-time dynamic binding transactionally and migrate all consumers.
- [x] Notify waiters on relevant ready, binding, body-retirement and terminal changes.
- [x] Preserve cancellation acknowledgement, nested completion and terminal-hook retirement.
- [x] Pass dependency, failure identity, cancellation, publication and shutdown regressions.
- [x] Update the runtime contract and commit with exact Plan and Stage trailers.

### Stage 3: Simplify retraction and notified waiting

Depends on Stage 2. Outcome: a wait uses a local postorder work stack
and notified progress instead of rebuilding and scanning the graph per helped body.

- [x] Execute a complete retraction pass while preserving executor authority and deadlines.
- [x] Reuse local stack capacity; skip retired backlink phases and release completed pins promptly.
- [x] Replace polling with a prepare/recheck/wait protocol that cannot miss state changes.
- [x] Cover deep chains, shared DAGs, late binding, newly nested work, unsupported
      executors, timeout, normal-dispatch races and shutdown retirement.
- [x] Pass affected native targets, representative asset/RHI lifecycle regressions,
      isolated new tests, a workspace `all` build and documentation validation.
- [x] Publish lasting contracts, close passed gates and commit with exact Plan and Stage trailers.
- [x] Mark the plan completed and run the all-plan validator after every required gate passes.
