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

The final full 87-scenario run reported **85 passed, 2 failed assertions,
0 timeouts, and 0 skipped**. It rebuilt the server and ran with
`-ContinueOnFailure`. Only `magic_training_post_hunt` and
`magic_training_post_hunt_no_overflow` failed. These known gameplay-policy
mismatches are deferred from the Linux migration; the suite is not fully green.
Hunt-area arrival, all navigation and corpse cases, and the corrected economic,
restart, and depot fixtures passed this run.

## Remaining work outside portability

Do not skip these scenarios silently or loosen their assertions to make the
migration green. Reproduce them independently before changing gameplay policy.

| Scenario | Observed problem / next decision |
| --- | --- |
| `magic_training_post_hunt`, `magic_training_post_hunt_no_overflow` | The controller visits the depot before arbitration. Tests expect immediate post-hunt `Idle` arbitration; the detour also changes mana overflow. Decide intended hunt-end behavior first. |

The first isolation follow-up validated `mainland_loop`,
`real_depot_rejected_move`, `pickup_progression_bundle`, `goal_arbitration`,
`magic_training_restart`, and `healing_resupply` with per-scenario environment
reset/restore. `death` initially timed out because its prior killer survived at
the depot. After fixture-owned killer cleanup and milestone-gated third death,
the isolated death rerun passed in 34.6 seconds with its 45-second limit unchanged.

The depot verifier now checks exact inventory while the existing Depart fixture
checkpoint holds the controller paused, before optional liquidation. Both normal
cycles and all five restart recoveries require the Lua pass marker. Recovery
recreates only the server (`--no-deps`), so provisioning cannot refill equipment
slots. The paused Lua success marker is explicitly flushed to the log pipe.
All six normal/restart scenarios passed the final targeted run; partial and
rejected moves passed the preceding run. Production depot/selling policy is
unchanged.

The economic-fixture follow-up passed `magic_training_progression`,
`magic_training_reserve`, and `magic_training_service`. Progression now starts
with 600 total gp, ten selected potions, and two meat; the nearby currency reward
is suppressed. This proves learning-goal priority, not completed spell payment.

The low-wealth contract deliberately separates banking from liquidation: it seeds
56 bank gp rather than requiring 50 gp plus rabbit-sale proceeds. Its initial
zero-capacity setup could not recover the 30 oz threshold after depositing only
the displaced 25 oz club. Removing capacity pressure instead allowed unrelated
reward/equipment work to run first. The final fixture starts with 10 oz free,
forcing readiness service while allowing the club deposit to restore 35 oz.
Local regressions and the final live rerun passed. The live scenario completed
in 22.3 seconds, verifying the exact 56 gp withdrawal, retained upgrade, hunt
resumption, and absence of sales or terminal events. Reproduce with:

```sh
pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario combat_readiness_low_wealth -SkipBuild
```

Docker approval is required on the current machine; no permission workaround
is part of this work.

Earlier runs exposed hunt-area patrol safety and ordinary-corpse attacker
classification failures. Both scenarios passed the final full run. Retain these
observations if either recurs; a single passing run does not establish timing
stability.

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
