# Testing

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
| `-FullNavigation` | Complete fixed A-B-C-B-A fixture route and temporary blockage recovery. |
| `-TargetPursuit` | Visible-only last-seen pursuit, bounded reacquisition, and out-of-budget abandonment. |
| `-CorpseLoot` | Non-corpse, empty, guaranteed-loot, and container death items; open-before-inspect ordering, suspended-route retry, and bounded inaccessible-corpse timeout. |
| `-DeathTelemetry` | Death context, exponential relog, fresh controller state, abandonment, and removal. |
| `-Healing` | Potion verification, threshold recovery, missing-stock service, purchase, and resume behavior. |
| `-ValueLoot` | Value-per-weight replacement under constrained capacity plus generic food replacement and collection cap without mandatory food purchase. |
| `-PickupProgression` | Nested and multi-root reward inspection, claim verification, upgrades, restart recovery, and space rejection. |
| `-GoalArbitration` | Pickup, service, hunt, and critical-healing precedence across safe boundaries. |
| `-OracleDeparture` | Tagged Oracle route, dialogue, vocation/town/position verification, and restart persistence. |
| `-StaminaProjection` | Premium bonus, low-stamina penalty, and ordinary stamina projections. |
| `-HuntRegionPlanning` | Cached scanner batching, threat rejection, reachability, cooldowns, and observed correction. |
| `-AdaptiveChallenge` | Synthetic frontier and planner-helper fixture covering idle and no-kill exclusion, sparse-combat escalation, hysteresis, recovery backoff, equipment/recovery prediction, lethal rejection, local exhaustion, terminal finality, and no post-terminal controller events. It does not validate real combat sampling or long-running hunt behavior. |
| `-CombatReadiness` | Equipment, the one-potion return threshold and 10-potion restock target, optional-food hunting, generic food consumption and reclaimable capacity, low-wealth banking, carried-upgrade retention through service, and restart reconstruction. It does not cover the terminal case where total funds cannot buy enough potions to exceed the return threshold. |
| `-EquipmentPurchases` | Justified purchase and equip verification, clean restart persistence, carried-upgrade recovery, displaced-item-space rejection, and rejected transactions. |
| `-MainlandRewards` | Real Thais reward object from a teleported, high-capacity fixture; scale-armor claim and equip, displaced-item and bundle preservation, restart reconstruction, and non-null battle-axe rejection evidence. It does not prove normal traversal, realistic capacity limits, or a specific rejection reason. |
| `-Depot` | Real Thais locker/chest discovery from Naji, including exact nearest-locker selection, carried-upgrade equipment, displaced and inferior equipment deposits, nested loot, move verification, retries, and restart checkpoints. |
| `-SlottedLoot` | Invalid-slot loot sale through a live seller, direct depot fallback without an eligible seller, protected-equipment retention, move verification, and interrupted-deposit restart recovery. |
| `-MainlandLoop` | Two real Thais hunt/depot cycles, local services, depot fallback for remote-buyer loot, restart recovery, and teleport exclusion. |
| `-SpellTraining` | Loaded spell-offer trainer discovery, reserve-backed affordability rejection, normal spell dialogue/payment, and restart persistence. |
| `-SpellUse` | Light Healing preemption, Haste, Whirlwind Throw, unlearned-spell potion fallback, and mana-reserve melee fallback. |
| `-SpellCalibration` | Engine-path Light Healing, measured Haste duration, and single-target Whirlwind attribution; deterministic classifier evidence for race-heavy censored/concurrent/ambiguous cases; profile confidence, LRU eviction, bounded values, telemetry, and controller-recreation reset. |
| `-MagicTraining` | Creature aggregated regeneration-forecast boundaries, including finite final ticks, strict overflow/exact-full behavior, PZ pause, one normal Haste/Great Light/Light or Light-refresh cast, service and spell-learning precedence, post-hunt `Idle` arbitration, failed verification, and restart forecast recomputation. Controlled setup proves arithmetic and engine paths, not ordinary long-running frequency; regeneration phase is not preserved across serialization. |

Navigation or looting changes require at least:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot
pwsh -File scripts/test-playerbot-gameplay.ps1 -TargetPursuit -Focused
pwsh -File scripts/test-playerbot-gameplay.ps1 -SpellUse -Focused
pwsh -File scripts/test-playerbot-gameplay.ps1 -MagicTraining -Focused
```

`-TargetPursuit` runs successful `target_pursuit` reacquisition and bounded
`target_pursuit_abandon` fallback scenarios.

Use `-Focused` with one or more scenario switches to skip the baseline. Use
`-SkipBuild` only with a known-current `angelion-server:latest` image; it does
not prove that the image matches the worktree. `-KeepStack` preserves the final
stack for debugging. `-TimeoutSeconds` accepts `30` through `3600` and replaces
each scenario's fail-fast deadline.

Use `-MagicTrainingCase <name>` with `-Focused` to run one case from the
16-scenario magic-training matrix without paying for the other server
recreations. PowerShell validates the case name from the supported mode list.

```powershell
docker compose -f server/compose.yaml build server
pwsh -File scripts/test-playerbot-gameplay.ps1 -Healing -Focused -SkipBuild
```

The driver includes Compose status and the last 80 server-log lines in timeout
failures. Docker builds reuse a persistent BuildKit `ccache` mount.

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

Gameplay modes use fixed destinations and controlled worlds. They do not
validate dynamic Rookgaard region generation, threat cooldowns, observed XP
correction, oscillation suppression, or `GOD Admin` notifications. Observe those
on the normal stack:

```powershell
$env:PLAYERBOT_HUNT_DURATION_SECONDS = "180"
docker compose -f server/compose.yaml up --build --detach --force-recreate server
docker compose -f server/compose.yaml logs --follow --no-log-prefix server
```

Inspect `hunt_region_candidate`, `hunt_region_scan`, `hunt_region_selection`,
`hunt_region_outcome`, `hunt_challenge_frontier`, `hunt_scope_exhausted`,
`hunt_region_patrol`, and `navigation_progress` events.
Remove the environment override and recreate the server afterward.

Focused tests prove deterministic frontier transitions, not soak stability. The
default five-minute development hunt is smoke evidence. Use a separate normal
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
