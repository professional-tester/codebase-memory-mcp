# ADR-0001: Coordination daemon for one shared Zova database

- **Status:** Proposed (pending maintainer approval)
- **Date:** 2026-09-04
- **Relates to:** [#16](https://github.com/0ctacity/codebase-memory-mcp/issues/16), parent [#4](https://github.com/0ctacity/codebase-memory-mcp/issues/4)
- **Baseline:** upstream `DeusData/codebase-memory-mcp` `v0.10.8` (`46ae198f`); fork `main` (`68ec1b9`)
- **Scope:** architecture decision for #16. No daemon code merges under #16; the per-workspace
  lease proceeds only through the child tasks listed at the end, each with its own review.

## Decision

**Add an exclusive per-workspace lease around the entire indexing pipeline. Retain the
database-wide writer gate for every actual Zova mutation and every whole-file operation.**

1. **Per-workspace indexing lease — new.** One exclusive lease per `workspace_id`, held for the
   whole index run: extraction, LSP and semantic passes, CBM prepared-view construction,
   publication, and teardown. Acquired before the pipeline starts; released after publication
   commits or fails.
2. **Database-wide writer gate — unchanged.** Continues to guard every real Zova mutation
   (publish, delta, delete-workspace, quarantine, migration) and every whole-file operation
   (compact, restore, import, repack). It is **not** narrowed, relaxed, or replaced. The lease
   governs *work*; the gate governs *database mutation*. They are layered, not alternatives.
3. **Concurrency target.** Different workspaces prepare concurrently and serialize only at
   publication. A second request for a workspace already being indexed does not start a
   duplicate run — it waits or fails fast instead of running a pipeline whose result will be
   superseded.
4. **Deferred.** Build-cohort admission and shared watcher/UI ownership are out of scope until
   their compatibility and liveness contracts are separately designed.
5. **Daemon process — rejected** for now, per the analysis below. Revisit only if the gates in
   [Revisit triggers](#revisit-triggers) are met.

### Why the daemon itself is rejected

**Upstream's daemon does not own database connections.** `frontend.h:1-3` calls the client a
"Stateless stdio bridge", and `runtime.h:101`'s `active_connections` counts *IPC* connections.
The daemon is a work and lifecycle broker, not a connection pool. The premise embedded in #16
("ownership and lifetime of the shared database connection pool") does not hold upstream, so
adopting the daemon would not fix this fork's per-call open cost — the one performance problem
the daemon is often assumed to solve.

### Why the lease is the right scope

The defect this fixes is real and specific: today two processes indexing the same workspace both
run to completion and publish serially, and **last writer wins with the loser's entire run
wasted** — minutes of CPU and multi-GB RSS under the supervised-worker model. There is no
stale-generation check in the publish path. The lease moves that failure to the *start* of the
run, where it costs nothing.

Note what this preserves: the pipeline already runs **outside** the writer gate — the gate is
acquired only inside publish (`cbm_zova.c:6838`, `:6960`), never in `mcp.c` or `main.c`. So
"prepare concurrently, publish serially" is already the publish-side behaviour; the lease adds
the missing **same-workspace** exclusivity without disturbing it.

---

## Context

### What upstream built

Upstream `src/daemon/` is 24 files, 891 KiB of C (`ipc.c` 265 KiB, `runtime.c` 149 KiB,
`application.c` 148 KiB), plus 10 test files totalling 929 KiB. Per upstream `README.md:112-142`
it provides:

- One per-account daemon shared across all agent sessions; owns **watchers, shared indexing
  jobs, and the UI HTTP server** ("concurrent agent sessions do not start duplicate HTTP
  servers", `README.md:142`).
- **Job/watch subscription coalescing**: the first subscriber starts the physical resource,
  later subscribers join it (`daemon.h:96-109`).
- **Leased clients** with heartbeats and expiry (`daemon.h:87-94`).
- **Crash-safe exact-build admission** across every CBM process — same version, executable
  build, coordination ABI, and canonical cache root (`README.md:126`,
  `version_cohort.h:64-69`); conflicts are logged to `daemon-conflicts.ndjson`.
- An **activation barrier** so `install`/`update`/`uninstall` can drain coordinated processes
  (`README.md:128`, `version_cohort.h:78-92`).
- **Shared project mutation leases** (`project_lock.h:17-29`): `SH(project-set) + EX(project)`,
  or `EX(project-set)` for the `"*"` wildcard.

Critically, `frontend.h:1-3` describes the client side as a **"Stateless stdio bridge"**, and
`runtime.h`'s `active_connections` (`runtime.h:101`) counts *IPC* connections. Tool execution
and database access stay in the client process. There is no shared database connection pool.

### What this fork has today

| Concern | Current implementation |
|---|---|
| Cross-process writer exclusion | One database-wide `fcntl`/`LockFileEx` lock, `<cache>/cbm.zova.writer.lock` — `src/zova/cbm_zova_writer_gate.c:49-86` |
| Crash safety | OS-released on process death; proven by `SIGKILL` test — `tests/test_zova.c:127-200` |
| Per-call database access | `resolve_store()` reopens the catalog **and** another store on the full-authority route, with no caching short-circuit — `src/mcp/mcp.c:1126-1143` |
| Prepared statements | Lazy (`src/store/store.c:218-234`), therefore **discarded every call** because the store is closed every call |
| Store close | Passive WAL checkpoint on every close — `src/store/store.c:990-1005` |
| In-process index serialization | Ticket FIFO queue — `src/mcp/mcp.c:4763-4787`; pipeline lock — `src/pipeline/pipeline.c:54-74` |
| Watcher | One polling thread **per MCP server process** — `src/main.c:788-801` |
| UI HTTP server | One thread **per MCP server process**, bound to `127.0.0.1:9749`; a second bind logs `ui.unavailable` `in_use` and gives up — `src/ui/http_server.c:1784-1793` |
| Version/format gate | Per-open check: format `9` + schema `7` ⇒ compatible, format `7` + schema `5` ⇒ repack, else incompatible — `src/zova/cbm_zova.c:1350-1381` |
| Whole-file operations | `compact`, `restore`, `import`, `repack`, migration all replace the entire `cbm.zova` file — `README.md:472-476` |
| IPC substrate | **None.** No sockets, named pipes, pid files, runtime dir, or `CBM_RUNTIME_DIR` anywhere in `src/` |

---

## Required analysis (per #16)

### 1. Ownership and lifetime of the shared database connection pool

**There is no pool to own, upstream or here.** The daemon owns IPC connections, not database
handles. Adopting it therefore leaves the fork's real waste untouched: three open/close cycles
per tool call (`mcp.c:1126-1139`), a discarded prepared-statement cache, a passive checkpoint
per close (`store.c:1004`), and an O(all workspaces) catalog scan that recomputes
shortest-unique selectors on every open (`src/zova/cbm_zova_repository.c:400-472`) — a cost
that **grows with the number of indexed repositories**, which is new and specific to the
shared-database design.

A pool is also actively hostile to this fork's operations model: `compact`, `restore`,
`import`, and `repack` **replace the entire database file**. Every pooled handle would need
eviction before the swap and invalidation of prepared statements, graph sessions, catalog, and
selectors after it. `src/zova/cbm_zova_operations.c:2333-2336` already documents a
self-deadlock hazard in the single gate, requiring a release/reacquire dance — evidence that
this codebase pays real complexity for cached/long-lived state against a swappable file.

**Conclusion:** handle caching is a separate decision with its own eviction protocol. It is not
a reason to adopt a daemon.

### 2. Workspace-level versus database-wide writer admission

Today admission is **database-wide and coarse**. Every publish, delta, delete, compact,
restore, repack, quarantine, and migration step takes the same exclusive lock
(`cbm_zova.c:6838`, `:6960`, `:7046`; `cbm_zova_operations.c:1151`, `:1326`, `:1447`, `:1826`,
`:2301`, `:2357`; `cbm_zova_migration.c:954`, `:1261`, `:1336`).

Consequences:

- Two processes indexing **different** projects block each other unnecessarily.
- Two processes indexing the **same** project both run to completion. There is no stale-generation
  check in the publish path, so the result is **last-writer-wins with the loser's entire index
  run wasted** — minutes of CPU and multiple GB of RSS under the supervised-worker model.

Upstream's `project_lock.c` (7.6 KiB) solves exactly this, and its header states the leases are
**"Shared daemon/local-CLI"** — the mechanism is file-lock based and works without a daemon.
This is the highest-value, lowest-risk piece of upstream's coordination work for this fork.

**Conclusion:** adopt a per-workspace lease scoped to the *pipeline*, and leave the
database-wide gate exactly as it is — covering every mutation, not only whole-file operations.
The two answer different questions: "may I do this work?" (lease, per workspace) and "may I
mutate this database?" (gate, database-wide).

### 3. Client authentication, binary fingerprint, ABI, schema, pinned-Zova compatibility

The fork currently detects incompatibility **only at open time and only for on-disk format**
(`cbm_zova.c:1350-1381`). It has no notion of binary build identity, coordination ABI, or
cache-root identity. Because `workspace_id` is content-derived from the normalized root
(`cbm_zova.c:85-98`), *reads* by a mixed-version binary are already safe — the database is
self-describing. Only **writers** need exact-build admission.

The fork has one dimension upstream lacks: a **pinned Zova SDK and ABI**. Concentrating writes
in a daemon would make the daemon the single arbiter of that pin, which cuts both ways — one
authority versus a new version-skew axis between daemon and clients, and a daemon that can age
past its own pin across fork updates.

Upstream's policy is *stricter than this fork needs*: it refuses any process whose version,
build, or ABI differs, and requires all active processes to match. That is correct for
upstream's daemon-owned-services model, but for this fork it would turn a partial update into a
hard refusal for work that is demonstrably safe.

**Conclusion:** adopt identity *detection and explicit refusal for writers*, not exact-build
admission for all participants. Record binary fingerprint, protocol/store/feature ABI,
cache-root fingerprint, and pinned-Zova commit in a lock file; map upstream's
`cbm_daemon_hello_status_t` failure modes (`service.h:38-47`) onto fork-specific refusals.

### 4. Concurrent MCP, CLI, watcher, UI, backup, restore, and compaction

Two real defects, both caused by per-process ownership:

- **Duplicate watchers.** One watcher thread per MCP server process (`main.c:788-801`), each
  polling independently with per-project state. Two agent sessions on one repository detect the
  same change twice and each fires an index. Dedup is per-process only
  (`src/watcher/watcher.c:565-651`); the in-process pipeline lock does not span processes, and
  the watcher's non-blocking `try_lock` skip (`main.c:169-203`) cannot see another process's
  index.
- **Duplicate UI.** The second session's bind on `:9749` fails and is logged as
  `ui.unavailable` / `in_use` (`http_server.c:1784-1793`). It degrades cleanly, but the user
  silently gets one UI for N sessions, and which one is undefined.

These are precisely what upstream's daemon fixes (`README.md:114`, `:142`). They are the
strongest argument for adoption.

Counter-argument: backup, restore, compaction, and import become **harder** under a long-lived
owner. They replace the whole file (`README.md:472-476`), so a daemon holding state would need
a drain-and-quiesce protocol — upstream's `version_cohort_reserve_for_mutation`
(`version_cohort.h:78-92`) — before any of them can run. Today they are trivially safe because
nothing is cached across calls.

**Conclusion:** the duplicate-watcher and duplicate-UI problems are worth solving, but a
cross-process **claim record in the database or runtime directory** solves them at a fraction of
the cost. Only coalescing an *in-flight index across sessions* genuinely needs a broker.

### 5. Crash recovery, stale sockets, version cohorts, and upgrade handoff

The fork's current position is unusually strong. The writer lock is an OS lock
(`cbm_zova_writer_gate.c:74-82`), and `tests/test_zova.c:127-200` proves by `SIGKILL` that a
crashed holder cannot wedge the database. Recovery composes with the generation and health
model (`cbm_zova.c:2083-2185`; `README.md:478-482`).

A daemon replaces a kernel-guaranteed invariant with application-level machinery: socket and
pid lifecycle, stale-endpoint detection, pid reuse, lease expiry (`daemon.h:93`), daemon-crash
detection, cohort re-admission on restart, and a handoff protocol for fork upgrades. Upstream
paid for this with 891 KiB of C, 929 KiB of tests, and Windows DACL hardening
(`3e540aac`, `b91abca1`, `47bd4b68`) — and Windows is a first-class target for this fork.

**Conclusion:** this is a strict regression in failure-mode simplicity unless the
duplicate-work problem is proven severe. Not worth buying speculatively.

### 6. Cross-platform IPC and installer implications

Greenfield. `src/foundation/` contains no lock registry, no private-file-lock helper, no socket
or pipe abstraction, and no `CBM_RUNTIME_DIR` (upstream's `project_lock.c` depends on
`foundation/lock_registry.h`, which this fork does not have). Adoption means introducing:
endpoint discovery, an owner-only runtime directory with correct Unix modes and Windows DACLs,
a framing protocol (`daemon.h:12-32`), HELLO negotiation, heartbeats, drain semantics, and
Windows named-pipe equivalents.

The installer is fork-owned (#18) and would need `daemon start/stop/status`, an activation
barrier, and "restart your agent sessions" UX. CI (#21) would need a daemon packaging decision
and lifecycle coverage on every supported target.

**Conclusion:** the substrate cost, not the daemon logic, is the dominant term.

### 7. Measured startup and steady-state benefit

**No baseline exists.** `README.md:253-267` is explicitly *"not measurements of the current
Zova full-authority route"* (`README.md:255`), `docs/BENCHMARK.md` has no startup section, and
there is no cold-start or per-call-open measurement anywhere in the repository.

Steady state would get *worse* in one dimension: a daemon adds a process, an IPC hop, and a
heartbeat timer to every request path. The benefit — coalescing duplicate watchers, indexes,
and UI — only materializes for users running several agents against the same repositories.

**Conclusion:** do not decide on performance grounds until a baseline exists. Instrument first.

---

## Design: the per-workspace indexing lease

### Identity

Key the lease on `workspace_id` — `w1_<first 32 hex of sha256(normalized root)>`
(`cbm_zova.c:85-98`). It is content-derived, so independent processes derive the same identity
with no registry, no negotiation, and no path-alias or case-folding ambiguity. Keying on the
project name instead would reintroduce the collision handling that
`cbm_zova_repository.c:400-429` already solves.

### Primitive

Generalize `cbm_zova_writer_gate.c:49-149` into a reusable file lock (`acquire` / `try_acquire`
/ `release` over an arbitrary path) and build the writer gate and the workspace lease on top of
it. Same primitives as today: `fcntl(F_SETLKW|F_SETLK)` on POSIX, `LockFileEx` on Windows.

**Crash safety is the non-negotiable property.** The OS releases the lock when the holder dies,
which is what `tests/test_zova.c:127-200` proves by `SIGKILL`. The new lock must have an
equivalent test.

### Location

`<cache>/cbm.zova.ws-<workspace_id>.lock`, mode `0600` — a sibling of the existing
`cbm.zova.writer.lock`, so no new directory and no new directory-permission code. Files are
created on demand and never removed: deleting a lock file while another process holds or is
about to hold it is a correctness bug, not a cleanup nicety. Record this explicitly rather than
adding a reaper.

### Acquisition semantics

| Path | Behaviour |
|---|---|
| Explicit `index_repository` (MCP or CLI) | Blocking wait, bounded by a default and overridable (`CBM_INDEX_LEASE_TIMEOUT_S`), honouring cancellation |
| Watcher (`main.c:159-203`) | Non-blocking `try_acquire`; on contention log and skip, retry next poll — today's `cbm_pipeline_try_lock` skip, now correct across processes |
| `delete-workspace`, incremental (`CBM_MODE_INCREMENTAL`) | Same lease — both are mutations of the workspace |

Refusal must be machine-readable and name the workspace, following the existing
`CBM_ZOVA_OPERATION_BUSY` + `reason` convention (`cbm_zova_operations.h:11-28`).

**Cancellation:** do not use a raw blocking `F_SETLKW`, which cannot be interrupted
cooperatively. Wait as a bounded retry loop — non-blocking `F_SETLK`, short sleep, cancellation
check — so an MCP cancellation returns promptly.

### The supervised worker must not re-acquire

`cbm_index_spawn_worker` (`index_supervisor.c:147-275`) runs the pipeline in a child process
while the parent blocks in `cbm_subprocess_run`. POSIX record locks are per-process and are
**not** inherited by the child, so a worker that re-acquired the same lease would conflict with
its own parent and fail.

Rule: **lease ownership belongs to the process that received the request; a worker inherits the
right to work and never re-acquires.** `cbm_index_worker_active()`
(`index_supervisor.c:39-41`) already supplies the in-process signal, set from `--index-worker`
(`main.c:346-348`).

Both supervisor outcomes are covered, because the parent holds the lease either way — including
the spawn-failure fallback that degrades to in-process (`index.supervisor.spawn_failed`,
`index_supervisor.c:237-239`).

### Required consequence: the in-process pipeline lock must become per-workspace

`g_pipeline_busy` (`pipeline.c:54-74`) is a single process-wide atomic, commented "Prevents
concurrent pipeline runs on the same DB file", and `handle_index_repository` takes it around
`cbm_pipeline_run` (`mcp.c:4916-4923`).

Left as-is it serializes pipelines for **different** workspaces inside one server process, and
the concurrency target in Decision §3 would be false. Since the cross-process lease already
provides same-workspace exclusion, `g_pipeline_busy` becomes redundant for its stated purpose.

Recommendation: remove `cbm_pipeline_lock/try_lock/unlock` from the index path and make the
lease the single pipeline-exclusion authority, keeping the in-process lock only for the state it
genuinely protects (`srv->active_pipeline`). Every call site needs its workspace keyed:
`main.c:90,169,187,195,201` (watcher), `mcp.c:2853,2884` (delete), `mcp.c:4919,4923`
(index), `mcp.c:6913,6915` (auto-index).

### Ordering

Only one lock is held at a time in this design — the lease for the whole run, the gate only
inside publish — so no lock-ordering cycle is introduced. Constraint to enforce in review:
**never acquire the workspace lease while holding the writer gate.** The existing
release/reacquire hazard at `cbm_zova_operations.c:2333-2336` lives entirely inside the
operations module, takes no workspace lease, and stays untouched.

### Contract position: active readers and file replacement

The gate covers whole-file operations, but a pipeline for workspace `W` may now prepare while a
`restore` or `import` replaces the entire database, then publish into the replaced file. Upstream
prevents this with an `EX(project-set)` wildcard that blocks every named project
(`project_lock.h:18-20`).

**Interim contract (this decision).** A workspace lease does not exclude whole-file replacement.
The window is acknowledged and bounded as follows:

- A publish that lands after a replacement produces a generation that is internally coherent for
  that workspace — it is a valid, complete generation — but it may resurrect content the restore
  intended to remove. It is never a partial or corrupt generation, because publication remains
  atomic under the gate.
- The operator-facing rule is therefore a documented limitation, not a silent hazard: **do not
  run `restore`, `import`, or `repack` concurrently with indexing.** This belongs in the
  operations documentation and release notes.
- No active reader is affected: readers hold no lease, resolve a generation at open time, and a
  replaced file simply invalidates their next open.

**Recommended follow-up** (separate decision, not this one): whole-file operations additionally
take a database-wide preparation barrier that excludes new pipeline leases. Not folded in here
because it changes `restore`'s failure semantics, which today only has to exclude writers.

### Contract position: watcher and UI ownership

**Interim contract (this decision).** There is no ownership claim and no transfer.

- **UI:** the first process to bind owns `127.0.0.1:9749` for its lifetime. Any later session
  logs `ui.unavailable` / `in_use` and continues without a UI (`http_server.c:1784-1793`). This
  is the existing behaviour, now stated as the contract rather than an accident.
- **Watcher:** one watcher per MCP server process, uncoordinated across processes. Two sessions
  watching one workspace both detect a change and both request an index.

Duplicate detection is tolerated under this contract **only because the per-workspace lease makes
the second request fail fast instead of running**. Without the lease this contract would be
unacceptable — it would waste a full pipeline. With it, the cost of a duplicate watcher is a
redundant poll.

Liveness transfer — what happens when the owning session dies — is the hard part, and it is what
pushes this toward a supervisor rather than a claim record. Deferred by design; see the deferred
child work below.

### Invariants to verify

- Atomic publication, generation, and workspace-health semantics are unchanged.
- **Parity:** the Zova and pure-SQLite compatibility routes must refuse identically on lease
  contention.
- **Zero unexpected fallbacks:** if the lock file cannot be created (read-only cache,
  permissions), fail with an explicit error — never proceed uncoordinated.
- Incremental publication and `delete-workspace` take the same lease.
- Cross-repository selection and project prefiltering are unaffected: the lease never changes
  what is published, only who may publish it.

### Tests

- Two processes, same workspace: second waits or fails; no duplicate run.
- Two processes, different workspaces: both prepare; publication serializes; both generations
  commit correctly.
- `SIGKILL` the lease holder: lease released, next acquisition succeeds (mirror
  `test_zova.c:127-200`).
- Supervised worker path: lease held by the parent across the worker's lifetime; worker does not
  self-deadlock.
- Watcher contention: skip logged, retried next poll.
- MCP cancellation during a lease wait returns promptly.
- Contention error is machine-readable through MCP.

## Options considered

| Option | Cost | Fixes | Breaks |
|---|---|---|---|
| **Adopt** upstream daemon | 891 KiB C + 929 KiB tests + greenfield IPC/runtime-dir/installer/CI; Windows DACL work; ~1.0 MiB of new regression surface | Duplicate watchers, duplicate UI, duplicate concurrent indexes, cross-process cancellation scope, version skew, activation barrier | Replaces OS-guaranteed crash safety with leases; complicates whole-file replacement; contradicts "zero infrastructure" (`README.md:215`); new version-skew axis against the pinned Zova SDK |
| **Per-workspace lease only** (chosen) | One file-lock primitive + lease integration across six call sites; no IPC, no runtime dir, no new process | Last-writer-wins duplicate index runs; wasted multi-GB pipelines | Requires retiring the global `g_pipeline_busy`; adds one lock file per workspace |
| **Reject** entirely | None | Nothing | Leaves duplicate index runs unaddressed |

---

## Consequences

### Positive

- A duplicate index of the same workspace no longer wastes an entire run. The loser waits or
  fails fast with an explicit refusal instead of publishing a generation that is immediately
  superseded.
- Different workspaces keep preparing concurrently; only publication serializes, exactly as it
  does today.
- The database-wide gate is untouched, so every existing mutation, whole-file operation,
  generation, and recovery guarantee keeps its current semantics and its current tests.
- Watcher contention becomes correct across processes: today's skip-and-retry covers only
  in-process pipelines.
- No new standing process; `README.md:215` ("Single self-contained application, zero
  infrastructure") stays true.
- Crash safety is the same OS-released-lock guarantee (`tests/test_zova.c:127-200`), extended
  to the new lock with an equivalent test.

### Negative / accepted

- **The global in-process pipeline lock must be retired** (`pipeline.c:54-74`). Until it is,
  different workspaces still serialize inside a single server process, and the concurrency
  target is not met. This is required work, not optional cleanup.
- A blocked `index_repository` now waits (or fails) where it previously ran and lost. Clients
  that fire an index on session start will see this; the refusal must be actionable.
- One lock file per workspace accumulates in the cache directory and is never removed.
- A pipeline may prepare while a `restore`/`import` replaces the database — see
  [Open question](#open-question-whole-file-operations-vs-in-flight-preparation).

### Deferred, not fixed

- Build-cohort admission: an incompatible binary is still detected only at open time and only
  for on-disk format (`cbm_zova.c:1350-1381`).
- Shared watcher and UI ownership: duplicate watchers across agent sessions remain, and the UI
  stays first-session-wins (`ui.unavailable` / `in_use`, `http_server.c:1784-1793`). The
  duplicate index a second watcher triggers is now refused fast, so the cost drops from "a
  wasted multi-GB pipeline" to "a redundant poll".
- Cross-process cancellation scope.
- Per-call reopen cost (three opens per tool call, discarded prepared statements,
  `mcp.c:1126-1143`).

---

## Threat and failure analysis

| Threat | Under this decision | Under daemon adoption |
|---|---|---|
| Crashed writer wedges the database | Impossible — OS releases the lock (`cbm_zova_writer_gate.c:74-82`), tested | Possible — needs lease expiry, liveness detection, and cohort re-admission |
| Stale IPC endpoint / pid reuse | N/A — no endpoint | New: client may connect to a dead or wrong generation owner; upstream's answer is owner-only DACLs plus build fingerprints |
| Lease holder hangs (not crashes) mid-pipeline | New, bounded: the waiter's bounded retry loop times out with an explicit error. Unrecoverable within the process, but never wedges the database | Same, plus a stuck daemon affects every session |
| Supervised worker re-acquires its parent's lease | Prevented by rule — worker inherits the right to work and never re-acquires (`index_supervisor.c:39-41`); a regression here is an immediate self-deadlock on every index | N/A |
| Version skew after partial update | **Deferred / accepted** — detected only at open time and only for on-disk format (`cbm_zova.c:1350-1381`) | Fail-closed HELLO conflict for every participant (`service.h:38-47`); correct, but turns a partial update into a hard stop for all sessions |
| Daemon dies mid-publish | N/A | New failure class: a generation left in `building` must compose with the existing `rebuild_required` / `whole_file_recovery` model (`README.md:478-482`) |
| Whole-file replacement (compact/restore/import/repack) | Gate unchanged; one residual gap — a pipeline may prepare across a replacement (see [Open question](#open-question-whole-file-operations-vs-in-flight-preparation)) | Requires drain, quiesce, and post-swap invalidation of every pooled handle |
| Untrusted local account | Lock file is `0600` in the owner's cache dir; no rendezvous endpoint to attack | IPC endpoint is an account-wide rendezvous; upstream mitigates with owner-only DACLs (`3e540aac`) and randomized private directories |
| Denial of service by over-strict admission | None — this decision adds no identity check | Broad: a mismatch can refuse all sessions until the operator intervenes |

---

## Revisit triggers

Reopen the daemon question if **any** of the following is demonstrated. Each is written to be
measurable so that this ADR cannot quietly become a permanent "no" by inertia.

1. **Instrumented cost is proven material.** A cold-start and per-call baseline exists
   (`resolve_store` open count, catalog scan time, prepared-statement rebuild cost) and shows
   per-call overhead dominating p95 for representative queries. #24's profiling guards can
   carry this instrumentation.
2. **Duplicate in-flight work is observed in practice**, not just in theory: two MCP sessions on
   one project contending for a lease often enough to matter, or repeated `ui.unavailable`
   reports from multi-agent users.
3. **Per-workspace leases fail to relieve head-of-line blocking.** If measured contention stays
   high after the lease lands, the remaining value is broker-side coalescing.
4. **Shared watcher/UI ownership is wanted** and the liveness contract (what happens when the
   owning session dies) turns out to need a supervisor rather than a claim record.

Absent these, the daemon's cost is not justified by any measured problem.

---

## Proposed child work (none starts before this ADR is approved)

### In scope now

1. **File-lock primitive** — generalize `cbm_zova_writer_gate.c:49-149` into a reusable
   acquire/try/release lock over an arbitrary path; rebuild the writer gate on it. Carry over the
   `SIGKILL` crash-safety test (`test_zova.c:127-200`) for the new lock.
2. **Per-workspace indexing lease** — acquire/release across the whole pipeline at every index
   entry point: `index_repository` (MCP and CLI), watcher (`main.c:159-203`), auto-index
   (`mcp.c:6913`), `delete-workspace` (`mcp.c:2853`), and incremental publication. Blocking with
   a bounded, cancellable wait for explicit requests; non-blocking skip for the watcher;
   machine-readable contention error.
3. **Retire the global pipeline lock** — remove `cbm_pipeline_lock/try_lock/unlock` from the
   index path (`pipeline.c:54-74`) so different workspaces prepare concurrently. **This is
   required for the concurrency target, not cleanup.** Keep in-process state protection for
   `srv->active_pipeline`.

### Instrumentation (separate, feeds the revisit triggers)

4. **Baseline counters** — store opens per tool call, catalog scan duration, prepared-statement
   cache misses, and cold-start time. Fits #24's profiling guards. Nothing in this ADR should be
   justified by unmeasured startup cost again.

### Deferred — needs its own design before any work starts

5. **Build-cohort admission** — separate design for the compatibility contract: which identity
   fields (binary fingerprint, protocol/store/feature ABI, cache-root fingerprint, pinned-Zova
   commit), what is refused (writers only, or every participant), and how a refusal is surfaced
   to operators. The liveness question — what happens to in-flight work when a cohort conflict
   is detected — is the hard part.
6. **Shared watcher and UI ownership** — separate design for the liveness contract: ownership
   claim, transfer on owner death, and behaviour for a session that outlives its owner. Requires
   answering whether a claim record is sufficient or a supervisor is needed.

## Zova invariants exercised

- One shared `cbm.zova` database with isolated workspaces — **preserved**; the lease is keyed by
  `workspace_id` and reinforces workspace isolation.
- Atomic publication, workspace health, backup, recovery, fault behaviour — **preserved**; the
  writer gate and the publish path are unchanged.
- Exact parity with the pure-SQLite compatibility route — **must be verified**: both routes must
  refuse identically on lease contention.
- Zero unexpected fallbacks — **must hold**: if a lock file cannot be created, fail with an
  explicit error, never proceed uncoordinated.
- Cross-repository search with exact project selectors — **unaffected**; the lease changes who
  may publish, not what is published.
- Fork-owned installers and pinned Zova SDK — **preserved**; no daemon packaging decision needed.

## References

- Upstream: `src/daemon/` — 24 files, 912,554 bytes; `tests/test_daemon*` — 10 files,
  951,404 bytes.
- Upstream `README.md:112-142` (session coordination daemon), `:126` (exact-build admission),
  `:128` (activation barrier), `:132` (CLI mode never starts the daemon), `:142` (daemon-owned UI).
- Upstream headers read for this analysis: `daemon.h`, `service.h`, `version_cohort.h`,
  `project_lock.h`, `frontend.h`, `runtime.h`.
- Fork: `src/zova/cbm_zova_writer_gate.c`, `src/pipeline/pipeline.c:54-74`,
  `src/mcp/index_supervisor.c:26-41`, `:147-275`, `src/mcp/mcp.c:1126-1143` and `:4763-4787`,
  `src/store/store.c:218-234` and `:990-1005`, `src/zova/cbm_zova.c:85-98`, `:1350-1381`,
  `:2083-2185`, `:6838`, `src/zova/cbm_zova_operations.c:1133-1284`, `:1790-1895`, `:2279-2447`,
  `src/main.c:670-820`, `src/watcher/watcher.c:565-651`, `src/ui/http_server.c:1767-1805`,
  `tests/test_zova.c:127-200`, `tests/test_zova_operations.c:1131-1200` and `:3011`,
  `README.md:22`, `:215`, `:255`, `:472-482`.
