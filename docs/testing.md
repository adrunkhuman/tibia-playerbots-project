# Testing

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
pwsh -File scripts/test-playerbot-supply-assertions.ps1
```

On Linux, pure contract checks require a C++17 compiler; fixture-isolation checks
require Lua or LuaJIT. Run these from the repository root:

```sh
sh server/tests/playerbot_contracts.sh
lua scripts/test-playerbot-fixture-isolation.lua
lua scripts/test-playerbot-hunt-fixture.lua
lua scripts/test-playerbot-depot-fixture.lua
lua scripts/test-playerbot-death-fixture.lua
lua scripts/test-playerbot-transit-fixture.lua
lua scripts/test-playerbot-supply-fixture.lua
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

## Hunt supply budgets and early Exura

Hunt selection keeps lethal, adaptive-challenge, topology, and route gates first.
Among safe candidates, it prefers a duration budget that fits carried potions
minus emergency and route reserves, then projected XP (or coin income under the
cash-pressure rule below). If none fits, it chooses
the lowest expected potion consumption among safe candidates, with XP breaking
ties. This is not a new hunt eligibility gate: existing readiness and emergency
return rules still apply. A budget is not permission to spend the reserve.

The uncalibrated static estimate uses spawn probabilities and intervals, existing
combat/clear throughput, and conservative local-crowd inflation. Each modeled
crowd's fight damage is divided by that same crowd's summed isolated damage;
the largest local ratio is applied to spawn-rate damage, with a floor of one.
Two identical attackers therefore inflate their already-summed damage by 1.5,
not three. It estimates whole potions over the hunt time remaining after travel,
using minimum healing. It credits only legal Exura's audited minimum and the active default regeneration
condition. Regeneration uses whole ticks during estimated combat time, expires
after its current lifetime, and gets no travel credit. Exura retains its mana
reserve and receives at most one mana pool of credit, not repeated refill cycles.
No current-health spending, future food, looted supplies, other spells, equipment
regeneration, expected profit, or observed supply calibration is assumed.

Before scoring, transport feasibility processes eight loaded offers per turn
against an immutable player/resource snapshot. Shared compact component
reachability snapshots replace per-arrival full-graph distance arrays. Each planning
session captures an immutable offer catalog from current provider positions and
offers; provider movement does not repeatedly cancel active planning. Repeated bounded
passes retain the cheapest affordable label and establish true multihop
connectivity. Scoring then remains bounded at 256 candidates per turn. The cheap
full-atlas pass keeps only nonlethal, sustainable candidates with static
connectivity or a plausible eligible registered transport chain. Survivors retain normal policy order; no
local, remote, tested, untested, or income quota changes that order. Route
validation checks at most one outbound, depot-exit, or supply path per turn and
continues beyond failed candidates. Once a safe candidate exists, it skips each
candidate whose no-travel XP bound and income tier cannot beat that choice, but
continues to any later competitive candidate. Actual travel time and outbound/return
potion reserves update the budget before final
selection. Potion changes, mana loss, cancellation, or snapshot invalidation
stop the incremental session safely.

`hunt_region_candidate` reports `supply_estimate_source=static_duration_budget`,
`supply_budget_fits`, `supply_expected_damage`, `supply_regeneration_healing`,
`supply_spell_healing`, `supply_expected_potions`, `supply_reserved_potions`, and
`supply_routine_potions`. `score` remains projected XP; it is not a composite
supply score. `route_validated=true` candidate events carry the revised budget.
Selections report `selection_rule=supply_budget_then_xp` or
`lowest_potion_consumption_then_xp`.

Affordable, legally offered Light Healing precedes optional goals and cash
rebalancing. Mandatory departure, capacity, and healing-supply service remain
higher priority. Exura retains 100 gold and more than the emergency/route potion
threshold, rather than funding a ten-potion restock. Other spells keep the full
restock reserve. The selected reserve is checked again before NPC payment; no
spell is granted and datapack eligibility is unchanged. Goal candidates explain
this as `priority_recovery_spell` or `deferred_recovery_spell`; the selected
training plan also reports `potion_reserve`.

