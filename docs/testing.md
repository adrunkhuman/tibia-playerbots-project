# Testing

## Safety first

Run commands from the repository root with PowerShell 7+ (`pwsh`). Windows uses Docker Desktop; Linux uses Docker Engine with Compose v2. Linux client bootstrap also requires a locally built `client/otclient`.

The gameplay suite owns the disposable Compose project `angelion`: it resets the database and removes the stack unless `-KeepStack` is used. Do not run it beside a development session whose state you need. If Docker requires elevation on Linux, run Docker commands and the gameplay driver from an explicitly authorized elevated shell; scripts do not elevate themselves or change socket permissions. Elevated failures can leave root-owned artifacts.

The [log follower](../scripts/playerbot-gameplay/runtime.ps1) bounds each poll and retained log data. A capture-limit failure preserves partial diagnostics rather than collecting indefinitely; verbose hunt records remain in raw evidence and are parsed on demand. These are log limits, not a process-RAM guarantee. On Linux, a systemd scope with `MemoryMax=3G` and `MemorySwapMax=0` can additionally protect the host during log-heavy tests; it limits the test process and its CLI children, not Docker-managed server containers.

For persistence checks, restart or recreate only the server with `--no-deps`. Rerunning `playerbot-setup` can refill equipment slots and invalidate saved-state evidence. A failed Docker-access preflight does not clean the stack. Commands using `$env:` or `Remove-Item` must run inside PowerShell.

## Choose checks by change

| Change | Run | Passing establishes | Does not establish |
| --- | --- | --- | --- |
| Documentation only | Check local links and command targets; run `git diff --check` | References and examples match the worktree | Runtime behavior |
| Pure playerbot policy or telemetry parsing | `sh server/tests/playerbot_contracts.sh`, `sh server/tests/playerbot_loot_contracts.sh` for cargo changes, and affected `scripts/test-playerbot-*-assertions.ps1` | Deterministic contracts and captured-log assertions | Loaded-world integration |
| Fixtures or scenario isolation | `lua scripts/test-playerbot-fixture-isolation.lua` plus the affected Lua fixture check | Fixture setup is isolated and expected state is seeded | Ordinary autonomous behavior |
| Server, Compose, or cross-stack behavior | Server smoke test below | Fresh provisioning, startup, lifecycle output, and ports | Gameplay or client compatibility |
| Navigation or corpse looting | `pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -FullNavigation -CorpseLoot` | Integrated movement, recovery, corpse opening, and bounded loot failure paths | Whole-map routing or a progression soak |
| Target approach | `pwsh -File scripts/test-playerbot-gameplay.ps1 -TargetApproach -Focused` | Reachable, unreachable, and attacker-priority fixtures | General creature memory |
| A playerbot subsystem | `pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused <switch>` | Selected controlled scenarios and JSONL assertions | Frequency or reliability in ordinary long-running play |
| One regression | `pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario <name>` | That catalog scenario only | Neighboring modes |
| Protocol or gameplay-facing client | Smoke test plus manual client checklist below | Tested server/client interaction surface | Compatibility from login alone |

Useful subsystem switches include `-Healing`, `-ValueLoot` (value replacement plus currency, partial-weight, nested-slot, and protected full-slot cargo), `-DeathTelemetry`, `-GoalArbitration`, `-CombatReadiness`, `-HuntRegionPlanning`, `-AdaptiveChallenge`, `-EquipmentPurchases` (including rope/shovel replenishment and nested-inventory duplicate protection), `-MainlandRewards`, `-OracleDeparture`, `-Depot`, `-SellLoot`, `-SpellTraining`, `-SpellUse`, `-SpellCalibration`, `-MagicTraining`, and `-MainlandLoop`. The accepted switches and scenario catalog are authoritative in [`scripts/test-playerbot-gameplay.ps1`](../scripts/test-playerbot-gameplay.ps1).

`-SkipBuild` requires a known-current `angelion-server:latest` image and does not prove it matches the worktree. `-KeepStack` preserves the final stack for debugging. `-TimeoutSeconds` accepts 30–3600 seconds. Most focused scenarios use controlled state or destinations; map-derived planning modes improve integration evidence but still do not prove long-running progression.

## Fast checks without a live stack

The spell contract check needs neither Docker nor elevation:

```powershell
pwsh -File scripts/test-knight-spell-contract.ps1
```

Run the assertion scripts relevant to the change:

