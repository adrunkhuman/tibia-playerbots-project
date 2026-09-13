# Playerbots

## Mental model

A playerbot is a normal server `Player` whose decisions come from a scheduled controller instead of a connected client. `Bot One` is loaded from the database, placed in the world, and acted through normal movement, combat, speech, item-use, container, trade, and delay rules. Playerbot code must not assume that `Player::client` exists.

The controller observes current state, selects a goal at a safe boundary, executes one prioritized command per scheduler turn, verifies the result, and reevaluates. Survival and pending irreversible actions take precedence over discretionary work. Decisions are deterministic; language models are not in the control path.

This design is intended to scale without one thread, graphical client, renderer, UI, or blocking loop per bot. The current manager controls only the registered `Bot One`; population-scale scheduling has not been implemented.

## Capabilities and limits

“Ordinary” means available to the normal autonomous controller. “Focused evidence” describes controlled test coverage, not a fixture-only capability. Those tests do not establish how reliably an ordinary character will reach or sustain the behavior during long-running progression.

| Area | Status and evidence | Current boundary |
| --- | --- | --- |
| Goal selection | Ordinary | Chooses among survival, service, rewards, spell learning, equipment, magic training, and hunting only at safe boundaries; it is not a general-purpose planner. |
| Hunting | Ordinary; map-derived selection has focused real-map coverage | Scores loaded hostile spawns against equipment, skills, health, supplies, route safety, expected XP, and cash pressure. Adaptive challenge and performance estimates reset with the controller; no long progression soak is established. |
| Combat and healing | Ordinary | Approaches reachable same-floor targets, handles attackers, uses melee, potions, and supported Knight spells. Transit combat is restricted to confirmed route blockers; group tactics and other vocations are not developed. |
| Loot | Ordinary; edge cases have focused evidence | Opens owned corpse containers through normal item use before inspecting contents, ranks known saleable loot by value and weight, and retries inaccessible corpses within bounds. Skinning and other secondary corpse actions are unsupported. |
| Navigation | Ordinary; transition and recovery scenarios have focused evidence | Routes across the loaded map using a shared coarse topology, then plans and executes local paths and supported transitions. Supports tools, doors, floor changes, teleports, and registered NPC travel. Detailed search and recovery have budgets; unsupported transitions and changing obstacles can prevent a journey. Static topology changes may require a supported reload or restart. |
| NPC service | Ordinary | Discovers loaded shops, bankers, spell trainers, and registered travel offers; buys supplies, sells known loot, banks, and verifies transactions. Custom dialogue or opaque travel conditions are excluded unless explicitly modeled. |
| Equipment and depots | Ordinary; restart and remote liquidation paths have focused evidence | Buys or equips supported upgrades, deposits retained loot, and can sell from local or remote depots. Two-handed loadout trade-offs are unsupported and those items remain protected. |
| Spells | Ordinary for audited Knight spells; calibration and overflow training have focused evidence | Can learn and use selected healing, support, and offensive spells through normal speech. Loaded spell rules remain authoritative. Observations may rank legal casts but never weaken safety or legality. |
| Rewards and Oracle | Ordinary goal selection; claiming and departure have focused evidence | Claims supported shared container rewards and one legacy doublet case. The tagged Oracle departure path supports a level 8–10 character with no vocation. Scripted quests, levers, hazardous quest transit, and general dialogue reasoning are unsupported. |
| Death and restart | Ordinary; failure paths have focused evidence | Uses normal death, save, temple login, and bounded relog recovery. It reconstructs intent from persisted state rather than resuming an interrupted route, conversation, open container, or transaction in place. |
| Social behavior | Planned | Multiple bots, parties, guilds, trading between players, relationships, personalities, and generated login rhythms are not implemented. |
| Speech | Planned | No language-model dialogue or personality voice exists. |

Focused scenarios often use fixed destinations, controlled monsters, teleports, or seeded state. Dynamic hunt, route, and target-approach scenarios exercise loaded-world planning, but none proves unattended progression, stable economics, or hundreds-of-bots performance. Plans and temporary gaps belong in the [issue tracker](https://github.com/adrunkhuman/tibia-playerbots-project/issues), not this document.

Navigation is hierarchical: [`PlayerBotTopology`](../server/src/playerbottopology.cpp) indexes the loaded map into connected local regions and transition edges. The [controller](../server/src/playerbotcontroller.cpp) selects a global route, refines its next segment into tile-level actions, and considers eligible NPC travel connections. Whole-map routing describes its scope, not a guarantee that every destination is reachable.

## Knowledge and game boundaries

Bots may use static facts a player could learn and remember: map geography, known spawn areas, quest locations, and loaded NPC services. They must not read hidden live state to bypass game mechanics. In particular, a bot identifies corpses through normal container and ownership metadata, opens them normally, and only then inspects contents.

The world is not observer-gated: bot behavior does not become cheaper or less real when no human is nearby. Normal engine legality, action delays, path execution, payment, inventory capacity, spell costs, cooldowns, danger checks, and item verification remain authoritative. Focused fixtures may arrange state directly, but fixture coordinates and shortcuts must not enter production knowledge.

## Lifecycle and persistence

A controller is `Running`, `Paused`, or `Stopped`. Running controllers schedule bounded turns. Paused is a fixture checkpoint that suppresses turns without ending the controller. Stopped is terminal: pending work is cancelled, one terminal event is emitted, and that controller must never schedule or emit another turn.

Player data, inventory, equipment, learned spells, storages, and depot contents persist through the normal player save path. Routes, targets, conversations, open containers, pending actions, hunt observations, and objective state are memory-only and are reevaluated after relog or restart. Death retains server ownership while bounded recovery reloads the same character. A human cannot log into `Bot One` while that ownership is active.

The `playerbot-setup` Compose service provisions the development account, character, and `player_bots` registration before the server. It is idempotent and fails rather than taking over an unrelated same-named or deleted character. Schema and seed details are authoritative in [`server/schema/insertPlayerbots.sql`](../server/schema/insertPlayerbots.sql).

## Telemetry contract

Playerbot telemetry is JSON Lines on server stdout. Every record has this stable envelope:

- `schema: 1`
- UTC RFC 3339 `ts`
- `component: "playerbot"`
- `event`, `bot`, persistent `player_id`, and `position`

Event-specific fields are conditional. Machine-readable states, actions, results, statuses, and reasons use lowercase values. The main families cover lifecycle and goals, action verification, navigation and NPC travel, hunting and supplies, rewards and equipment, spells, liquidation, and periodic health summaries. Successful movement is not emitted once per tile, and repeated events may be suppressed. A controller's `terminal` event is final.

```powershell
docker compose -f server/compose.yaml logs --no-log-prefix --since 30m server | Where-Object { $_ -match '"component":"playerbot"' }
```

JSONL is authoritative; private messages shown to `GOD Admin` are only an aid. Producers in [`playerbot.cpp`](../server/src/playerbot.cpp) and [`playerbottelemetry.cpp`](../server/src/playerbottelemetry.cpp), plus the assertions under [`scripts/playerbot-gameplay/`](../scripts/playerbot-gameplay/), own event detail. Consumers must tolerate additional event-specific fields and suppression rather than depending on human-readable logs or per-step output.

See [Testing](testing.md) for checks and evidence limits. Repository invariants and maintenance rules live in [AGENTS.md](../AGENTS.md).
