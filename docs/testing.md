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

The baseline suite seeds policy-approved rat loot and an unsellable nested bag.
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
empty and guaranteed-loot monsters. It verifies normal corpse opening before
empty classification and preserves item movement for a corpse containing gold:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -CorpseLoot
```

For playerbot navigation or looting changes, run the combined suite:

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot
```

The suite rebuilds the server and resets the disposable Compose stack and
database volume. It removes them afterward; `-KeepStack` leaves the final stack
running. The default timeout is 300 seconds and can be changed with
`-TimeoutSeconds` from 60 to 3600 seconds.

The overlay's `PLAYERBOT_GAMEPLAY_MODE` accepts `cycle`, `navigation`, or
`corpse`. The script selects these modes automatically: the baseline uses
`cycle`, `-FullNavigation` uses `navigation`, and `-CorpseLoot` uses `corpse`.
Navigation and corpse modes skip the economic provisioning assertions and start
the controller directly in hunt mode so their fixtures do not interfere with
startup service.

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