Non-Docker coverage: `sh server/tests/playerbot_contracts.sh` checks duration,
scarce/plentiful and all-over-budget selection, XP tie-breaking, safety precedence,
route reserve growth and travel-time reconciliation, finite regeneration, Exura
legality/mana bounds, exact affordability, and goal priority.
The supply assertion regression above rejects incorrect selection, payment,
priority, and missing live verification evidence. The supply Lua regression
checks the live verifier's exact potion/payment counts and bounded wait.

Parent/live validation (resets the disposable test stack):

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario adaptive_challenge,spell_training_low_supplies,spell_training_low_supplies_unaffordable,spell_training,spell_training_shortlist,hunt_region_planning
```

`adaptive_challenge` additionally runs deterministic synthetic candidate facts
through the real bounded hunt scoring session/runtime: two potions select the
one-potion, lower-XP candidate; ten select the eight-potion, higher-XP candidate.
The all-over-budget case keeps two potions but extends the horizon to 25 minutes:
the easy candidate needs two potions, the costly one twelve, and neither fits.
The runtime must still select the easy candidate, not stop or revert to max XP.
Its `supply_budget_fixture` events include the horizon, both candidates' estimates
and budget-fit flags, and the selection rule. Their source is
`synthetic_runtime_candidates`, not measured live combat consumption. The two new spell fixtures use
the real trainer and payment path: two potions plus 270 gold learn Exura and
retain both potions/100 gold; 269 gold rejects it. Existing spell persistence,
shortlist, adaptive-challenge, and hunt-planning assertions remain in place.
Map extraction and real combat consumption still need live validation; the
synthetic contrast alone does not establish sustainable XP.

## Adaptive challenge and remote hunt routes

Challenge evidence now requires at least 60 active combat seconds and three
kills. Zero to two potions, or routine Exura casts while the sampled mana floor
stays healthy, can support a bounded `+0.10` strong-evidence step. A single
recovery never causes backoff by itself. Three potions cause backoff only at one
or more uses per active combat minute; slower use holds the frontier. Danger,
death, critical health, critical mana during healing, and burst potion pressure
back off by `0.10`. The frontier is capped at `0.60`; predicted-lethal combat,
route danger, and reserves remain separate hard gates. `hunt_challenge_frontier`
reports p10 health and mana percentages plus potions per active combat minute.

The atlas no longer discards a hunt only because static walking/tool topology
cannot reach it. Before expensive routing, a disconnected candidate must match a
plausible chain of registered travel arrivals whose loaded offers satisfy current
level, premium, opaque-condition, and aggregate-fare requirements. All cheap
survivors compete together using calibrated XP and estimated travel; there is no
remote probe, unobserved slot, execution randomization, or local/remote quota.
Each scheduler turn validates at most one outbound, depot-exit, or supply route.
Validation compares static navigation with registered NPC travel, including
actual offer conditions, fare, travel time, and danger. After route reserves are reconciled, modeled potion use or an available
potion count at the route threshold also requires a safe route from the depot to
a loaded potion seller. The combined outbound, exit, and
supply fares must leave the recovery spending reserve intact. A changed route is
accepted only if its complete current fare still leaves the later-phase fares
and current potion-target cost protected; each paid NPC step repeats that check.
Free routes are never rejected for a cash shortfall, and the protected potion
cost decreases after restocking. Exit-route validation reserves only later supply
fare, not the exit fare a second time. Protected post-hunt depot exits cannot use
the legacy unsafe-route fallback.

A selected hunt must have a safe route to a usable depot near the hunt area; it
need not return to the selection origin. Depot preflight keeps one approach for
every distinct loaded depot ID, and supply preflight keeps one approach for every
loaded matching provider. Neither list has an arbitrary eight/four truncation.
Both validate one route per turn. Normal post-hunt depot
discovery also permits NPC travel and replans from the bot's current position.
For example, a Carlin departure may select a Darashia hunt and then use a
Darashia depot. No city or home mapping is encoded.

Candidate telemetry exposes `topology_reachable`, `transport_plausible`,
`calibration_source`, `calibration_sample_count`, `outbound_npc_travel`,
`outbound_fare`, `exit_npc_travel`, `exit_fare`, `supply_npc_travel`,
`supply_fare`, the chosen `exit_depot_destination`, and bounded rejection reasons
such as `transport_requirements_unavailable`, `safe_depot_exit_unavailable`, and
`travel_fare_breaks_recovery_reserve`. Selection records aggregate
`route_rejection_counts`. Hunt outcomes report `performance_observed` and
`performance_evidence_reason`.

`sh server/tests/playerbot_contracts.sh` covers routine Exura and potion use,
challenge and performance evidence qualification, shared correction averaging,
variant-local correction priority, exclusion of default/invalid entries,
non-weighting of repeated outings, more than eight failed route candidates before
a winner, planning cancellation between bounded scoring turns, absence of route
quotas, travel-offer requirements, combined fare reserves, and switching from a
coarse provider route to the live local interaction range. `server/tests/playerbotdepotworkflow_test.cpp`
checks that post-hunt depot ranking uses the current hunt area instead of the
departure city. The focused `remote_hunt` gameplay fixture raises the seeded
Knight to level 15 in Carlin, restricts only the fixture's route candidate queue to atlas candidates outside
static topology, and requires a registered NPC travel success, remote hunt entry,
a safe local depot preflight, and the same depot during the real post-hunt workflow:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario remote_hunt
```

