# Linux validation baseline

This records the Linux development migration, not a claim that every gameplay
scenario passes. Commands and prerequisites are in [testing.md](testing.md) and
[client-runtime.md](client-runtime.md).

## Verified scope

- PowerShell 7 runs the shared bootstrap and gameplay driver on Linux.
- Bootstrap preserves the locally built client and checks the pinned asset hashes.
- The server builds in Docker. Fresh database health, bot provisioning, online
  lifecycle telemetry, and local ports 7171/7172 passed the startup check.
- Pure C++ contracts and the depot workflow regression passed with real server
  headers in the Docker builder.
- The client was built and launched locally. This is not full client compatibility
  coverage; in-game testing remains necessary. Client fixes remain separate from
  the tooling work.

The last full 87-scenario run reported **75 passed, 8 failed assertions, and
4 timeouts**. Subsequent targeted runs validate corrections; they do not replace
a fresh full-suite result. The 22-case follow-up passed 20 cases initially.
After fixture corrections, all five depot restart checkpoints and arbitration
passed a seven-case rerun; `real_depot` then passed its final one-case rerun.
Thus all 22 selected cases have passing follow-up evidence, across separate runs.
The selection includes all five full-navigation cases, all three corpse cases,
restart log consumers, and the affected depot/service/progression scenarios.

## Remaining work outside portability

Do not skip these scenarios silently or loosen their assertions to make the
migration green. Reproduce them independently before changing gameplay policy.

| Scenario | Observed problem / next decision |
| --- | --- |
| `hunt_area_arrival` | The selected destination is reachable, but patrol safety rejects the later route. Define bounded safe-region fallback without relaxing danger limits. |
| `combat_readiness_low_wealth` | Sale cargo goes to the depot before service; the fixture expects 56 gp including proceeds but observes a 50 gp withdrawal. Isolate sale/service inputs. |
| `magic_training_progression` | Equipment/service work consumes the funds intended for spell learning. Isolate the learning/affordability contract. |
| `magic_training_post_hunt`, `magic_training_post_hunt_no_overflow` | The controller visits the depot before arbitration. Tests expect immediate post-hunt `Idle` arbitration; the detour also changes mana overflow. Decide intended hunt-end behavior first. |
| `death` | The third fixture kill can precede service discovery after recovery. Replace the fixed delay with a bounded milestone wait. |
| `real_depot` Lua verifier | After deposit completion, optional liquidation can withdraw a tool before the asynchronous inventory verifier runs. Separate deposit-boundary inventory checks from post-departure activity. A passing PowerShell scenario alone does not resolve this verifier race. |

The ordinary `corpse` scenario has both passing and failing traces: a defensive
attacker classification previously prevented normal looting. The latest targeted
run passed all three corpse cases; this does not establish timing stability.

## Test isolation follow-up

Keep PowerShell as the cross-platform runner. Refactor by behavior, not language:

1. Put priority, scoring, reserve arithmetic, and state-transition rules into
   small controlled contract tests where practical.
2. Give engine-path fixtures explicit inventory, rewards, services, and cleanup.
   Checkpoint flags must not leak into the next scenario.
3. Wait for observable milestones with deadlines, not wall-clock guesses.
4. Retain a small real-map smoke suite and selected integration routes. Exact
   coordinates belong in those map-specific tests, not generic planner contracts.
5. Assert outcomes and safety guarantees; require an internal phase sequence only
   when that sequence is an agreed behavior contract.

Preserve exact ownership, persistence, protocol, safety, and timeout guarantees.
Use failures to distinguish broken fixtures from actual behavior regressions.
