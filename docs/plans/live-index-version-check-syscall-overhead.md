# Plan: eliminate open/pread/close syscalls from live-index cache-hit version checks

## Problem / Goal

`article-graph`'s query-pipeline benchmark
(`query-pipeline-benchmark.md` in that repo) found that `age_gds.bfs_stream`
against a named, cached projection (`explorer` — a **live** index, per
[`09-live-graph-index.md`](implemented-plans/09-live-graph-index.md)) costs
consistently more per call than the old plain-SQL neighbor query it
replaced — a small, roughly constant ~1.2-2.75x factor (later ~1.5-2.1x at
1M-node scale), not scale-dependent. The benchmark attributed this to
"generic extension-function dispatch... and NetworKit-side bookkeeping to
look up cached graph state" without root-causing the bookkeeping cost
further. This plan root-causes and fixes one concrete piece of it.

Root cause, in `index_storage_read_version` (`src/catalog/index_storage.cpp:222`),
called from `session_cache_get`'s live/incremental cache-hit branch
(`src/catalog/session_cache.cpp:136-155`) on **every single call**, even
when the cached graph is already up to date:

```cpp
uint64_t index_storage_read_version(const char *nodes_path)
{
    int fd = ::open(nodes_path, O_RDONLY);
    ...
    pr_checked(fd, &version, sizeof(version),
               offsetof(NodesFileHeader, version_counter), nodes_path);
    ::close(fd);
    return version;
}
```

This is a fresh `open()` + `pread()` + `close()` — three syscalls, each a
kernel round trip — on every cache-hit call to `bfs_stream` (or any other
algorithm) against a live-indexed graph, purely to check whether the
already-cached in-memory graph is stale. For a non-live/flat graph (Stage 4
mmap path), a cache hit is a single hash-map lookup with no I/O at all; the
live-index path added in Stage 9 pays real kernel-transition cost on every
call regardless of whether anything changed.

This also appears to be a drift from Stage 9's own design intent — that
plan's design section says the cache-hit check should "read
`version_counter` from nodes header" under an `LW_SHARED` lock
(implying a read from an already-mapped/attached region), not reopen the
file from a path string each time. The implementation as shipped does the
latter.

**Goal:** make a live-index cache hit as cheap as a flat-index cache hit —
an in-memory version comparison, no syscalls — while preserving the
existing staleness-detection correctness (a writer's mutation must still be
visible to readers promptly; per Stage 9, on `LW_EXCLUSIVE`).

## Design

Two options, in order of preference:

### Option A (preferred): keep a persistent mmap of the nodes-file header per cached entry

Instead of storing only `nodes_file_path`/`edges_file_path` strings on
`ProjectedGraph` (`session_cache.h:46`) and re-opening them each call, `mmap`
the nodes file once (at cache-insert time, alongside the existing
deserialize-from-mmap step already used to build the graph — Stage 9's
`index_storage_deserialize` already touches this file via mmap for the
initial load) and **keep that mapping alive** for the lifetime of the cache
entry, storing the mapped pointer (or a thin RAII wrapper) on
`ProjectedGraph`. The cache-hit version check becomes a single volatile
memory read (`*(volatile uint64_t*)(mapped_ptr + offsetof(...))`) — no
syscall, correctly observes concurrent writer mutations to the same mmap'd
page (the OS keeps `MAP_SHARED` mappings coherent across processes, which is
exactly the property Stage 9's `LW_EXCLUSIVE`-guarded mutation path already
relies on for the *initial* mmap-based deserialize).

Requires: extend `ProjectedGraph` with an owned mmap handle (fd + pointer +
length, RAII-wrapped, `munmap`+`close` on eviction/destruction — mirror
however Stage 9's initial-load mmap is currently scoped/unmapped, likely in
`index_storage_deserialize`, so the "read header, then unmap" pattern used
there needs to become "read header, keep mapped" for the *nodes* file
specifically — the edges file mmap, if separately scoped, does not need to
stay resident, only the header/version-counter page does).