This proves the integrated runtime path against the loaded map and NPC offers. It
does not calibrate static potion forecasts from observed use or guarantee that a
remote hunt beats a local hunt in ordinary selection.

## Sustained hunt eligibility and gross coin income

Atlas construction retains every pocket, including one-to-three-spawn pockets,
so they can contribute to larger cross-floor variants. Scoring rejects a
standalone candidate with fewer than four reachable spawn blocks. Larger
candidates must have at least three replenishing blocks and at least half their
reachable blocks replenishing. These are eligibility gates, not personality
preferences or permission to weaken danger, topology, route, or reserve checks.
The quadratic screen handles at most 256 reachable blocks per candidate;
larger variants receive `replenishment_model_limit`, while their smaller pocket
and neighborhood variants remain available.

`Spawn::findPlayer` uses same-floor spectators within 11 tiles on each axis;
failed attempts restart the spawn timer. A spawn attempt requires absence at
that instant, not absence throughout its interval. The bounded **patrol duty-share
heuristic** uses the blocking rectangle plus one tile of margin. A block qualifies
with at least 25% modeled unblocked time over a travel-and-fight lap, and at least
5% over the same lap with every fight removed. The second check rejects token
empty-patrol opportunities and circuits supported only by predicted combat.

Unblocked time includes fights at outside approaches and Chebyshev travel only
between same-floor approaches whose entire bounding rectangle is outside the
blocker. The cycle denominator includes every fight and the existing atlas
Manhattan travel plus 20-step-per-floor portal proxy. No cross-floor segment gets
unblocked travel credit. Separate absences add to the duty share; the longest
continuous absence remains diagnostic. Totals count exactly one lap, even though
two laps find the continuous maximum across the patrol origin. Duplicate approaches
combine fight time; rotating the patrol does not change the result. A block whose
every monster ignores spawn blocking has share one in both models, but does not
bypass minimum hunt size.

Rationale: four spawns alone can still occupy one blocking rectangle. Requiring
at least three and half the blocks to replenish excludes a token distant spawn.
The 25% duty-share and 5% empty-lap floors reject brief or token exposure without
requiring the bot to stay away for an entire respawn interval. That earlier gate
was rejected after live `hunt_planning` validation scanned 1,520 candidates with
zero suitable hunts. The safe six-spawn Snake/Spider site centered at
`(32403,31727,9)` had only a 0.09 minimum continuous-away/interval ratio. A focused
map-derived geometry check now qualifies its six members at roughly 32% minimum
full-lap and 7% minimum empty-lap share, without fixture gear changes. Those are
static model results, not a claim that the updated live fixture passes.

Inspection of the raw
`World-spawn.xml` with the atlas's same-floor pocket clustering found 5,905
pockets, including 1,900 with at most three entries; 3,490 larger pockets had
at least half their positions outside another position's 12-tile rectangle.
These counts precede hostile-monster and topology filtering, and are not counts
of eligible loaded hunts. The reported Troll pocket contains two entries at
`(32459,32275,8)` and `(32460,32277,8)`, both with 240-second intervals.

