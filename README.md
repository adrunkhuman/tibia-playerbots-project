# tibia-playerbots

A Tibia 8.60 real-map server built for a population made mostly of bots.

Bots run inside the server as database-backed `Player` objects. They need no
OTClient, renderer, external bot API, network connection, or dedicated thread.
They walk, fight, loot, eat, heal, trade, bank, claim rewards, die, and relog
through the same server operations that handle human actions.

The target is hundreds of bots on the real map, with personalities and
relationships that persist. The nearest relative is the WoW Playerbots project.

## What a bot does

On a clean development stack, Bot One starts as a level 8 Knight in Carlin. It
picks hunting grounds by evaluating live spawns against its own gear, fights what
it can handle, loots by value, keeps itself fed and healed, sells to NPCs and
banks the money, claims supported quest rewards and equips upgrades it finds,
and recovers from death. Focused fixtures also exercise the earlier lifecycle
from level 1 in Rookgaard through Oracle departure to the mainland.

Normal hunting and Oracle navigation derive destination goals from loaded map
and spawn data rather than following an ordered route. Shops, spell trainers,
travel providers, and bankers are discovered from loaded NPC state; the bespoke
Oracle role remains explicit. Shared map topology bounds global discovery while
detailed route planning retains per-decision limits. The navigator remains a
prototype rather than a complete hierarchical world router.

## Design constraints

From the original architecture notes, still holding:

- Bots are `Player` instances controlled by a server extension. Runtime bot
  behavior uses normal movement, combat, item-use, and action-delay APIs rather
  than directly mutating player or world state.
- Movement, navigation, and combat stay deterministic. Language models are for
  speech, later, and never in the decision path.
- Bots may use static world facts that a player could learn and retain, such as
  geography, known hunting grounds, and known quest locations. They must not use
  hidden live state to bypass game mechanics.
- Focused tests may use scenario coordinates and direct fixture setup. Those
  must not leak into the production world-knowledge model.
- Persistence uses the normal player save path. There is no parallel bot store.

Two more came out of the work:

**No observer-gated simulation.** Bots do not get cheaper when nobody is
watching. Many humans will be connected in different places, and a world that is
only real where someone is standing is a stage set.

**Map knowledge is not live omniscience.** A bot may know roughly where spawns
and quest chests are because players can learn those facts. It does not get
hidden live state: it identifies a corpse through normal corpse and ownership
metadata and has to open it before it knows what is inside.

## Decisions

A bot observes, proposes goal candidates, scores them, commits to one, works it,
verifies the result, and reevaluates. Survival and pending irreversible actions
outrank anything discretionary.

The hunt planner derives regions from loaded spawns, estimates threat
against the bot's health, armor, defense, weapon, and skill, clusters suitable
spawns, and checks reachability. Selected regions provide patrol destinations;
navigation callers provide destination goals rather than transition
checkpoints. Focused regression fixtures retain fixed destinations for
deterministic assertions.

Significant decisions, transitions, candidates, outcomes, and failures emit
JSON Lines on stdout with schema version `1`. The stable envelope and event
families are documented in [`docs/playerbots.md`](docs/playerbots.md). Repeated
events may be suppressed, and successful movement is not logged once per tile.
Tests assert against this stream rather than scraping human-readable output.

## 8.60 fidelity

The datapack is a downgrade and it shows. NPC dialogue, monster stats, spells,
loot tables, and quest rewards are reconciled against real 8.60 behavior as
problems turn up.

Bots are useful here as a side effect. Repeated merchant, dialogue, combat, and
item interactions expose broken content faster than occasional manual play.
Several content fixes started as bot failures.

## Layout

| Path | Contents |
| ---- | -------- |
| `server/` | Angelion TFS 1.5 downgrade, branch `8.60`, imported as a squashed subtree. Bot code is under `src/playerbot*`. |
| `client/` | OTClient Redemption, imported as a squashed subtree and configured for this project. |
| `scripts/` | Client bootstrap and gameplay test drivers. |
| `docs/` | Playerbot behavior, testing, and client runtime documentation. |
| `.subtree/` | Pinned upstream baselines. |

`AGENTS.md` contains the working rules and their rationale. Read it before
changing either subtree.

## Running it

The supported local environment uses Docker Desktop and PowerShell 7+. GitHub
CLI authentication is needed only when the bootstrap script must download the
pinned private client executable.

```powershell
pwsh -File scripts/bootstrap-client.ps1
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow server
```

Launch `client/launch-angelion-redemption.cmd`. Ports `7171` and `7172` bind to
localhost only.

The tracked accounts are local development defaults:

| Account | Password | Characters |
| ------- | -------- | ---------- |
| `admin` | `admin` | `GOD Admin`, `Rook Tester` |
| `bot-one` | `bot-one` | `Bot One` |

A human client cannot take control of `Bot One` while the server owns it.
Database and world state are disposable; reset them with:

```powershell
docker compose -f server/compose.yaml down --volumes
```

## Testing

CI runs a smoke test on server changes: fresh stack, database health, bot
provisioning, startup, lifecycle telemetry, and local game ports.

The gameplay suite is local and PowerShell-based. It boots scenario worlds with
fixture monsters and asserts on the event stream, covering corpse handling,
value looting, healing, death recovery, goal arbitration and interruption,
reward claiming, Oracle departure, equipment purchasing, spell training, and
spell use. Focused scenarios also cover target approach, adaptive hunt planning,
depot risk recovery, and local or remote loot liquidation.

```powershell
pwsh -File scripts/test-playerbot-gameplay.ps1 -FullNavigation -CorpseLoot
pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -EquipmentPurchases
pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -MainlandRewards
pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -SpellTraining
pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -SpellUse
pwsh -File scripts/test-playerbot-gameplay.ps1 -Focused -SellLoot
```

Most gameplay fixtures use controlled destinations. Hunt-region planning,
hunt-area arrival, service-route, and target-approach modes exercise dynamic
planning on the loaded map, but they do not replace a long-running progression
soak. Client compatibility is checked manually against the list in `AGENTS.md`;
a successful login alone proves nothing.

## Deferred

Roughly in order. Each stage waits for the preceding behavior to be real rather
than assumed.

| Stage | Work |
| ----- | ---- |
| Mechanical quests | Hazardous transit: plan entry, objective, and exit; budget damage; abort on risk. |
| Multiple bots | Fleet scheduling, shared world knowledge, and performance work. |
| Population | Generated characters, level spread, and login rhythm. |
| Economy | Give a large population economic drains instead of only loot faucets. |
| Society | Parties, guilds, trade, and a relationship graph fed by server events. |
| Speech | Language models voicing personalities already represented in persistent data. |

Tibia's skull and frag system is a mechanical reputation substrate for the
social layer rather than something to reinvent. Progress lives in the issue
tracker, not here.

## Upstreams

| Component | Upstream | Branch |
| --------- | -------- | ------ |
| Server | [Giorox/Angelion-TFS-1.5-Downgrade-8.6](https://github.com/Giorox/Angelion-TFS-1.5-Downgrade-8.6) | `8.60` |
| Client | [opentibiabr/otclient](https://github.com/opentibiabr/otclient) | `main` |

Both are squashed subtrees, not submodules. Pinned baselines live in
`.subtree/`.

```powershell
git subtree pull --prefix=server angelion-upstream 8.60 --squash
git subtree pull --prefix=client redemption-upstream main --squash
```

Review and test upstream updates before merging them into `master`.
