# Testing

See [Linux validation baseline](linux-validation.md) for migration evidence,
known gameplay failures, and the test-isolation follow-up.

Use PowerShell 7+ (`pwsh`) on Windows or Linux. The same scripts, scenario
catalog, and assertions apply on both platforms. Linux needs Docker Engine and
Compose v2; bootstrap also requires a locally built `client/otclient`.
Commands containing `$env:` or `Remove-Item` below run inside PowerShell, not Bash.
The `pwsh -File ...` commands work from either shell at the repository root.

If Docker requires elevation on your Linux setup, run Docker commands and the
gameplay driver from an explicitly authorized elevated shell; the scripts do not
elevate themselves or require changes to socket permissions or group membership.
Elevated test runs can leave root-owned failure artifacts. The suite resets the
disposable `angelion` database and removes its stack unless `-KeepStack` is set;
do not run it alongside a normal development session. A failed daemon-access preflight does not attempt stack cleanup.

The spell contract check needs neither Docker nor elevation:

```powershell
pwsh -File scripts/test-knight-spell-contract.ps1
```

Run the non-Docker telemetry assertion regressions:

```powershell
pwsh -File scripts/test-playerbot-navigation-assertions.ps1
pwsh -File scripts/test-playerbot-readiness-assertions.ps1
pwsh -File scripts/test-playerbot-log-parsing.ps1
pwsh -File scripts/test-playerbot-scenario-isolation.ps1
pwsh -File scripts/test-playerbot-depot-scenario.ps1
pwsh -File scripts/test-playerbot-death-scenario.ps1
pwsh -File scripts/test-playerbot-magic-training-assertions.ps1
```

On Linux, pure contract checks require a C++17 compiler; fixture-isolation checks
require Lua or LuaJIT. Run these from the repository root:

```sh
sh server/tests/playerbot_contracts.sh
lua scripts/test-playerbot-fixture-isolation.lua
lua scripts/test-playerbot-depot-fixture.lua
lua scripts/test-playerbot-death-fixture.lua
```

The depot Lua regression checks login-time inventory expectations, bounded waiting
for the Depart pause, and lost inventory/reserve failures. The PowerShell depot
regression checks pause, verification, and stopped-database rearm ordering.
Recovery uses `--no-deps` to keep provisioning from changing saved inventory;
the Lua success marker is flushed explicitly because the paused bot emits no
later telemetry.
`PLAYERBOT_DEPOT_VERIFIER_PHASE` is Lua-fixture-only: it preserves the original
restart phase's prior-gold expectation while recovery pauses at Depart.

Run the affected live depot scenarios without the longer risk-fallback fixture:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario real_depot,real_depot_restart_approach,real_depot_restart_locker,real_depot_restart_chest,real_depot_restart_deposit,real_depot_restart_depart,real_depot_partial_move,real_depot_rejected_move
```

`server/tests/playerbotdepotworkflow_test.cpp` includes its compile command and
requires the server development headers. These checks supplement, not replace,
live gameplay validation.

## Server smoke test

For server, infrastructure, or cross-stack changes, run:

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs playerbot-setup server
```

Confirm that MariaDB is healthy, the map loads, the server reports online,
ports `7171` and `7172` accept local connections, and `playerbot-setup` exits
successfully. Exactly one valid `Bot One` registration must exist, and server
JSONL must contain a `playerbot` lifecycle record with `status="online"`.

GitHub's `server-smoke` workflow checks the fresh Compose stack, provisioning,
startup, lifecycle output, and ports for `server/**` changes. It does not
execute gameplay actions.

## Gameplay suite

