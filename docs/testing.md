# Testing

## Server Smoke Test

For server, infrastructure, or cross-stack changes, run:

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml config --quiet
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs playerbot-setup server
```

Confirm that MariaDB is healthy, the map loads, the server reports online, and
ports `7171` and `7172` accept local connections. Confirm that
`playerbot-setup` exits successfully, exactly one valid `Bot One` registration
exists, and server JSONL contains a `playerbot` `lifecycle` event with status
`online`.

The GitHub `server-smoke` workflow runs only the normal Compose stack. It checks
fresh provisioning, database health, server startup, lifecycle output, and both
local game ports. It does not execute gameplay actions.

## Playerbot Gameplay Suite

The baseline suite seeds NPC-discovered dead-rabbit loot and an unsellable nested bag.
It verifies tagged live-NPC discovery and runtime offer registration without
physical shop probing, NPC greeting acknowledgement before trade, normal sale
and reserve purchases, carried-money-first purchase deductions, banker
deposit/100 gp withdrawal, fake-depot drop, protected equipment/tools, and a
resumed hunt cycle. It starts with no potion or meat reserve and therefore
exercises purchases to five potions and one meat. It exercises startup service,
not a return triggered by the 30 oz capacity threshold:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1
```

`-FullNavigation` additionally verifies the complete A-B-C-B-A coordinate
sequence and temporary-blockage recovery with ambient monsters suppressed:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation
```

`-CorpseLoot` runs the baseline, resets the stack, then creates deterministic
non-lootable, empty, guaranteed-loot, and container-only death-item monsters.
It verifies that loaded non-corpse items are skipped without an open attempt,
including a container after a valid lootable corpse, normal corpse opening
precedes empty classification, and item movement is preserved for a corpse
containing gold:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -CorpseLoot
```

`-ValueLoot` runs the baseline, resets the stack, and constrains the bot's
capacity by one ounce. It verifies that the bot replaces a 2 gp dead rabbit
with a 5 gp dead wolf discovered from loaded NPC offers, records no capacity
skip, and completes a bank-funded potion purchase:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -ValueLoot
```

`-DeathTelemetry` runs the baseline, resets the stack, and sets a one-second
relog delay with a two-recovery death-loop limit. It verifies contextual death
and terminal events, one- then two-second elapsed backoff, fresh service-first
controllers, no actions during relog delays, resumed service discovery, final
death-loop abandonment, and removal from online state.

`-Healing` runs the baseline, resets the stack, then starts Bot One at 50%
health with three small health potions and no nearby monsters. It verifies
normal potion consumption and health gain, repeated one-at-a-time healing above
the 60% threshold, and resumption of the interrupted hunt objective. A second
focused stack starts at the same health ratio without potions and verifies service
redirection, purchase verification, healing from the new stock, and resumed
service:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -Healing
```

`-PickupProgression` starts from the normal jacket-and-club loadout with the
configured playerbot speed boost intact. It verifies recursive bundle
inspection, known-utility selection, normal claim/storage effects, direct
equipment, and post-claim restart selection. Focused nested fixtures inspect the
purple-bag reward, claim the complete bag, open it normally, equip its wooden
shield ahead of mutable food siblings, and preserve the hand axe and spellbook.
Additional clean fixtures
claim and verify the two-root torch/platinum economic bundle against a
pre-existing platinum stack without equipment actions, reconstruct direct and
nested claimed-but-unequipped rewards without a
second claim, and reject a new claim when the root backpack cannot hold the reward:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -PickupProgression
```

This suite covers only shared container rewards with action ID `2000`. It does
not cover the separately known action ID `2001` doublet reward or rewards from
dedicated quest scripts. Other non-container reward sources remain outside the
current playerbot behavior.

`-GoalArbitration` verifies four consecutive decision boundaries. A useful
pickup beats optional service and hunting at startup; pickup cooldown then lets
service win; completed service with settled reserves selects hunting; and the
ten-second hunt deadline reevaluates to hunting rather than forcing unnecessary
service. A second fixture damages the bot during pickup and verifies that
critical healing interrupts it before claim and forces service. Candidate
utilities, results, decision IDs, and the absence of terminal failure are asserted:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -GoalArbitration
```

`-OracleDeparture` advances the clean fixture to level 8, selects the live
runtime-tagged Oracle, navigates from the Rookgaard temple, and completes the
normal `hi`, `yes`, `thais`, `knight`, `yes` dialogue. It verifies vocation ID
`4`, town ID `2`, and the Thais temple position, then stops and restarts the
server to verify clean-shutdown persistence and startup
`objective="departure_complete"` reconstruction:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -OracleDeparture
```

The fixture emits `ORACLE_DEPARTURE_START`, `ORACLE_DEPARTURE_PASS`, and
`ORACLE_DEPARTURE_RESTART_PASS`. It does not validate the noncanonical Oracle
starter-item grants tracked in issue #65.

For playerbot navigation or looting changes, run the combined suite:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot -Healing -ValueLoot -PickupProgression -GoalArbitration -OracleDeparture
```