### Option B (fallback, if Option A's mmap lifetime turns out unsafe/complex): keep a resident file descriptor, `pread` without reopening

If keeping an mmap alive across calls proves awkward (e.g. interacting with
`evict_lru()`'s destruction order, or with concurrent `graph_project`/reindex
replacing the file at the same path), a smaller fix: keep the `fd` from
`open()` resident on `ProjectedGraph` and call `pread` directly on it each
check, skipping `open`/`close` (two of the three syscalls) but still paying
one syscall per call. This is a strictly smaller win than Option A (removes
~2/3 of the syscall cost, not all of it) but is lower-risk if file-replacement
semantics make a persistent mmap unsafe (a persistent `fd` survives the
original file being unlinked/replaced on POSIX; a persistent mmap does too,
as long as the fix consistently opens the *current* path only at
projection-(re)creation time, not per read — needs verification against
however `graph_reindex`/live-index replacement actually writes the file:
in-place mutation vs. write-new-then-rename).

**Decision needed from the user:** which option to implement — Option A
(bigger win, needs verifying file-replacement semantics are compatible with
a long-lived mmap) or Option B (smaller, safer, incremental). Recommend
starting with Option B if there's any uncertainty about how `graph_reindex`
replaces the on-disk file, then revisiting Option A once that's confirmed.

## Implementation order

1. Read `index_storage.cpp`'s write-side (`index_storage_add_node`, `graph_reindex`
   handling, and whatever `age_gds_graph_reindex` does — confirm whether the
   live index ever replaces the nodes file wholesale (rename-over) vs.
   always mutating in place. This determines whether Option A is safe.
2. Extend `ProjectedGraph` (`session_cache.h`) with either an mmap handle
   (Option A) or a resident `fd` (Option B) for the nodes file, RAII-managed
   so `evict_lru()`/cache-entry destruction cleans it up (currently
   `ProjectedGraph` has no custom destructor — `std::unordered_map::erase`
   relies on the struct's own destructor; needs one now, or an RAII member
   type).
3. Rewrite `index_storage_read_version` (or add a sibling function) to read
   from the resident handle instead of `nodes_path` each call. Keep the
   existing `open`+`pread`+`close` version available for any other caller
   that doesn't have a resident handle (e.g. first-load path, or diagnostic
   tools).
4. Update `session_cache_get`'s cache-hit branch (`session_cache.cpp:136-155`)
   to call the new resident-handle version-check instead of
   `index_storage_read_version(pg.nodes_file_path.c_str())`.
5. Add a test that measures cache-hit call cost before/after (a simple
   timed loop of repeated `bfs_stream` calls against a live-indexed graph,
   same idea as `article-graph`'s `diagnose_patha.py` isolated-call
   methodology) — assert the fix reduces per-call syscall count (`strace -c`
   over N calls) rather than asserting a specific timing number.
6. Re-run correctness tests for live-index staleness detection
   (`test/sql/09-live-graph-index.sql` or wherever Stage 9's tests live) —
   confirm a mutation made by a concurrent backend is still observed
   promptly after the fix (this is the property most at risk from getting
   the mmap/fd lifetime wrong).

## Files to change

| File | Change |
|---|---|
| `src/catalog/session_cache.h` | Add resident mmap/fd handle field(s) to `ProjectedGraph`, RAII-managed. |
| `src/catalog/session_cache.cpp` | Cache-hit branch uses the resident handle instead of calling `index_storage_read_version` by path. |
| `src/catalog/index_storage.cpp` / `.h` | Add a resident-handle version-check function; keep (or repurpose) the existing path-based one for first-load/diagnostic use. |
| `test/sql/*live*` (wherever Stage 9's live-index tests live) | Add/extend a staleness-detection regression test; add a syscall-count-based perf assertion if the test harness supports `strace`. |

## Docs to update

- `docs/architecture.md` — if it describes the live-index cache-hit path's cost model, update to reflect the resident-handle version check.
- `docs/reference/session-cache.md` — only if it makes any claim about cache-hit cost; otherwise no change.
- `docs/implemented-plans/index.md` — add this plan once implemented.
