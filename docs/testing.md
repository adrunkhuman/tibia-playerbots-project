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

The PowerShell driver builds the server, creates disposable scenario stacks,
asserts against JSONL telemetry, reports per-scenario timing, and removes the
stack afterward:

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
| `-CorpseLoot` | Non-corpse, empty, guaranteed-loot, and container death items; open-before-inspect ordering. |
| `-DeathTelemetry` | Death context, exponential relog, fresh controller state, abandonment, and removal. |
| `-Healing` | Potion verification, threshold recovery, missing-stock service, purchase, and resume behavior. |
| `-ValueLoot` | Value-per-weight replacement under constrained capacity and bank-funded purchase. |
| `-PickupProgression` | Nested and multi-root reward inspection, claim verification, upgrades, restart recovery, and space rejection. |
| `-GoalArbitration` | Pickup, service, hunt, and critical-healing precedence across safe boundaries. |
| `-OracleDeparture` | Tagged Oracle route, dialogue, vocation/town/position verification, and restart persistence. |
| `-StaminaProjection` | Premium bonus, low-stamina penalty, and ordinary stamina projections. |
| `-HuntRegionPlanning` | Cached scanner batching, threat rejection, reachability, cooldowns, and observed correction. |
| `-CombatReadiness` | Equipment, supplies, capacity, service recovery, upgrades, and restart reconstruction. |
| `-Depot` | Real locker/chest discovery, nested deposits, move verification, retries, and restart checkpoints. |
| `-MainlandLoop` | Two real Thais hunt/depot cycles, local services, restart recovery, and teleport exclusion. |

Navigation or looting changes require at least:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot
pwsh -File scripts/test-playerbot-gameplay.ps1 -TargetPursuit -Focused
```

`-TargetPursuit` runs successful `target_pursuit` reacquisition and bounded
`target_pursuit_abandon` fallback scenarios.

Use `-Focused` with one or more scenario switches to skip the baseline. Use
`-SkipBuild` only with a known-current `angelion-server:latest` image; it does
not prove that the image matches the worktree. `-KeepStack` preserves the final
stack for debugging. `-TimeoutSeconds` accepts `30` through `3600` and replaces
each scenario's fail-fast deadline.

```powershell
docker compose -f server/compose.yaml build server
pwsh -File scripts/test-playerbot-gameplay.ps1 -Healing -Focused -SkipBuild
```

The driver includes Compose status and the last 80 server-log lines in timeout
failures. Docker builds reuse a persistent BuildKit `ccache` mount.

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
`hunt_region_outcome`, `hunt_region_patrol`, and `navigation_progress` events.
Remove the environment override and recreate the server afterward.

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