This is a geometry/opportunity heuristic with **uncalibrated expected timing**,
not proof of successful respawns or scheduler phase alignment. Periodic patrols
can still miss every attempt despite a positive duty share. Spawn-rate limits, other
players, chase movement, and empty subsequent circuits can defeat that timing.
If navigation would remove a modeled atlas waypoint, the runtime exhausts the
whole region, applies its ten-minute cooldown, and uses the existing service/
replanning path. It does not retain eligibility for the smaller patrol. Fixed
fallbacks and synthetic hunts without modeled geometry retain waypoint skipping
and empty-patrol exhaustion. No waiting, teleportation, or forced spawning was added.
Normal-stack validation must check candidate availability and repeated combat;
full route geometry and sustainable progression remain runtime checks.

Coin estimates use loaded loot definitions and `ItemType::worth`, including
conditional nested-container loot. The exact inclusive `0..100000` roll is
divided by `RATE_LOOT`; the same roll controls strict chance comparison and
stack-count modulo. Fractional counts follow the Lua-to-integer conversion.
Expectations are cached per monster per atlas build. A build-local memo also
shares exact `(chance, countmax, stackable, rate)` arithmetic across monsters
and nested loot, enumerating each distinct loaded tuple only once. It is
bounded by loaded metadata and discarded after the build; a rebuilt atlas gets
a fresh memo, and loot-rate changes still invalidate the atlas cache. Stamina at or below 840 credits no coins, matching the
loot callback. Per-spawn replenishment results restrict both recurring coin and
XP yield to replenishing members: the blocked half of a viable three-of-six
circuit contributes neither. Each qualifying member's coin and XP spawn rates
are multiplied by the smaller of its full-lap and empty-lap unblocked shares.
Thus slow modeled fights cannot inflate its income above the empty-lap rate cap.
The existing clear-cycle cap still applies. All members still contribute to
clear time, danger, and supply costs, so this correction cannot relax the existing
safety budget. Spawn probabilities, discounted spawn rates, and patrol/combat
clear throughput cap gross gold/minute. XP stages, premium XP, resale, costs, observed
XP corrections, and unopened corpse contents do not enter this estimate.
It assumes loot-container space and successful collection; competition, full
containers, missed corpses, and spawn suppression can lower realized income.

Cash pressure means carried potions are at most the current return reserve plus
one, and carried plus bank money cannot cover the existing loaded-offer restock
spending reserve (including the carried-gold reserve). Initial scoring uses the
current dynamic hunt restock target, not the spell policy's fixed ten-potion
target. After outbound and return route validation, each candidate refreshes
cash pressure and its supply budget from their combined potion reserve; its
recovery target is at least ten and at least that reserve plus one, saturating
at `UINT32_MAX`. Spell-training reserve behavior is unchanged. Exact
affordability or plentiful supplies retains XP preference. Under pressure, expected gross coin
income breaks ties after supply fit and, when neither budget fits, minimum
potion consumption; XP breaks remaining ties. This preserves the existing
all-over-budget fallback and emergency/route reserves. Missing recovery offers
retain the existing unaffordable-reserve behavior. Funds changes invalidate an
in-progress planning snapshot. There is no personality framework.

Before routing, supply-fit tiers still precede over-budget tiers, and lower
potion demand still precedes higher demand when neither fits. Under cash pressure,
coin income remains the tie-breaker inside that normal policy; it does not receive
reserved route slots. Both outbound and return routes retain the existing safety,
reserve, and affordability checks. The planner incrementally replaces rejected
candidates from the complete cheap-viable queue. It stops after exhausting that
finite queue or when no remaining optimistic bound can beat the validated choice.
`hunt_region_scan` reports `route_candidate_policy=all_cheap_viable_ranked` and
`route_candidate_count`.

`hunt_region_candidate` adds `sustained_eligible`, `reachable_spawns`,
`replenishing_spawns`, `minimum_away_interval_ratio`,
`minimum_unblocked_patrol_ratio`, `minimum_empty_patrol_unblocked_ratio`,
`replenishment_estimate_source=patrol_duty_share_heuristic`, `cash_pressure`,
`coin_estimate_source=static_loaded_loot_gross`, and
`expected_coin_gold_per_minute`. Ratio minima cover all reachable members,
including nonqualifying members; continuous-away ratio is diagnostic only.
Rejections distinguish `tiny_spawn_pocket` and
`insufficient_replenishment` (or `replenishment_model_limit`). Selection rules under pressure are
`supply_budget_then_coin_income_then_xp` and
`lowest_potion_consumption_then_coin_income_then_xp`; `score` remains XP.
Verified loot moves report `coin_gold_acquired` separately from existing
`total_value` (which includes sale-valued items). Hunt outcomes aggregate only
verified coin moves during that active hunt and report actual gold/minute
against the prediction; bank withdrawals and sales do not count as acquired
hunt coins.