```powershell
pwsh -File scripts/test-playerbot-navigation-assertions.ps1
pwsh -File scripts/test-playerbot-cycle-assertions.ps1
pwsh -File scripts/test-playerbot-readiness-assertions.ps1
pwsh -File scripts/test-playerbot-log-parsing.ps1
pwsh -File scripts/test-playerbot-runtime.ps1
pwsh -File scripts/test-playerbot-scenario-isolation.ps1
pwsh -File scripts/test-playerbot-depot-scenario.ps1
pwsh -File scripts/test-playerbot-death-scenario.ps1
pwsh -File scripts/test-playerbot-magic-training-assertions.ps1
pwsh -File scripts/test-playerbot-supply-assertions.ps1
```

On Linux, the C++ contract checks require a C++17 compiler. Lua fixture checks require Lua or LuaJIT:

```sh
sh server/tests/playerbot_contracts.sh
sh server/tests/playerbotlifecycle_contracts.sh
sh server/tests/playerbothunttiming_contracts.sh
sh server/tests/playerbotrouting_contracts.sh
sh server/tests/playerbotselllootapproach_contracts.sh
sh server/tests/playerbotselllootbasket_contracts.sh
sh server/tests/playerbotselllootfilter_contracts.sh
sh server/tests/playerbotshortcutheuristic_contracts.sh
sh server/tests/playerbotroutecorridor_contracts.sh
sh server/tests/playerbotguidedtree_contracts.sh
sh server/tests/playerbotroutecache_contracts.sh
sh server/tests/playerbotdangersamplekey_contracts.sh
sh server/tests/playerbotplanningbudget_contracts.sh
sh server/tests/playerbot_loot_contracts.sh
sh server/tests/playerbot_equipment_purchase_contracts.sh
lua server/tests/playerbot_door_passages_contracts.lua
lua scripts/test-playerbot-fixture-isolation.lua
lua scripts/test-playerbot-hunt-fixture.lua
lua scripts/test-playerbot-depot-fixture.lua
lua scripts/test-playerbot-death-fixture.lua
lua scripts/test-playerbot-transit-fixture.lua
lua scripts/test-playerbot-supply-fixture.lua
lua scripts/test-playerbot-coin-estimation.lua
```

A zero exit status and each script's explicit pass marker are the pass signal. These checks prove policy, arithmetic, parsing, and fixture contracts; they do not execute an ordinary live world.

### Depot liquidation timing

Follow one bot's depot visit across `sell_loot_plan` records: route work can span several turns. `snapshot_us` measures synchronous candidate construction; `walking_us` and `npc_us` are nested within `elapsed_us`, so do not add them. A budget denial reports `budget_wait_us` instead. `sell_loot_approach` and `sell_loot_candidate` show approach retries and rejections; `action_result` with `action: "sell_loot_provider_route"` shows the execution route, quote, and rejection reason. `sell_loot_defer` marks an abandoned trip. The planning budget does not impose a hard latency bound on synchronous work.

### Hunt-selection timing

Capture a normal selection and group `hunt_planning_slice` events by `server_run_id`, `controller_id`, and `planning_pass`. Each admitted `selectHuntRegion` call emits one slice, including yields, failures, stale revisions and final selection. Budget-denied calls instead emit `hunt_planning_budget` with the requested retry delay, credit/debt, cumulative consumed time and queue depth. `invalidated_planning_pass`/`invalidated_scoring_revision` identify a replaced pass.

