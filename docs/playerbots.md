# Playerbots

## Runtime model

`Bot One` is a database-backed `Player` controlled inside the server. It has no
client connection or external bot API, and a human cannot take control while
the server owns it. Bot work runs through the dispatcher and scheduler; the
design must not add a thread, client, renderer, UI, or blocking loop per bot.

Runtime behavior uses normal movement, combat, speech, item-use, and action-delay
APIs. Bot-facing `Player` methods must remain safe when `client` is null. Normal
shutdown and death recovery use the existing player save path rather than a
parallel persistence mechanism.

## Provisioning

The `playerbot-setup` Compose service runs before the server. It owns the
development account `bot-one`, character `Bot One`, and `player_bots` registry.
Provisioning is idempotent: it does not restore spent money, replace occupied
equipment slots, or take over an unrelated same-named or deleted character.

A new Bot One starts as a level 8 Knight at the Thais temple with development
hunt skills, plate equipment, sword, shield, backpack, rope, shovel, five small
health potions, one meat, 100 gp bank balance, and two 100-gp backpack stacks.
Global `freePremium` supplies premium access.

Inspect setup and startup with:

```powershell
docker compose -f server/compose.yaml logs playerbot-setup server
```

## Decisions

The controller evaluates top-level goals only at safe boundaries: startup,
completed or failed pickup, completed service/depot work, and hunt deadline or
capacity return. It does not switch goals during movement, combat, looting,
dialogue, transactions, or pending item verification. Safety can force service;
critical healing can interrupt reward or Oracle travel before an irreversible
action.

Utilities are deterministic arbitration scores, not probabilities:

| Candidate | Baseline |
| --------- | -------: |
| Critical healing service | 1000 |
| Oracle departure | 950 |
| Capacity service | 900 |
| Equipment reward | 650 |
| Spell training | 550 |
| Ordinary service | 400 |
| Hunt | 300 |
| Economic reward | 250 |

Service needs and reward value adjust these baselines, so candidates can cross
nominal tiers. Equal scores keep declaration order: departure, service, pickup,
spell training, then hunt. Successful pickup and spell-training families cool
down for five minutes; failed or interrupted families cool down for 60 seconds.

## Navigation and hunting

Callers provide destinations, not ordered transition checkpoints. The bounded
navigator searches loaded map state within a 192-tile margin around its
endpoints and stops after 100,000 expanded nodes. It supports ordinary floor
changes, configured ladders, rope and shovel holes, direct-use holes, and
unlocked doors. Arbitrary map teleports are not navigation edges. Cardinal
movement costs 10; diagonal movement costs 30. Failed steps are excluded for 10
seconds. Repeated A-B oscillation
suppresses the implicated transition for two minutes.

Hunt regions come from all loaded hostile spawns on floors 6 through 15. The
shared cache groups overlapping eight-tile spawn kernels through spatial
buckets. Each controller considers regions within 200 weighted tiles of its
town temple and 300 weighted tiles of its current position, then scores them in
bounded batches using health, equipment, weapon, defense, skill, cooldowns, and
observed performance. It validates suitable candidates incrementally through
the navigator and selects the highest route-adjusted score. Navigation remains
behind the destination/reachability interface so a later navigator can replace
tile planning without changing hunt selection.

Predicted threat rejects a region above `0.35` expected fight damage per maximum
health. It models up to three overlapping hostile spawns. Taking one maximum
health pool of damage within the first two minutes abandons the region for ten
minutes. Completed hunts update a per-controller XP correction after at least
30 seconds and one kill; the sample is clamped to `0.25` through `2.0` and uses
a 65/35 rolling blend. This state resets on relog or restart.

When an attacked monster leaves normal positional or creature visibility, or
the attack association is lost, the bot clears chase and pursues a reachable
tile adjacent to its last observed position. It never updates this goal from a
hidden creature's live state.
Visible targets may update the approach goal and can be reacquired within six
tiles. Pursuit lasts at most five seconds and six tiles of Chebyshev displacement.
Reaching the approach point or exhausting either budget returns to patrol and
suppresses that target for ten seconds. A 60-second traversal combat timeout
retains its separate 120-second suppression.

The map-derived region planner and bounded pursuit are prototypes, not
whole-map hierarchical navigation or general creature memory. Regression
fixtures use fixed destinations unless their focused mode explicitly exercises
dynamic planning or pursuit.

## Survival, service, and loot

At or below 60% health, the bot uses one small health potion through normal
use-with-creature handling and verifies consumption and net healing. Missing
stock redirects it to service. Food use preserves one meat and respects the
fullness limit.