The PowerShell driver builds the server, starts one disposable MariaDB container,
restores the ordered schema and development-character baseline before each
scenario, recreates the server process, asserts against streamed JSONL telemetry,
reports per-scenario timing, and removes the stack afterward:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1
```

The baseline covers NPC discovery, sale and reserve purchase, bank deposit and
withdrawal, fixture-depot handling, and return to hunting. Add switches that match
the changed behavior:

| Switch | Coverage |
| ------ | -------- |
| `-FullNavigation` | Complete fixed A-B-C-B-A route, temporary blockage recovery, real Carlin service routing, mutable shovel portals, and forced patrol-route failure recovery. |
| `-TargetApproach` | Same-floor routed engagement, unreachable-target patrol fallback, and encountered-attacker replacement. |
| `-CorpseLoot` | Non-corpse, empty, guaranteed-loot, and container death items; open-before-inspect ordering, suspended-route retry, and bounded inaccessible-corpse timeout. |
| `-DeathTelemetry` | Death context, exponential relog, fresh controller state, abandonment, and removal. |
| `-Healing` | Potion verification, threshold recovery, missing-stock service, purchase, and resume behavior. |
| `-ValueLoot` | Value-per-weight replacement under constrained capacity plus generic food replacement and collection cap without mandatory food purchase. |
| `-PickupProgression` | Nested and multi-root reward inspection, claim verification, upgrades, restart recovery, and space rejection. |
| `-GoalArbitration` | Pickup, service, hunt, and critical-healing precedence across safe boundaries. |
| `-OracleDeparture` | Tagged Oracle route, dialogue, vocation/town/position verification, and restart persistence. |
| `-StaminaProjection` | Premium bonus, low-stamina penalty, and ordinary stamina projections. |
| `-HuntRegionPlanning` | Cached atlas batching, threat rejection, reachability, selected-return reserve, cooldowns, and observed correction. |
| `-AdaptiveChallenge` | Synthetic frontier and planner-helper fixture covering idle and no-kill exclusion, sparse-combat escalation, hysteresis, recovery backoff, equipment/recovery prediction, lethal rejection, local exhaustion, terminal finality, and no post-terminal controller events. It does not validate real combat sampling or long-running hunt behavior. |
| `-CombatReadiness` | Equipment, the one-potion return threshold and 10-potion restock target, optional-food hunting, generic food consumption and reclaimable capacity, low-wealth banking, carried-upgrade retention through service, and restart reconstruction. It does not cover the terminal case where total funds cannot buy enough potions to exceed the return threshold. |
| `-EquipmentPurchases` | Justified purchase and equip verification, clean restart persistence, carried-upgrade recovery, displaced-item-space rejection, and rejected transactions. |
| `-MainlandRewards` | Real Thais reward object from a teleported, high-capacity fixture; scale-armor claim and equip, displaced-item and bundle preservation, restart reconstruction, and non-null battle-axe rejection evidence. It does not prove normal traversal, realistic capacity limits, or a specific rejection reason. |
| `-Depot` | Real Thais locker/chest discovery from Naji, including exact nearest-locker selection, carried-upgrade equipment, displaced and inferior equipment deposits, one carried rope and shovel, surplus tool deposits, nested loot, move verification, retries, and restart checkpoints. Both normal cycles, all five checkpoint recoveries, and partial moves verify completed inventory while paused at Depart, before optional selling. Rejected moves retain separate exact delta/retry/discard assertions. |
| `-SlottedLoot` | Invalid-slot loot sale through a live seller, direct depot fallback without an eligible seller, protected-equipment retention, move verification, and interrupted-deposit restart recovery. |
| `-SellLoot` | Local and remote-depot liquidation, capacity-bounded manifests, verified withdrawal, seller travel, sale ordering, and proceeds-funded resupply. The workflow excludes fluid containers and splashes; current fixtures do not seed those item types. |
| `-MainlandLoop` | Two real Thais hunt/depot cycles, local services, depot fallback for remote-buyer loot, restart recovery, and teleport exclusion. |
| `-SpellTraining` | Loaded spell-offer trainer discovery, reserve-backed affordability rejection, normal spell dialogue/payment, and restart persistence. |
| `-SpellUse` | Light Healing preemption, Haste, Whirlwind Throw, unlearned-spell potion fallback, and mana-reserve melee fallback. |
| `-SpellCalibration` | Engine-path Light Healing, measured Haste duration, and single-target Whirlwind attribution; deterministic classifier evidence for race-heavy censored/concurrent/ambiguous cases; profile confidence, LRU eviction, bounded values, telemetry, and controller-recreation reset. |
| `-MagicTraining` | Creature aggregated regeneration-forecast boundaries, including finite final ticks, strict overflow/exact-full behavior, PZ pause, one normal Haste/Great Light/Light or Light-refresh cast, service and spell-learning precedence, post-hunt `Idle` arbitration, failed verification, and restart forecast recomputation. Controlled setup proves arithmetic and engine paths, not ordinary long-running frequency; regeneration phase is not preserved across serialization. |

Navigation or looting changes require at least:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot
```

Target-approach changes also require
`pwsh -File scripts/test-playerbot-gameplay.ps1 -TargetApproach -Focused`.
`-TargetApproach` runs `target_approach`, `target_approach_unreachable`, and
`target_attacker_priority`.

Use `-Scenario <name>` for one catalog entry. New focused names include
`sell_loot`, `sell_loot_remote_depot`, `depot_risk_fallback`,
`hunt_region_planning`, `hunt_area_arrival`, `carlin_service_route`,
`mutable_portal_route`, and `patrol_recovery`. `hunt_area_arrival` proves that a
selected real region activates before its first waypoint. `patrol_recovery`
proves bounded forced route-failure handling on a fixed route; it does not inject
an unsafe active-region patrol route or verify reserve growth across waypoints.