| Fields | Meaning |
| --- | --- |
| `elapsed_us`, `over_10ms` | Synchronous elapsed time inside `selectHuntRegion`; the 10 ms marker is observational, not a work cap. |
| `setup_us` | Player observation, scan, topology and runtime work construction. |
| `transport_us` | Transport work and completion. |
| `score_us`, `score_completion_us` | Scoring batch; then applying scores, sorting, shortlist and snapshot copies. |
| `evidence_us` | Fixture callbacks, scan/candidate serialization and output, and fixture shortlist work. Planning-session telemetry borrows the session instead of copying its candidate vectors. |
| `route_checks_us`, `route_bookkeeping_us` | Route checks, discovery and observations; then selector setup/final selection, including final selection output. |
| `outbound_route_us`, `depot_route_us`, `supplier_route_us` | Whole route-planner calls, including both walking and NPC alternatives. |
| `walking_us`, `npc_us` | Time spent attempting each alternative, including unsuccessful alternatives. |
| `depot_discovery_us`, `supplier_discovery_us` | Candidate lookup for the corresponding service. |
| `*_checks`, `route_attempts` | Handled route-request turns, including continuations; newly initialized route searches. `depot_unavailable_checks` counts depot requests with no route available to attempt. |
| `route_request_sequence`, `route_yields` | Pending selector request and search continuations. A request keeps its sequence across turns. |
| `route_local_searches`, `route_local_expanded_nodes`, `route_node_limits` | Detailed walking searches, expanded nodes this slice, and exhausted total allowances. A yield is not a node-limit result. |
| `route_topology_queries`, `route_topology_expanded_nodes`, `route_topology_cache_hits` | Coarse topology work and shared source-connectivity reuse. |
| `route_local_connections`, `route_connection_cache_hits` | Detailed transport connection evaluation and reuse within one route request. The legacy `route_coarse_connections` field is zero: discarded coarse estimates are no longer computed. |
| `route_coarse_rejects` | Connections rejected without local expansion because all endpoints exist in the build-time topology, no supported path connects them, and no subsequent change has added an unmodelled supported connection. Their `hunt_route_connection` result is `unreachable` with `fallback_reason: coarse_unreachable`. |
| `route_bound_rejects`, `route_risk_rejects`, `route_unknown_connections` | Safe cost/fare-bound pruning, detailed prefixes whose accumulated/peak danger disqualifies every extension, and connections whose reachability remains unknown. `route_local_bound_stops` is retained but unused by hierarchical connections. |
| `route_ordinary_searches`, `route_hierarchy_fallbacks` | Destination-guided ordinary tile searches and failed preferred itineraries entering refinement. |
| `route_alternate_itineraries`, `route_corridor_searches`, `route_corridor_widenings` | Alternative coarse itineraries found; connected-piece corridor attempts and attempts beyond the initial width. Restricted failures do not establish unreachability. |
| `route_guided_searches`, `route_heuristic_builds`, `route_heuristic_restarts` | Whole-graph target-directed searches/retargets; shortcut-potential acquisitions and restarts caused by changed shortcut geometry. |
| `route_shared_cache_hits`, `route_shared_cache_misses`, `route_source_tree_hits`, `route_incomplete_cache_hits` | Shared successful-segment reuse, segment misses, reusable fallback source trees, and unresolved targets reused without further expansions. A new target does not refill its source tree's allowance; a larger allowance can extend it. Incomplete evidence remains unknown, not proof of unreachability. |
| `route_transport_spendable_gold` | Funds remaining above the minimum final recovery reserve for a handled route turn. This is a snapshot, not an additive counter. Free transport remains eligible below the reserve. |
| `route_invalidations`, `route_transport_restarts`, `route_transport_restart_limits`, `route_request_restart_limits` | Invalidated request facts, dependent transport rebuilds, and exhausted provider/request-change retries. |
| `route_invalidation_reason`, `route_invalidation_cause`, `route_invalidation_tile_key`, `route_invalidation_journal_delta`, `route_invalidation_watched_tiles` | Which request dependency changed and the bounded world-change journal evidence. Cosmetic/count-only item updates with unchanged navigation semantics are ignored. |
| `walking_expanded_nodes`, `npc_returned_expanded_nodes` | Reported plan metrics. NPC counts exclude failed/null attempts and unreported segment searches. |
| `schedule_delay_ms`, `schedule_late_us` | Requested delay before this callback; elapsed time from the controller's due time to callback execution. Both are null outside scheduled callbacks. |

Sum slice `elapsed_us` and compare it with `hunt_region_scan.decision_latency_us`. The difference is **not** all scheduler lag: it includes requested waits and other work between calls. Scheduler lateness includes posting/queueing and earlier dispatcher tasks, but not pre-selection work in the current callback. Fallback retries request `SCHEDULER_MINTICKS` (50 ms). Unfinished route computation uses a shorter continuation delay because it already yields bounded work; this is not a movement or item-use cooldown. The scheduler does not clamp the delay and starts its timer after processing the posted event.

Route-alternative, route-stage and discovery timings are **nested within** `route_checks_us`; do not add them to the phase totals. Sum continuation slices for complete route costs. `route_attempts` is no longer the number of calls to the resumable adapter: most calls resume an existing request. Transport-label connection caches remain request-local; ordinary segment and danger-sample caches are shared and bounded. Timings measure elapsed time, not CPU time. `elapsed_us` excludes its own slice serialization/output and any subsequent terminal stop, and does not measure the full `navigate` task. Microsecond rounding can slightly reduce summed phase totals. Existing `hunt_region_scan.scoring_time_us` retains its narrower meaning. Focused fixtures do not establish normal live-world timing.

Candidate `topology_reachable` and `topology_travel_steps` describe coarse walking reachability, not NPC transport. A selected hunt may have `false`/zero there if its outbound NPC route is validated; assertions must not require a walking-only winner.

`hunt_route_connection` records one completed walking/NPC connection: requested targets, source, last reached position (`to`), result, expanded nodes, successful-segment cache hits, incomplete-result reuse, fallback reason and accumulated active time. Use it to distinguish expensive successes from exhausted alternatives. `alternate_itineraries`, `corridor_searches`, `corridor_radius`, `unrestricted_fallback` and `heuristic_restarts` identify the refinement stages used by that connection. It excludes scheduling waits and is nested in slice timings.

