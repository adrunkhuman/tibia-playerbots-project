# Testing

## Safety first

Run commands from the repository root with PowerShell 7+ (`pwsh`). Windows uses Docker Desktop; Linux uses Docker Engine with Compose v2. Linux client bootstrap also requires a locally built `client/otclient`.

The gameplay suite owns the disposable Compose project `angelion`: it resets the database and removes the stack unless `-KeepStack` is used. Do not run it beside a development session whose state you need. If Docker requires elevation on Linux, run Docker commands and the gameplay driver from an explicitly authorized elevated shell; scripts do not elevate themselves or change socket permissions. Elevated failures can leave root-owned artifacts.

For persistence checks, restart or recreate only the server with `--no-deps`. Rerunning `playerbot-setup` can refill equipment slots and invalidate saved-state evidence. A failed Docker-access preflight does not clean the stack. Commands using `$env:` or `Remove-Item` must run inside PowerShell.

## Choose checks by change

| Change | Run | Passing establishes | Does not establish |
| --- | --- | --- | --- |
| Documentation only | Check local links and command targets; run `git diff --check` | References and examples match the worktree | Runtime behavior |
| Pure playerbot policy or telemetry parsing | `sh server/tests/playerbot_contracts.sh` and affected `scripts/test-playerbot-*-assertions.ps1` | Deterministic contracts and captured-log assertions | Loaded-world integration |
| Fixtures or scenario isolation | `lua scripts/test-playerbot-fixture-isolation.lua` plus the affected Lua fixture check | Fixture setup is isolated and expected state is seeded | Ordinary autonomous behavior |
| Server, Compose, or cross-stack behavior | Server smoke test below | Fresh provisioning, startup, lifecycle output, and ports | Gameplay or client compatibility |
| Navigation or corpse looting | `pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot` | Integrated movement, recovery, corpse opening, and bounded loot failure paths | Whole-map routing or a progression soak |
| Target approach | `pwsh -File scripts/test-playerbot-gameplay.ps1 -TargetApproach -Focused` | Reachable, unreachable, and attacker-priority fixtures | General creature memory |
| A playerbot subsystem | `pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused <switch>` | Selected controlled scenarios and JSONL assertions | Frequency or reliability in ordinary long-running play |
| One regression | `pwsh -File scripts/test-playerbot-gameplay.ps1 -Scenario <name>` | That catalog scenario only | Neighboring modes |
| Protocol or gameplay-facing client | Smoke test plus manual client checklist below | Tested server/client interaction surface | Compatibility from login alone |

Useful subsystem switches include `-Healing`, `-ValueLoot`, `-DeathTelemetry`, `-GoalArbitration`, `-CombatReadiness`, `-HuntRegionPlanning`, `-AdaptiveChallenge`, `-EquipmentPurchases`, `-MainlandRewards`, `-OracleDeparture`, `-Depot`, `-SellLoot`, `-SpellTraining`, `-SpellUse`, `-SpellCalibration`, `-MagicTraining`, and `-MainlandLoop`. The accepted switches and scenario catalog are authoritative in [`scripts/test-playerbot-gameplay.ps1`](../scripts/test-playerbot-gameplay.ps1).

`-SkipBuild` requires a known-current `angelion-server:latest` image and does not prove it matches the worktree. `-KeepStack` preserves the final stack for debugging. `-TimeoutSeconds` accepts 30–3600 seconds. Most focused scenarios use controlled state or destinations; map-derived planning modes improve integration evidence but still do not prove long-running progression.

## Fast checks without a live stack

The spell contract check needs neither Docker nor elevation:

```powershell
pwsh -File scripts/test-knight-spell-contract.ps1
```

Run the assertion scripts relevant to the change:

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

On Linux, the C++ contract checks require a C++17 compiler. Lua fixture checks require Lua or LuaJIT:

```sh
sh server/tests/playerbot_contracts.sh
lua scripts/test-playerbot-fixture-isolation.lua
lua scripts/test-playerbot-hunt-fixture.lua
lua scripts/test-playerbot-depot-fixture.lua
lua scripts/test-playerbot-death-fixture.lua
lua scripts/test-playerbot-transit-fixture.lua
lua scripts/test-playerbot-supply-fixture.lua
lua scripts/test-playerbot-coin-estimation.lua
```

A zero exit status and each script's explicit pass marker are the pass signal. These checks prove policy, arithmetic, parsing, and fixture contracts; they do not execute an ordinary live world.

## Server smoke test

For server, infrastructure, or cross-stack changes:

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs playerbot-setup server
```

Confirm MariaDB is healthy, the map loads, the server reports online, and `127.0.0.1:7171` and `127.0.0.1:7172` accept connections. `playerbot-setup` must exit successfully, exactly one valid `Bot One` registration must exist, and server JSONL must contain a playerbot `lifecycle` event with `status="online"`.

The [`Server CI`](../.github/workflows/server-ci.yml) workflow's `server-smoke` job performs this fresh-stack check for `server/**` and workflow changes. It does not execute gameplay actions.

## Gameplay suite

The driver builds or reuses the server image, starts disposable MariaDB, restores the ordered schema and development characters before each scenario, recreates the server, and asserts against streamed JSONL telemetry:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1
```

The baseline covers NPC discovery, selling and supply purchase, banking, fixture-depot handling, and return to hunting. Add the smallest switch set matching the change. A passing scenario means its controlled setup reached the required telemetry and state assertions before timeout. It does not mean a normal character will reach that state unaided, repeat it indefinitely, or progress reliably across the real map.

For a normal-stack observation, shorten hunts only when useful, recreate the server, and inspect `goal_*`, `action_result`, `hunt_*`, `navigation_progress`, `summary`, and `terminal` JSONL events. Focused tests prove deterministic transitions; use 20–60 minute hunts to evaluate repeated combat, service, recovery, planner retries, and telemetry volume. Do not report a fixture or short smoke run as proof of sustained progression.

## Client compatibility

For protocol or gameplay-facing client changes, manually test:

- login and character selection;
- movement and tile updates;
- containers, inventory, books, and writable items;
- NPC conversation and trade;
- use-with actions;
- combat and death;
- logout and persistence.

Watch both logs for parser errors, unknown opcodes, restart loops, and runaway memory use. A successful login alone proves nothing beyond login. Use a human-controlled character on `admin` / `admin` for manual checks; do not attempt to take control of `Bot One`.