The suite rebuilds the server and resets the disposable Compose stack and
database volume. It removes them afterward; `-KeepStack` leaves the final stack
running. It prints wall-clock timing for the build and every scenario.

Focused debugging can skip both the baseline and a known-current image build:

```powershell
docker compose -f server/compose.yaml build server
pwsh -File scripts/test-playerbot-gameplay.ps1 -Healing -Focused -SkipBuild
```

`-Focused` requires at least one scenario switch and runs only the requested
scenarios. `-SkipBuild` requires an existing `angelion-server:latest` image and
does not verify that it matches the worktree; omit it before final validation.
Docker builds use a persistent BuildKit `ccache` mount. The compile layer copies
only CMake and C++ inputs, so datapack and fixture-only edits reuse the server
binary while source edits reuse unchanged compiler results.

Default fail-fast deadlines cover each complete scenario, including stack setup
and all waits: 180 seconds for the baseline, full navigation, pickup progression,
and Oracle departure; 240 seconds for goal arbitration; 60 for corpse and
ordinary healing; 45 for death; and 90 for healing-resupply and value-loot. An
explicitly supplied `-TimeoutSeconds` from 30 through 3600 overrides every
scenario deadline for slower machines. A wait failure includes Compose status
and the last 80 server-log lines.

The overlay's `PLAYERBOT_GAMEPLAY_MODE` accepts `cycle`, `navigation`, `corpse`,
`death`, `healing`, `healing_resupply`, `value`, `progression`,
`progression_bundle`, `progression_nested`, `progression_resume`, `progression_nested_resume`,
`progression_space`, `arbitration`, `arbitration_interrupt`, or `departure`. The
script selects these
modes automatically from the corresponding switches. Navigation, corpse,
healing, healing-resupply, and value modes start the controller directly in
hunt mode so their fixtures do not interfere with the baseline startup service
assertions. Progression, arbitration, and departure modes retain the normal
temple start and objective selection. Death mode kills the bot outside the
temple protection zone, verifies two temple relogs with one- then two-second
backoff, observes the second fresh controller resume service, and forces one
final death to verify the configured death-loop limit.

Gameplay modes deliberately retain fixed hunt destinations and override hunt
duration to 10 or 900 seconds where required. They do not validate dynamic
Rookgaard region generation, threat/cooldown decisions, observed XP correction,
oscillation suppression, or `GOD Admin` notifications. Validate the prototype
manually on the normal stack and inspect its JSONL evidence:

```powershell
$env:PLAYERBOT_HUNT_DURATION_SECONDS = "180"
docker compose -f server/compose.yaml up --build --detach --force-recreate server
docker compose -f server/compose.yaml logs --follow --no-log-prefix server
```

Look for `hunt_region_candidate`, `hunt_region_scan`,
`hunt_region_selection`, `hunt_region_outcome`, `hunt_region_patrol`, and
`navigation_progress`. The three-minute interval above is an observation
override; tracked defaults are a `300` speed bonus and 300-second hunt interval.

## Connectionless Regression Overlay

The optional overlay mounts `server/tests/`, including
`playerbot_connectionless.lua`, and covers representative Lua UI/network sends,
temporary inventory/container mutation, death, explicit removal, rejected
login, and clean-shutdown persistence.

Start with interaction mode:

```powershell
$env:PLAYERBOT_REGRESSION_MODE = "interactions"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --build --detach --force-recreate server
```

Wait for its `PASS` marker, then stop the server cleanly before each follow-up:

```powershell
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "death"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server

docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "remove"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server

docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
$env:PLAYERBOT_REGRESSION_MODE = "reject"
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml up --detach --force-recreate server
```

The `death` mode verifies saved food condition and inventory before killing the
bot. The `remove` and `reject` modes cover explicit removal and rejected login.
Wait for each mode's `PASS` marker before stopping or recreating the server for
the next mode.

Return to the normal stack with:

```powershell
docker compose -f server/compose.yaml -f server/compose.playerbot-regression.yaml stop server
Remove-Item Env:PLAYERBOT_REGRESSION_MODE
docker compose -f server/compose.yaml up --detach --force-recreate server
```

## Client Compatibility

For protocol or gameplay-facing client changes, test:

- Login and character selection
- Movement and tile updates
- Containers and inventory
- Books and writable items
- NPC conversation and trade
- Use-with actions
- Combat and death
- Logout and persistence

Do not claim compatibility from successful login alone. Watch client and server
logs for parser errors, unknown opcodes, restart loops, and runaway memory use.
Log in as `Rook Tester` with `admin` / `admin` to observe Bot One and smoke-test
normal client interaction.