Focused non-Docker checks:

```sh
sh server/tests/playerbot_contracts.sh
lua scripts/test-playerbot-coin-estimation.lua
pwsh -File scripts/test-playerbot-supply-assertions.ps1
pwsh -File scripts/test-playerbot-navigation-assertions.ps1
```

The C++ checks cover tiny/dense exclusion, retaining tiny-pocket membership in a
larger circuit, brief/token opportunity rejection, blocking exceptions,
chance/count/rate/denomination arithmetic, cash-pressure boundaries, income/XP
ranking, and safety/supply precedence. It also compiles the pure hunt runtime,
planning session, and policy: regressions cover mixed three-of-six yield,
one-lap accounting, short but meaningful absence, empty-patrol viability and
rate caps independent of slow combat, the map-derived six-spawn site,
modeled waypoint failure/cooldown versus synthetic/fallback skipping, and
raised-route-reserve income ranking through the real planning session (including
exact affordability and lethal rejection). A twelve-candidate regression proves
that an initially twelfth-ranked income candidate reaches routing without
changing raw planner selection, and checks affordable/plentiful cases and
safety/supply precedence. Memo regressions make 5,000 requests for five exact
arithmetic tuples and verify five evaluations, including separate rate/count/
chance/stackability keys and fresh build ownership. The Lua check runs the actual datapack
loot generator against deterministic rolls to verify the C++ expectation cases.
Neither test proves live respawn timing or actual collected income.

## Transit combat and danger retreat

Travel to a hunt area, progression destinations, service/depot returns (including
ordinary returns), and corpse approaches prefer movement and detours. A crowd
alone never permits stop-and-fight. Deliberate combat inside the reached hunt
area remains separate; fixed fallback patrol fixtures remain deliberate hunts
from startup. Navigation must confirm failed-detour, route-critical
blocker evidence before transit combat can start; target telemetry retains
`route_critical:true` and `defensive_path_blocker`.

Each blocker gets one no-chase attempt per transit episode, with a five-second
wall-clock budget checked before healing on each running scheduler turn.
Healing keeps its normal priority but cannot renew combat. Exhaustion emits
`transit_combat_budget`; movement of the blocker emits `transit_blocker_moved`.
Neither permits reacquisition in the same episode. Navigation resets, repeated
turns, and intermediate arrivals (including coarse/local NPC approach legs) do
not renew attempts. Hunt-area entry, a new goal decision, or a cycle-phase
transition ends the episode. Existing combat is
released with `transit_goal_changed` before it can preempt a new transit goal.
Loot retries and deadlines retain their existing bounds.

`hunt_region_observed_danger` still cancels hunt/defensive targets and defers
level-eight departure until depot arrival or a new hunt. It now uses the same
combat policy as ordinary transit, rather than a separate escape exception.
These bounds do not guarantee escape: an enclosed bot can still fail to find a
route or die. Route-risk calibration and supply/healing decisions are unchanged.

`sh server/tests/playerbot_contracts.sh` checks transit classification, optional
crowd rejection, wall-clock budgets across healing turns, no reacquisition,
episode retention across coarse/local approach arrivals, and semantic completion/
new-goal resets. The deterministic live
`transit_return` fixture starts at `(32105,32191,8)` in the hall north of the
synthetic depot stall at `(32105,32195,8)`. Four stationary adjacent attackers
occupy the north/west tiles, leaving the straight southward route empty. This
geometry was checked against `World.otbm` and `items.otb`; the local Lua test uses
those ground-only tiles rather than an all-walkable mock. The fixture requires
normal return movement and depot arrival within 30 seconds with all attackers
untouched and no target selection:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario transit_return
```

The fixture fails rather than skips if the map lacks the required open geometry.
Its log assertion has positive and adversarial non-Docker checks in
`scripts/test-playerbot-navigation-assertions.ps1`. Live execution remains needed
to validate the fixture geometry and server integration.
`corpse_inaccessible` requires a failed detour before exactly one confirmed
route-critical, no-chase blocker engagement. It must clear the target with
`skipped/transit_combat_budget` after five seconds (at most one additional
one-second scheduler turn), not wait for the old `failed/combat_timeout` result.
The assertion also requires suspension, resumed corpse navigation with retained
failure counts, and further route failure before one `corpse_inaccessible`
result. The original limits remain: at most six navigation failures and 70 seconds,
without reacquisition or a controller terminal event. Replay a captured JSONL log
without Docker with:

```powershell
pwsh -File scripts/test-playerbot-navigation-assertions.ps1 -InaccessibleCorpseLogPath /tmp/corpse-inaccessible.jsonl
```

The live navigation/loot/defense/healing regression command is:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot -TargetApproach -Healing -Focused
```