Service NPCs require an exact `playerbot_service` XML tag of `shop`, `banker`,
`oracle`, or `spell_trainer`. Shops publish their loaded offers to the bot;
trainers publish each spell registered through `addSpellKeyword`. Untagged
providers and tagged providers without offers are ignored. Providers must remain
within 200 weighted tiles of the registered town temple. The bot greets the
selected NPC, treats a private reply as focus acknowledgement, and opens the
normal trade window. Reply text is not interpreted.

Spell training currently considers tagged providers within the Thais temple
scope; Gregor is the initial tag. It derives trainer offers from loaded NPC
scripts and rejects offers with a registry mismatch, wrong vocation, level,
premium status, learned state, missing supply reserve, insufficient funds after
the 100 gp carried reserve plus five potion and one meat replacement costs, or
an unavailable route. A selected spell uses normal `hi`, keyword, and `yes`
dialogue. Completion requires both learned state and the exact total-money
delta. Learned-spell persistence reconstructs completion after restart and
prevents a repurchase.

The casting policy has four audited descriptors: `healing`, `support`,
`melee_offense`, and `ranged_offense`. It currently uses Light Healing for
recovery, Haste on a safe route with at least 20 remaining steps, Berserk at
level 35, and Whirlwind Throw before then against a visible adjacent combat
target. Casts use the normal player speech spell path. The live spell
engine remains authoritative for eligibility, costs, targeting, line of sight,
weapon requirements, aggression, cooldowns, and action constraints. Support
and offense retain 20 mana for recovery. Recovery runs before discretionary
casting; when missing at most 90 health, Light Healing avoids a potentially
wasteful small health potion. If potions are unavailable, it also attempts
Light Healing for larger deficits. Each cast verifies mana plus health, haste
condition, or target damage before falling back to a potion or normal melee.

The service cycle sells known surplus, restores five small health potions and
one meat, deposits carried money, and withdraws 100 gp. Hunting ends after the
configured duration or below 30 oz free capacity. Remaining top-level backpack
loot is moved through a reachable town depot locker into that player's real
depot chest. Nested containers are opened and deposits are verified through
normal item movement. Equipped items, the root backpack, currency, rope,
shovel, and supply reserves are retained.

Item value comes from tagged shop offers. Currency uses intrinsic value; other
loot must have a known buyer. Corpse contents are ranked by value per weight,
and more valuable loot may replace lower-density sellable cargo. Unknown items,
equipment, containers, currency, tools, and supplies are not replacement
candidates.

The bot identifies only server-classified corpse containers and checks normal
corpse ownership. It opens the corpse through normal item use before inspecting
contents. Empty and non-lootable corpses are skipped; skinning and other
secondary corpse actions are outside this behavior.

## Rewards and departure

Reward discovery is limited to loaded shared quest containers handled by action
ID `2000`. Rookgaard keeps its bounded reward area. Thais uses the same weighted
200-tile temple boundary as hunt discovery. The bot traverses complete nested
bundles, protects unknown items, and applies the same readiness, hunt-unlock,
and Pareto rules used for NPC equipment offers. Equipment evaluation is limited
to 16 unique item-and-weight simulations per scan; later equipment candidates
are preserved and rejected with `unique_item_evaluation_budget_exhausted`. The
bot verifies storage and inventory changes after normal use, preserves displaced
items, and equips useful upgrades through normal item movement. Routes longer
than 120 steps or requiring a door, rope, or shovel action are not simple pickup
objectives. At most 14 nested reward ancestors can be opened.

Action ID `2001` is excluded except for the hard-coded doublet reward with unique
ID `56002` and item ID `2485`. Dedicated quest scripts, levers, and other
non-container rewards require separate behavior.
Candidate telemetry reports `map_reward` as the acquisition source and gives
explicit malformed, unsupported, non-improving, capacity, inventory, utility,
and route rejection reasons. Transient claim state is not persisted; storage,
inventory, and equipment reconstruct the next decision after restart.

An unpromoted level 8 through 10 player can select the live tagged Oracle. The
bot derives a route, says `hi`, `yes`, `thais`, `knight`, `yes`, then verifies
vocation `4`, town `2`, and the registered Thais temple position. It remains
server-owned and continues directly into mainland service.

## Recovery and configuration

Death keeps normal corpse creation, penalties, saving, removal, and temple
login. The manager retains ownership, waits, reloads the same character, and
starts a fresh controller in service. It does not resume routes, targets,
containers, conversations, pending actions, or objectives.