The planning-budget contract runs ten synthetic concurrent requesters under a virtual clock and checks fairness and aggregate debt accounting. It is not a live ten-bot benchmark. For live comparisons, keep the same seeded character and compare candidate identities/eligibility, route outcomes, expanded nodes, active work, end-to-end latency and scheduler lateness. Do not interpret a faster run caused by fewer resolved candidates as an unconditional improvement.

## Server smoke test

For server, infrastructure, or cross-stack changes:

```powershell
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --tail 200 playerbot-setup server
```

Confirm MariaDB is healthy, the map loads, the server reports online, and `127.0.0.1:7171` and `127.0.0.1:7172` accept connections. `playerbot-setup` must exit successfully, one valid registration must exist for each of `Bot One` and `Bot Two`, and server JSONL must contain an `online` playerbot lifecycle event for each distinct GUID/controller in the same server run. Activation is staggered, so the server-online banner can precede Bot Two's online event.

The [`Server CI`](../.github/workflows/server-ci.yml) workflow's `server-smoke` job performs this fresh-stack check for `server/**` and workflow changes. It does not execute gameplay actions.

## Two-bot lifecycle and provisioning

```powershell
pwsh -File scripts/test-playerbot-provisioning.ps1
pwsh -File scripts/test-playerbot-multibot.ps1
```

The provisioning check uses an isolated, disposable database-only Compose project. It checks the seeds, idempotence, and rejection of SQL errors or conflicting identities.

The multibot check covers independent lifecycle, interactions, login protection, and persistence. It requires Python 3 and OpenSSL, owns the disposable `angelion` stack, and refuses existing containers. Persistence restarts use `--no-deps` to avoid provisioning changes.

Use `-Case shared_npc` or another listed case for a targeted check, `-SkipBuild` with a current image, and `-KeepStack` to retain the final case. Logs go to `artifacts/playerbot-multibot/` or `-ArtifactsPath`.

For the non-destructive network check against an already-running normal stack:

```sh
python scripts/test-playerbot-login.py
```

This checks human login/logout and bot takeover rejection with the local defaults, not graphical-client gameplay. `--self-test` checks the packet codec and local public-key extraction without connecting.

For observations, group telemetry by server run and controller. Summary counters are cumulative: use differences within each controller segment, not a sum of every summary.

## Gameplay suite

Existing gameplay and connectionless overlays require a single-bot database and seed only Bot One. The gameplay driver restores that baseline before each scenario, recreates the server, and asserts against streamed JSONL telemetry:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1
```

With no selection arguments, this runs the full suite: the baseline hunt/service cycle and all focused subsystem scenarios. Supplying subsystem switches without `-Focused` still runs the full suite. Use `-Focused` with the smallest switch set matching the change, or `-Scenario <name>` for exact selection; do not combine those selection modes. A passing scenario means its controlled setup reached the required telemetry and state assertions before timeout. It does not mean a normal character will reach that state unaided, repeat it indefinitely, or progress reliably across the real map.

The normal console output shows build/scenario phases, timings, and results. Use `-Verbose` when Compose lifecycle detail is needed. On a scenario failure, the reported artifact directory contains complete Compose command output, retained and streamed server logs, unfiltered playerbot JSONL evidence, container restart/exit/OOM state, Compose status, and failure metadata. Do not pipe the stored JSONL through summary filters: hunt candidate detail and startup events are part of the evidence.

For a normal-stack observation, shorten hunts only when useful, recreate the server, and inspect `goal_*`, `action_result`, `hunt_*`, `navigation_progress`, `summary`, and `terminal` JSONL events. Focused tests prove deterministic transitions; use 20–60 minute hunts to evaluate repeated combat, service, recovery, planner retries, and telemetry volume. For bounded agent diagnostics, start with `docker compose -f server/compose.yaml logs --tail 200 server`; increase the tail only for a specific missing interval, and save the complete JSONL source when it is test evidence. Do not report a fixture or short smoke run as proof of sustained progression.

## Client compatibility

Client runtime or cross-stack compatibility work requires `pwsh -File scripts/bootstrap-client.ps1`; server-only checks do not require client assets or a local Linux client build.

For protocol or gameplay-facing client changes, manually test:

- login and character selection;
- movement and tile updates;
- containers, inventory, books, and writable items;
- NPC conversation and trade;
- use-with actions;
- combat and death;
- logout and persistence.

Watch both logs for parser errors, unknown opcodes, restart loops, and runaway memory use. A successful login alone proves nothing beyond login. Use a human-controlled character on `admin` / `admin` for manual checks; do not attempt to take control of either registered bot.