These existing fixtures do not deterministically trigger observed hunt danger
followed by an adjacent attacker. A normal-stack observation must still verify
that the danger transition is followed by movement toward the depot rather than
`defensive_attacker` selection, with no reacquisition of a spent blocker. There is
no dedicated live danger fixture yet. Check a captured normal-stack log with:

```powershell
pwsh -File scripts/test-playerbot-navigation-assertions.ps1 -DangerRetreatLogPath /tmp/danger-retreat-live.log
```

The assertion requires a complete danger return with movement and no optional
combat, repeated blocker selection, or terminal event before depot arrival.
Its synthetic positive/negative regressions run without the log-path argument;
they validate telemetry assertions, not live monster behavior.

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
| `-CombatReadiness` | Equipment, the one-potion return threshold, safety-floor restock while Light Healing is level-eligible and unlearned, 20-potion ammo restock otherwise, optional-food hunting, generic food consumption and reclaimable capacity, low-wealth banking, carried-upgrade retention through service, and restart reconstruction. It does not cover the terminal case where total funds cannot buy enough potions to exceed the return threshold. |
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
The `mainland_loop` fixture keeps its configured 10-second execution deadline for
fast cycle, restart, and persistence coverage, but scores hunt candidates against
a 900-second planning horizon. This prevents the fixture-only deadline from making
every real travel route consume the whole forecast; normal playerbots use the same
configured duration for planning and execution.
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

### Hunt planning and arrival fixture baseline

`hunt_planning` and `hunt_area_arrival` test the atlas and mainland navigation,
not combat with Rookgaard starter equipment. Both bypass the Rookgaard baseline
reset and explicitly equip the exact mainland Knight loadout from
`server/schema/insertPlayerbots.sql`: head 2480, armor 2464, right-hand shield
2530, left-hand sword 2395, legs 2468, and boots 2643. Sword and shielding are
exactly 20, other skills 10, with zero skill tries. Login verifies each equipped
slot, the seeded backpack/tools, absent necklace/ring accessories, magic level 0,
maximum mana 35, and 470 oz capacity. Normal first-login gifts may occupy the ammo slot. The existing level-8/185-health baseline and ten
health potions remain; no funds are added or recovery thresholds changed.
Both scenarios require `HUNT_MAINLAND_LOADOUT_PASS` before their start marker.

This corrects a fixture mismatch: the former shared reset replaced the seed's
mainland equipment with starter armor 2650 and weapon 2382 before promoting the
character back to Knight. After sustained-hunt filtering, one observed arrival
run had only one suitable candidate; an ordinary patrol route failure correctly
exhausted it with no alternative. The fix changes fixture fidelity, not viability,
waypoint failure, or danger policy. Live navigation remains subject to route and
monster variability; seeded gear is not a guarantee of arrival.

`hunt_planning` still removes nearby live monsters repeatedly to isolate planning.
It does not edit loaded spawn metadata, create an authored hunt candidate list,
or choose a destination. Its C++ fixture observations retain first-route
unreachable/second-route node-limit failures, one scoring-barrier cancellation,
and cache-revision invalidation. `hunt_area_arrival` retains normal live monsters
and no such planning injections, and must enter the selected region before
matching combat and before the first waypoint completion.

`lua scripts/test-playerbot-hunt-fixture.lua` runs both real login paths with
mocked player/world APIs. It compares the installed equipment and skills against
the seed SQL, checks unchanged funds and potion counts, distinguishes monster
suppression by mode, and rejects failed equipment/skill writes or verification.
These pure checks do not establish live arrival or combat success.

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