Deaths within five minutes use exponential relog backoff capped at 60 seconds;
five minutes of life resets the count. A failed load permits three total relog
attempts. Ownership and human-login blocking remain active through delays,
retries, and abandonment.

| Environment variable | Default | Contract |
| -------------------- | ------: | -------- |
| `PLAYERBOT_ENABLED` | `true` | Only exact `false` disables controller startup. Provisioning still runs. |
| `PLAYERBOT_HUNT_DURATION_SECONDS` | `300` | Minimum one second. |
| `PLAYERBOT_RELOG_DELAY_SECONDS` | `5` | Initial death delay; exponential backoff caps at 60 seconds. |
| `PLAYERBOT_MAX_CONSECUTIVE_DEATHS` | `3` | Minimum one; the next death is abandoned. |
| `PLAYERBOT_SPEED_BONUS` | `300` | Login movement delta clamped from `0` through `1000`; combat and action delays do not change. |

Invalid numeric text uses the default. Recreate the server container after a
change.

## Telemetry

Playerbot telemetry is JSON Lines on server stdout:

```powershell
docker compose -f server/compose.yaml logs --no-log-prefix --since 30m server | Where-Object { $_ -match '"component":"playerbot"' }
```

Every record has `schema: 1`, UTC RFC 3339 `ts`, `component`, `event`, `bot`,
persistent `player_id`, and `position`. Event-specific fields are conditional.
States, actions, results, statuses, and reasons use stable lowercase values.

| Family | Events and purpose |
| ------ | ------------------ |
| Lifecycle | `lifecycle`, `state_transition`, `objective_transition`, `terminal` record ownership and controller state. |
| Goals | `goal_candidate`, `goal_selection`, `goal_result` expose arbitration evidence and decision IDs. |
| Rewards | `strategy_candidate`, `reward_inspection`, `strategy_selection`, `strategy_objective_result` expose bundle selection and verification. |
| Equipment | `equipment_offer_candidate` and `equipment_offer_shadow` expose loaded tagged-shop offers, loadout and hunt deltas, reserve and route checks, and non-mutating `would_buy` or `would_equip` decisions. Live `buy_equipment` goals use `strategy_selection`, `action_result`, `strategy_objective_result`, and `goal_result` to record purchase, carried-item recovery, displacement, equip verification, and fallback. |
| Spell training | `spell_trainer_discovered`, `spell_candidate`, `strategy_selection`, `action_result`, and `goal_result` expose loaded offers, eligibility rejections, provider/route choice, and exact payment verification. |
| Spell casting | `action_result` with `action="cast_spell"` records the need, semantic `policy_candidate`, selected method, mana reserve, normal-path request, engine result, observed outcome, and fallback. `legal_candidates` contains only normal-path casts confirmed by resource evidence. |
| Actions | `action_result`, `target_changed`, `service_discovered`, `npc_reply`, `stuck` record externally relevant attempts and outcomes. |
| Hunting | `hunt_region_candidate`, `hunt_region_scan`, `hunt_region_selection`, `hunt_region_outcome`, `hunt_region_patrol` expose planner inputs and results. |
| Navigation | `navigation_progress` records bounded recovery such as oscillation suppression. |
| Health | `summary` reports cumulative timing, action, failure, stuck, and suppression counters every 60 seconds. |

Successful movement is not logged per tile. Repeated identical transitions,
target changes, and action failures are emitted at most once per 60 seconds;
`summary.suppressed_events` counts omissions. Counters cover one in-memory
controller lifetime. Docker retains three 10 MiB server log files.

Target pursuit uses `action_result` with `action="target_pursuit"`.
`result="started"` includes `target_id` and `last_seen_position`;
`result="reacquired"` includes `target_id`; and `result="abandoned"` includes
`target_id` plus `reason`. Current abandonment reasons are
`last_seen_position_reached` and `pursuit_budget_exhausted`.
`state_transition` exposes entry to and exit from `target_pursuit`.

When `GOD Admin` is online, selected objectives and verified transactions also
appear as private messages and orange status text. JSONL remains authoritative.

## Human inspection

Stop the server cleanly, recreate it with the controller disabled, then log in
as `bot-one` / `bot-one`:

```powershell
docker compose -f server/compose.yaml stop server
$env:PLAYERBOT_ENABLED = "false"
docker compose -f server/compose.yaml up --detach --force-recreate server
```

Log out before restoring normal startup with
`Remove-Item Env:PLAYERBOT_ENABLED` and recreating the server. Live ownership
transfer is unsupported. See [`testing.md`](testing.md) for validation commands.