Use `-Focused` with one or more scenario switches to skip the baseline. Use
`-SkipBuild` only with a known-current `angelion-server:latest` image; it does
not prove that the image matches the worktree. `-KeepStack` preserves the final
stack for debugging. `-TimeoutSeconds` accepts `30` through `3600` and replaces
each scenario's fail-fast deadline.

`combat_readiness_low_wealth` isolates banking from selling: zero carried gold,
56 gp in the bank, ten selected health potions, a carried upgrade (2384), and
no rabbit sale cargo. Setup leaves 10 oz free capacity (`usedCapacity + 1000`
in native units), below the 30 oz readiness threshold. Depositing the displaced
25 oz club (2382) restores 35 oz, above the 30 oz return threshold. This forces
readiness → equip → depot → bank without sale proceeds or a zero-capacity dead
end; withdrawn currency weight remains reclaimable.
It requires exactly one normal 56 gp withdrawal (bank
56 → 0), the Lua-verified 56 gp carried balance and equipped upgrade, a hunt
start, and no sale or terminal event. Liquidation remains in `sell_loot` and
`sell_loot_remote_depot`: their assertions check manifest withdrawal and sales;
the local case also checks proceeds-funded potion resupply.

`magic_training_progression` seeds ten selected health potions, two meat,
100 carried gp and 500 bank gp. This leaves the 100 gp reserve plus the
500 gp Great Light price. Only nearby currency reward 50082 is marked claimed.
The scenario ends at `learn_spell` selection over feasible magic training
(utilities 550 and 350); it does not wait for NPC dialogue or spell payment.
`magic_training_reserve` and `magic_training_service` retain their intentional
low-mana and capacity/service setups.

Local regressions: `lua scripts/test-playerbot-fixture-isolation.lua` and
`pwsh -File scripts/test-playerbot-readiness-assertions.ps1`. Live coverage uses
`-Focused -CombatReadiness` and `-Focused -MagicTraining -MagicTrainingCase`
with each of `magic_training_progression`, `magic_training_reserve`, and
`magic_training_service` on `scripts/test-playerbot-gameplay.ps1`.

Use `-MagicTrainingCase <name>` with `-Focused` to run one case from the
16-scenario magic-training matrix without paying for the other server
recreations. PowerShell validates the case name from the supported mode list.

```powershell
docker compose -f server/compose.yaml build server
pwsh -File scripts/test-playerbot-gameplay.ps1 -Healing -Focused -SkipBuild
```

The driver includes Compose status and the last 80 server-log lines in timeout
failures. Docker builds reuse a persistent BuildKit `ccache` mount.

Each selected scenario owns six environment settings: `PLAYERBOT_GAMEPLAY_MODE`
(default `cycle`), `PLAYERBOT_HUNT_DURATION_SECONDS` (`1500`),
`PLAYERBOT_RELOG_DELAY_SECONDS` (`5`), `PLAYERBOT_MAX_CONSECUTIVE_DEATHS` (`3`),
`PLAYERBOT_DEPOT_RESTART_PHASE` (empty), `PLAYERBOT_DEPOT_VERIFIER_PHASE` (empty),
and `PLAYERBOT_DEPOT_MOVE_CASE` (`normal`).
`Invoke-Scenario` applies these defaults before the body; individual cases may
then override them. It restores incoming values after success or failure.
Skipped scenarios do not touch the environment. Suite CLI options (including
`-TimeoutSeconds`) and unrelated environment settings remain unchanged; this
is test-harness ownership, not a change to production configuration.

The death fixture's third kill waits for `service_discovered` after the second
recovered controller reports online. The driver releases a DB-only fixture marker;
Lua polls it for at most 30 seconds. The scenario's overall 45-second deadline and
existing recovery/terminal assertions remain unchanged.

Independent scenarios still receive a fresh database and server process. The
database container remains healthy between them to avoid repeated MariaDB and
volume initialization. Scenarios that verify clean-shutdown persistence stop and
restart only the server while retaining that scenario's database.

The PowerShell entrypoint contains CLI handling and suite lifecycle only. It
dot-sources the `scripts/playerbot-gameplay/runtime.ps1` harness, domain assertion
files, and domain scenario files into the same script scope. The in-server Lua
entrypoint loads ordered `.inc` files from
`server/tests/playerbot-gameplay/includes/`; only `playerbot_gameplay.lua` is
auto-loaded and registers the fixture events.

### Limits

Most gameplay modes use fixed destinations and controlled worlds.
`-HuntRegionPlanning` is a controlled real-map Carlin fixture that validates
loaded spawn-region discovery, cache rebuilds and hits, topology reachability,
cancellation, stale revisions, and non-local candidates. It does not prove a
long-running Rookgaard soak, live combat sampling, oscillation suppression, or
`GOD Admin` notifications. Observe those on the normal stack:

```powershell
$env:PLAYERBOT_HUNT_DURATION_SECONDS = "180"
docker compose -f server/compose.yaml up --build --detach --force-recreate server
docker compose -f server/compose.yaml logs --follow --no-log-prefix server
```

Inspect `hunt_region_candidate`, `hunt_region_scan`, `hunt_region_selection`,
`hunt_region_outcome`, `hunt_challenge_frontier`, `hunt_scope_exhausted`,
`hunt_region_patrol`, `hunt_supply_reserve`, `hunt_area_entered`,
`navigation_progress`, and `action_result` events whose `action` is
`hunt_waypoint`.
Remove the environment override and recreate the server afterward.

Focused tests prove deterministic frontier transitions, not soak stability. The
default 25-minute development hunt is smoke evidence. Use a separate normal
stack run with 20 to 60 minute hunts to assess sustained escalation, recovery
backoff, scan retries, and telemetry volume; short hunts can end before the bot
reaches deep patrol points or accumulates enough active-combat evidence.

Run the unattended benchmark from a fresh level-8 development character and
evaluate it through the first attainment of level 20. Use 45-minute hunt limits:

```powershell
$env:PLAYERBOT_HUNT_DURATION_SECONDS = "2700"
$env:PLAYERBOT_GAMEPLAY_MODE = ""
$env:PLAYERBOT_REGRESSION_MODE = ""
docker compose -f server/compose.yaml down --volumes --remove-orphans
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow --no-log-prefix server
```

Record completed `hunt_region_outcome` events, service and spell transactions,
restarts, terminal events, and the final persisted player state. The controller
continues after level 20; stop it cleanly or run a server save before reading the
database result.

`-SpellCalibration` marks deterministic classifier-helper evidence as
`source="classifier_helper"` and confidence/eviction math as
`source="profile_math"`. It also requires normal engine Light Healing, Haste,
Whirlwind Throw, and level-35 Berserk single- and multi-victim casts, then
restarts the server to confirm a fresh controller starts with no profiles. The
first normal offensive action must remain Berserk below full confidence. Helper
coverage supplies concurrent damage, other attacker, melee ambiguity, target
loss, censored/equality healing boundaries, and bounded profile math. The
synchronous single-thread spell path cannot interleave an external attacker with
`g_game.playerSay`, so those ambiguity cases do not use timing races. The suite
validates classifier behavior and selected mirrored envelopes, not complete
Lua-to-C++ formula fidelity; issue #1 remains authoritative. Loaded metadata and
audited formula envelopes remain the source for safety; observations only rank
already-legal actions. Inspect
`spell_calibration` and `action_result` records for rejection reasons such as
`censored_overheal`, `concurrent_damage`, `other_recovery`, `other_attacker`,
`melee_or_other_bot_damage`, `target_lost`, and `multi_target`. Profiles are
memory-bounded and reset with the controller, so this fixture does not test
database persistence.

## Connectionless regression

The optional regression overlay exercises representative Lua UI/network calls,
temporary inventory and container mutation, death, explicit removal, rejected
login, learned-spell state, and clean-shutdown persistence. Supported modes are
`interactions`, `death`, `remove`, `reject`, `spellTrainer`, `spellReset`,
`spellLearning`, `spellPersistence`, and `spellFailures`. `spellTrainer` buys
Light Healing and Light from Gregor through normal dialogue and verifies keyword
selection and both prices. `spellFailures` covers level, vocation, promotion,
premium, and money rejection without changing learned state or money. Run
`spellReset`, `spellLearning`, and `spellPersistence` in that order against the
same database volume, stopping the server cleanly between each mode; this also
verifies the learned casting gate, duplicate rejection, and persistence.

```powershell
$env:PLAYERBOT_REGRESSION_MODE = "interactions"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --build --detach --force-recreate server
```

Wait for `PLAYERBOT_CONNECTIONLESS_TEST PASS`, stop the server cleanly, change
the mode, and recreate it for the next case. Return to the normal stack with:

```powershell
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
Remove-Item Env:PLAYERBOT_REGRESSION_MODE
docker compose -f server/compose.yaml up --detach --force-recreate server
```

## Client compatibility

For protocol or gameplay-facing client changes, test:

- Login and character selection
- Movement and tile updates
- Containers and inventory
- Books and writable items
- NPC conversation and trade
- Use-with actions
- Combat and death
- Logout and persistence

Watch client and server logs for parser errors, unknown opcodes, restart loops,
and runaway memory use. A successful login alone does not establish
compatibility. Use `Rook Tester` on `admin` / `admin` to observe Bot One.
