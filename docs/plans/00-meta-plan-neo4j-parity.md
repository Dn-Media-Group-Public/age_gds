# Meta-Plan — Neo4j GDS Parity Execution Order

Executes the seven plans produced from `docs/neo4j-gds-parity-report.md`, in
order. Each plan is implemented by a **fresh subagent**, one plan per
subagent, to avoid exhausting context. The parent agent only tracks plan
completion, runs the commit, and moves to the next plan.

> **Deviation from the standard no-commit rule:** the user has explicitly
> authorized this meta-plan to commit to git after each stage. This
> authorization is scoped to this meta-plan only — do not treat it as
> standing permission for unrelated commits.

## Order

1. `docs/plans/00-weight-plumbing.md` — foundation; every later plan depends on it.
2. `docs/plans/01-centrality-parity.md`
3. `docs/plans/02-community-detection-parity.md`
4. `docs/plans/03-pathfinding-parity.md`
5. `docs/plans/04-similarity-linkprediction-parity.md`
6. `docs/plans/05-embeddings-parity.md`
7. `docs/plans/06-new-graph-procedures.md`

Do not reorder or parallelize — plans 1–6 assume plan 0's weight-override
helper exists, plan 6's conductance item assumes plan 2's partition-post
helper exists (or reimplements it if run out of order — prefer running in
order to avoid this).

## Workflow (repeat for every plan)

Launch one subagent per plan (`subagent_type: general-purpose` or
project default — a fresh agent, not a fork, since each needs to read and
implement a specific plan file from scratch). Give it this brief:

```
1. Read the plan file in full.
2. Implement all code changes exactly as specified.
3. Build inside Docker, same pattern as docs/plans/00-meta-plan.md:
   - docker run --rm -d --name age_gds_test -e POSTGRES_PASSWORD=password
     -v "$(pwd):/workspace" apache/age:latest
   - docker exec age_gds_test bash -c "apt-get update -q &&
     apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
     cd /workspace && git submodule update --init --recursive &&
     cmake --build build -j$(nproc) && cmake --install build"
4. Verify NetworKit is linked: must print > 0:
   docker exec age_gds_test bash -c "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"
   If it prints 0, stop and fix the build before continuing.
5. Write/update the test/sql/ cases the plan calls for, run them inside the
   same container, and confirm they pass.
6. Update docs/api.md and docs/architecture.md sections the plan lists under
   "Docs to update", including a "See also" link to the plan once implemented.
7. Move the plan file from docs/plans/<name>.md to docs/implemented-plans/<name>.md
   and update docs/implemented-plans/index.md to link it.
8. docker stop age_gds_test
9. Do NOT commit. Report back what changed, and the exact single-sentence
   description of the category implemented (e.g. "centrality algorithm
   parity fixes") for the parent to use as the commit message.
```

The subagent must **not** commit — the parent agent (this meta-plan) owns
the commit step, so that a consistent commit-message format is enforced and
so the parent can verify the working tree before committing.

## Parent agent steps, after each subagent reports back

1. Run `git status` and `git diff --stat` to confirm the subagent's changes
   match what it reported — do not commit blind.
2. `git add -A` (or targeted paths if anything unexpected shows up in
   `git status` — investigate before staging, per standard git safety
   practice).
3. Commit with a message that is **exactly one sentence**, naming only the
   category implemented, with **no** Claude/AI attribution, co-author
   trailer, or emoji — e.g.:
   - `git commit -m "Implement per-call relationship weight override plumbing."`
   - `git commit -m "Implement centrality algorithm parity fixes."`
   - `git commit -m "Implement community detection algorithm parity fixes."`
   - `git commit -m "Implement path-finding and flow algorithm parity fixes and new procedures."`
   - `git commit -m "Implement similarity and link-prediction parity fixes."`
   - `git commit -m "Implement embedding algorithm parity fixes."`
   - `git commit -m "Implement new graph-theoretic procedures."`
4. Move to the next plan.

## Failure Policy

- A plan is not done until its tests pass inside Docker and the NetworKit
  symbol check prints > 0.
- If a plan's subagent reports a NetworKit gap it didn't anticipate (no
  suitable class, contrary to the plan's assumption), stop that plan, do not
  commit partial/faked work, and report back for a plan revision rather than
  improvising an in-house algorithm.
- If a later plan's tests break an earlier plan's tests, fix the regression
  before committing that later plan.
