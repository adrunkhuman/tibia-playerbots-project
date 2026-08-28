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

The controller lifecycle is `Running`, `Paused`, or `Stopped`. `Running`
evaluates one turn at a time and keeps at most one scheduler callback pending.
An earlier requested deadline replaces a later pending callback, and generation
checks suppress stale callbacks. `Paused` suppresses future turns at a fixture
restart checkpoint without emitting a terminal event. `Stopped` is terminal: it
cancels the pending callback, clears active planning and navigation, emits one
`terminal` event, and performs no later turns. A restart creates a fresh
`Running` controller from persisted player state.

`PlayerBotController` owns scheduling, player lookup, lifecycle, turn routing,
world-command execution, navigation, survival, service, depot, and progression.
`PlayerBotHuntCoordinator` owns combat, corpse-loot, hunt planning and cycles,
patrol, danger and death transitions, and shared hunt-region cooldown access.
Service, depot, and progression remain in the controller because they share its
navigation and lifecycle concerns.

## Provisioning

The `playerbot-setup` Compose service runs before the server. It owns the
development account `bot-one`, character `Bot One`, and `player_bots` registry.
Provisioning is idempotent: it does not restore spent money, replace occupied
equipment slots, or take over an unrelated same-named or deleted character.

On a clean schema, Bot One starts as a level 8 Knight at the Carlin temple with
sword and shielding skill 20, legion helmet, chain armor, studded legs, leather
boots, Carlin sword, copper shield, backpack, rope, shovel, and 400 gp bank
balance. It starts without carried money, food, or potions. Global `freePremium`
supplies premium access.

Inspect setup and startup with:

```powershell
docker compose -f server/compose.yaml logs playerbot-setup server
```

## Decisions

The controller evaluates top-level goals only at safe boundaries: startup,
completed or failed pickup, completed service/depot work, and hunt deadline or
capacity return. Hunt completion enters the neutral `Idle` phase before goal
selection so a non-hunt opportunity can compete with the next hunt. The
controller does not switch goals during movement, combat, looting,
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
| Magic training | 350 |
| Hunt | 300 |
| Economic reward | 250 |

Service needs and reward value adjust these baselines, so candidates can cross
nominal tiers. Equal scores keep declaration order: departure, service, pickup,
spell training, equipment, magic training, then hunt. Successful pickup and
spell-training families cool down for five minutes; failed or interrupted
families cool down for 60 seconds.

Turn routing is independent of utility scores and uses fixed preemption order:
progression, magic training, hunt start, hunt planning, suspended-loot retry,
loot, hunt finish, service, depot return, depot deposit, traversal combat,
target pursuit, then ordinary hunting. A paused or stopped controller returns
no command.

## Navigation and hunting

Callers provide exact, within-range, or alternative destination goals rather
than ordered transition checkpoints. Shared map-derived topology indexes static
walk regions and directed floor changes, ladders, rope and shovel holes,
direct-use holes, map teleports, unlocked doors, and level doors. Keyed or quest
doors with an Action ID remain unavailable unless they are level doors. Detailed
planning searches loaded map state within a 1024-tile endpoint margin and stops
after 100,000 expanded nodes. Cardinal movement costs 10; diagonal movement
costs 30. Failed steps are excluded for 10 seconds. Repeated A-B oscillation
suppresses the implicated transition for two minutes. Topology is built at
startup and rebuilt after successful actions, items, scripts, or full reload
commands. Other runtime map or transition mutations do not invalidate it
automatically; use one of those supported reloads or restart the server before
relying on topology-based planning again.

Hunt regions come from all loaded hostile, attackable spawns. The shared cache
groups overlapping eight-tile spawn kernels through spatial buckets. Each
controller scores the global region set in bounded batches using health,
equipment, weapon, defense, skill, cooldowns, observed performance, and an
optimistic projected-XP bound. It validates suitable candidates incrementally
through normal navigation. Once the best proven reachable candidate cannot be
beaten by any unvalidated optimistic bound, remaining route checks are deferred.
The selected region is therefore the best proven candidate under the bound; the
planner does not calculate a complete route-adjusted ordering of every suitable
region. Navigation remains behind the destination/reachability interface so a
later navigator can replace tile planning without changing hunt selection.

Resetting navigation clears only the current route and recovery state. It does
not cancel an active hunt-region plan. The controller cancels hunt planning when
it leaves the hunt phase or when an explicit interruption, such as Oracle
departure, replaces the hunt.

One scan uses an immutable combat, recovery, resource, and frontier snapshot for
every scoring batch. Position, level, stamina, cooldown exclusions, map revision,
or a health decrease cancels the scan. Other equipment, mana, potion, or learned
spell changes apply to the next scan. Candidate telemetry describes the captured
snapshot rather than necessarily the player's state when selection completes.

Predicted pressure models up to five overlapping hostile spawns, current
equipment, one conservative small-health-potion recovery, and learned Light
Healing when its loaded metadata, current mana, cooldown, and legality allow
it. The prediction uses the spell's minimum server formula and preserves
recovery mana. A candidate is lethal at or above the bot's current health; it
does not credit potion or spell recovery before that lethal point.
`raw_threat_ratio` reports unrecovered predicted damage against maximum health.
`threat_ratio` subtracts conservative recovery for challenge ranking, while
`predicted_lethal` compares unrecovered damage with snapshotted current health.
`recovery.available_before_lethal` is therefore currently zero.

Each controller starts with a `0.20` pressure frontier. Every nonlethal region at
or below the frontier plus `0.05` headroom competes by projected XP; there is no
minimum pressure band. Thirty seconds of active combat, at least one recorded kill,
near-full health, and no verified recovery escalates the frontier by `0.025`.
Verified potion or healing-spell pressure, observed danger, or death backs it
off by `0.05`; the next two qualifying hunts hold before escalation resumes.
The target stays within `0.10` through `0.40`. It is an adaptation bound, not a
lethal threshold. Travel, idle full health, and target engagement without a
kill do not count as easy evidence. Active-combat uptime remains diagnostic; it
is not an escalation threshold because sparse, fast-kill hunts naturally have
low uptime.
Frontier and performance state are per-controller and reset on relog or restart.

Taking one maximum health pool of active-combat damage within the first two
minutes abandons the region for ten minutes. Completed hunts update a
per-controller XP correction after at least 30 seconds and one kill; the sample
is clamped to `0.25` through `2.0` and uses a 65/35 rolling blend. When all
globally scored candidates are unavailable, the controller emits
`hunt_scope_exhausted` and retries after 30 seconds. A
successful selection resets the counter; three consecutive exhausted scans stop
the controller instead of rescanning an unchanged world snapshot forever.

When an attacked monster leaves normal positional or creature visibility, or
the attack association is lost, the bot clears chase and pursues a reachable
tile adjacent to its last observed position. It never updates this goal from a
hidden creature's live state.
Visible targets may update the approach goal and can be reacquired within six
tiles. Pursuit lasts at most five seconds and six tiles of Chebyshev displacement.
Reaching the approach point or exhausting either budget returns to patrol and
suppresses that target for ten seconds. A 60-second traversal combat timeout
retains its separate 120-second suppression.

Defensive combat runs only outside the hunt cycle. During a hunt, only the
active traversal combat episode contributes combat evidence; target pursuit is
part of that episode for hunt death attribution, but does not itself add active
combat time or damage.

The map-derived region planner and bounded pursuit are prototypes, not
whole-map hierarchical navigation or general creature memory. Regression
fixtures use fixed destinations unless their focused mode explicitly exercises
dynamic planning or pursuit.

## Survival, service, and loot

At or below 60% health, the bot uses one recovery potion through normal
use-with-creature handling and verifies consumption and net healing. Vocationless
bots use small health potions (`8704`, 60-90 healing); bots with a non-zero
vocation use health potions (`7618`, 125-175 healing). The selected potion drives
readiness, restocking, recovery estimates, spell fallback, reserves, and
telemetry. Missing stock redirects the bot to service. Food is optional: its
absence does not block a hunt or select service. The current soft preference
collects up to two standard 8.60 food items. The bot can consume any item
classified as standard food, respects the normal fullness limit, and backs off
for five minutes after three unverified uses.

Loaded NPC state is the source of truth for structured capabilities. Every live
NPC with non-empty `getShopOffers()`, `getSpellOffers()`, or registered
`getTravelOffers()` is a shop, spell trainer, or travel provider respectively.
Banker scripts register the same loaded-state capability with `Npc():addBanker()`.
These capabilities do not require duplicate XML labels. The
`playerbot_service` role `oracle` remains explicit because its bespoke dialogue
has no structured native interface. `disabled` is the explicit override for
suppressing an NPC's otherwise discoverable structured capabilities.
The bot greets the selected NPC, treats a private reply as focus acknowledgement,
and opens the normal trade window. Reply text is not interpreted. Provider
selection is global, but route validation is bounded. Cost-ordered provider
scans apply their route and catalog budgets only after discovery, rather than
letting NPC registration order decide which providers are considered. Service
validates one provider approach per scheduler decision and yields for 500 ms
before continuing an unfinished provider search. An exhausted local range-goal
suppresses the provider for the current workflow, and selection continues with
the next matching provider.
Movement by at most eight tiles on the same floor triggers a fresh local route,
while larger or cross-floor movement rejects the provider.
Equipment discovery examines at most 16 providers and 64 catalog offers per goal
scan, validates at most four provider routes, and resumes from later providers on
the next scan.

Service goal selection counts sellable inventory only when a live shop NPC
currently publishes a matching offer. Selling uses a pure provider utility over
expected sale value and route cost. It starts with a geometric estimate and
reranks after each bounded route validation using the resulting path length.
The default profile values one gp
as ten route steps, which favors local service unless a remote provider's total
proceeds justify the trip. The weights are isolated so later personality policy
can vary thrift and travel aversion without weakening reachability, eligibility,
or survival constraints. Required purchases choose the nearest loaded offer,
then revalidate the NPC capability, offer, and trade window before transacting.
Missing required offers, unavailable NPCs, and failed shop verification stop
with explicit service errors instead of completing an unverified transaction.

Spell training considers every live NPC with loaded spell offers through
round-robin scans of at most four trainers. Each scan evaluates up to eight
approach tiles per trainer with a 5,000-node limit per approach and a 20,000-node
aggregate route budget; later trainers are evaluated on subsequent scans. It
derives trainer offers from loaded NPC scripts and rejects offers with a registry
mismatch, wrong vocation, level, premium status, learned state, missing supply
reserve, insufficient funds after the 100 gp carried reserve plus the cost of
the current stock gap to the 10-potion restock target, or an unavailable route.
A selected spell uses normal `hi`, keyword, and `yes`
dialogue. Completion requires both learned state and the exact total-money
delta. Learned-spell persistence reconstructs completion after restart and
prevents a repurchase.

Loaded `StdModule.travel` keyword trees publish static boat, carpet, and other
NPC travel offers with their dialogue path, destination, fare, level, premium,
and opaque-condition status. Route planning may combine multiple offers. Every
walk to a provider and from an arrival point is validated through bounded normal
navigation. A route must fit the bot's carried-plus-bank funds and is unavailable
while the bot is PZ-locked. Offers with dynamic destinations, custom conditions,
or custom actions are opaque and excluded rather than reimplemented in C++.
Execution greets the selected NPC, follows the registered dialogue, relies on
the normal script for payment and movement, and verifies the exact destination.
A failed NPC/destination pair is suppressed for five minutes. The runtime
executes one travel leg and replans from the observed arrival position.

At controller startup, `npc_capability_audit` reports loaded provider totals and
warns about empty legacy `shop` or `spell_trainer` declarations and `disabled`
metadata that hides structured offers. Names and coordinates appear only in
telemetry; capability and provider decisions use loaded offers, requirements,
prices, and route/service costs.

The casting policy has four audited descriptors: `healing`, `support`,
`melee_offense`, and `ranged_offense`. It currently uses Light Healing for
recovery, Haste on a safe route with at least 20 remaining steps, Berserk at
level 35, and Whirlwind Throw before then against a visible adjacent combat
target. Casts use the normal player speech spell path. The live spell
engine remains authoritative for eligibility, costs, targeting, line of sight,
weapon requirements, aggression, cooldowns, and action constraints. Support
and offense retain 20 mana for recovery. Recovery runs before discretionary
casting; when missing no more than the selected potion's maximum healing (90 or
175), Light Healing avoids a potentially wasteful potion. If potions are
unavailable, it also attempts Light Healing for larger deficits. Each cast
verifies mana plus health, haste condition, or target damage before falling back
to a potion or normal melee.

### Spell calibration

Loaded spell metadata remains authoritative for spell words, legality, mana, and
cooldowns. Formula bounds come from `light_healing.lua`, `whirlwind_throw.lua`,
`berserk.lua`, and `haste.lua`. The engine does not expose those Lua callback
bounds, so `playerbotspellcalibration` maintains a manually synchronized lower
and upper envelope. Review the envelope whenever those callbacks change;
calibration never replaces it. Formula fidelity remains under issue #1. The hunt
planner continues to use only Light Healing's lower envelope for recovery and
predicted-lethal checks.

Observed casts are controller-local ranking evidence only. They cannot alter
formula-minimum healing safety, predicted-lethal rejection, engine legality,
cooldowns, or mana reserves. Profiles are keyed by spell and target class where
relevant, bounded to 12 keys and 64 samples per counter. A thirteenth key
deterministically evicts the least recently observed key; telemetry records the
eviction. Profiles reset when the controller is recreated. Accepted, rejected,
and ambiguous counters each cap at 64. Confidence is accepted samples divided
by eight, capped at one; rejected and ambiguous evidence do not reduce it.
`conservative` is the accepted observed minimum. `ranking` blends the formula
midpoint toward the accepted mean by confidence. Normal combat-hook observations
cap at 10,000 before profiling; profiles clamp values at 60,000. Before full
confidence the #91 policy remains unchanged. At full confidence the controller
can rank existing legal offensive alternatives, but never invents a cross-role
action or bypasses normal `g_game.playerSay` casting.

Healing accepts an exact roll only when missing health is at least the captured
formula maximum and there is no concurrent damage or other recovery. Potential
overheal is `censored_overheal`, not a low roll. Damage accepts attributed
single-target Whirlwind Throw and Berserk samples with stable target ID and
class; melee, other attackers, target loss, unstable targets, and multi-target
Berserk effects are ambiguous. Haste captures newly applied condition identity
and remaining ticks immediately after the engine cast, then estimates duration
as remaining ticks plus at most 2,000 ms elapsed at verification. This is not a
wait for or confirmation of the full duration. Pre-existing or replaced
conditions are rejected. Only accepted healing adds verified hunt recovery
pressure.

### Magic training

Magic training is a one-cast overflow opportunity, not a level target or a
training window. Outside a hunt, the controller forecasts the next active
engine mana-regeneration tick. If `current mana + gain > maximum mana`, it may
make one audited safe cast so the tick does not lose mana, then immediately
returns to normal goal arbitration. Exact full mana is not overflow. Hunting
always keeps mana: it never selects this opportunity.

`ConditionRegeneration::getManaForecast` supplies each active condition's
read-only gain, interval, and time to its next engine tick.
`Creature::getManaRegenerationForecast` aligns condition phases to the server's
condition-execution interval, selects the earliest actual engine cycle, and sums
only default, equipment, or combat conditions due on that cycle. A finite
condition contributes its valid final tick because regeneration executes before
condition expiration; a condition that expires before reaching its next tick is
excluded. `remaining` is time to that aligned execution cycle. The API returns
no forecast for absent or expired conditions, zero gain or interval, or a
protection zone, where the engine pauses regeneration.
Overflow uses strict `current mana + gain > maximum mana`; exact full mana does
not cast. Overflow arithmetic, predicted mana, and lost mana use `uint64_t`.
Telemetry records current and maximum mana, tick gain, interval and remaining
time, prediction, and loss.

The candidate has utility 350: below service, reward, spell learning, and
equipment work, but above hunting. It is infeasible during hunting, combat,
navigation, recovery, pending actions, or a paused/no-overflow forecast. A
result always applies a short cooldown and reselects. Failed verification stops
after that one attempt; there is no retry loop or persisted training state.
Restart recomputes the opportunity from the player and condition.

Descriptors explicitly opt in with safe, priority, effect, and refresh-safe
metadata. This compile-time registry is the extension point for new vocations
and audited spells; additions require matching loaded spell and learned-state
support. Policy preflights enabled state, level, magic level, premium, soul,
mana reserve, exhaustion, weapon, and safe instant-spell shape. It rejects
aggressive, targeted, parameterized, directional, rune, and unsupported spells;
the normal `g_game.playerSay` path remains authoritative for final legality and
cost. Higher useful
priority wins: Haste first when absent, then Great Light when there is no light,
then Light. If all audited effects are active, the lowest-cost refresh-safe
descriptor wins; Light is currently that fallback. A cast must leave 20 mana,
but does not reserve a second spell cost. Verification requires the observed
mana delta to equal loaded cost and either base magic level or configured
`manaSpent` progression to increase; this handles `RATE_MAGIC` and level-boundary
counter resets. The reported `engine_result` describes that observation, not a
return value from `g_game.playerSay`. Calibration observations never select it.

The gameplay fixtures test forecast arithmetic and selector behavior separately.
The forecast fixture covers default and non-default conditions, expiration,
earliest-cycle selection, and gains summed in one engine cycle. The arbitration
fixtures cover a real hunt finish, `Idle`, one normal engine cast, continuation,
and higher-priority service or spell learning. They do not model arbitrary
condition serialization: regeneration phase intentionally resets on load, so
the restart fixture checks a fresh forecast from current persisted mana and
conditions rather than a retained training window.
All scenarios use controlled Lua setup. They prove forecast arithmetic,
arbitration, and normal engine casts under those conditions, not frequency or
utility on an ordinary long-running server.

The service cycle sells known surplus and returns for the selected recovery
potion when the carried count reaches one. It buys enough to carry at least two
and targets 10. Only the selected potion is reserved toward that target; other
health potions may be deposited. A complete restock takes priority when total
carried and bank gold can pay for it; an optional partial restock preserves the
carried-gold reserve. If total gold cannot raise stock above the return
threshold, service stops with `insufficient_potion_funds` without buying an
unusable partial reserve. The cycle deposits carried money and withdraws up to
100 gp without exceeding the bot's total available gold. It does not buy food
merely because none is carried.
Hunting ends after the configured duration or below 30 oz
effective free capacity. Effective capacity is physical free capacity plus the
weight of carried standard food and currency, because food can be consumed or
replaced and currency must not cause a service return. A backpack may therefore
have no physical capacity and remain hunt-ready when its reclaimable cargo keeps
effective capacity above the reserve. Remaining top-level backpack loot is moved through a reachable depot
locker into that player's real depot chest. Depot locality and locker identity
are independent: discovery enumerates
all map-indexed lockers, then validates that each candidate still has its indexed
depot ID. It ranks all standable adjacent squares by weighted current-position
distance, locker ID, locker position, and approach position. It validates at
most two routes per scheduler decision and resumes the sorted queue on the next
decision. A moved player rebuilds the queue from a new anchor. Failed approaches
are suppressed for two seconds, then become
eligible again; a scan containing only suppressed approaches waits for the
earliest suppression expiry without consuming an attempt. Complete unavailable
scans retry after one, two, and four seconds, then stop on the fourth failed
scan. A selected route is transferred into
normal navigation, so it is not planned again. The selected actual locker ID,
rather than the town ID, identifies the opened locker and player depot storage.
Nested containers are opened and deposits are verified through normal item
movement. Equipped items, the root backpack, currency, rope, shovel, the potion
reserve, food, and unknown items are retained. The depot equips carried upgrades
through the normal verified equipment path, then treats displaced and inferior
equipment as ordinary cargo for sale or deposit. This prevents equipment from
permanently consuming capacity without discarding upgrades. A carried weapon
must improve maximum damage with the player's trained weapon skill; higher raw
attack alone does not justify switching weapon classes. Two-handed weapons are
outside the current loadout evaluator and remain protected from automatic sale,
depot deposit, and cargo replacement until two-handed tradeoffs are supported.

Item value comes from loaded shop offers. Currency uses intrinsic value; other
loot must have a known buyer. Corpse contents are ranked by value per weight,
and currency may replace lower-density sellable cargo even when the discarded
cargo has a higher total value. Physical capacity still limits the move. Once two food
items are carried, further food is skipped. Any food remains replaceable by a
more valuable known item because the preference is not a reserve. Known
unequipped equipment is also replaceable; equipped gear is outside the backpack
cargo path. Unknown items, containers, currency, tools, and the potion reserve
are not replacement candidates. Food count, weight, reclaimable capacity, and preference
utility are observable, but food does not yet select a separate acquisition
goal. Later goal arbitration can weigh measured regeneration benefit against
travel, capacity, and service costs without restoring a hard requirement.

The bot identifies only server-classified corpse containers and checks normal
corpse ownership. It opens the corpse through normal item use before inspecting
contents. Empty and non-lootable corpses are skipped; skinning and other
secondary corpse actions are outside this behavior.

If an unchanged corpse route repeatedly fails, looting suspends navigation and
retries the corpse after a bounded delay. It does not discard the loot objective
or repeatedly replan the same route. A timeout ends the loot attempt with
`corpse_inaccessible`.

## Rewards and departure

Reward discovery considers all loaded shared quest containers handled by action
ID `2000`. The bot traverses complete nested bundles, protects unknown items,
and applies the same readiness, hunt-unlock, and Pareto rules used for NPC
equipment offers. Equipment evaluation is limited to 16 unique item-and-weight
simulations per scan; later equipment candidates are preserved and rejected with
`unique_item_evaluation_budget_exhausted`. The bot verifies storage and inventory
changes after normal use, preserves displaced items, and equips useful upgrades
through normal item movement. Routes longer than 120 steps or requiring a door,
rope, shovel, or NPC travel action are not simple pickup objectives. Reward
reachability uses a 100,000-node aggregate budget with at most 10,000 nodes per
approach. At most 14 nested reward ancestors can be opened.

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

Any relog or server restart creates a fresh controller. Persisted player,
inventory, equipment, spell, storage, and depot state is authoritative; routes,
targets, conversations, open containers, pending actions, and objective state
are discarded and reevaluated. Focused depot restart fixtures cover named
approach, locker, chest, deposit, and departure checkpoints. They do not claim
that an arbitrary interrupted shop or item transaction resumes in place.

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
| `PLAYERBOT_HUNT_DURATION_SECONDS` | `1500` | Minimum one second. |
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
A controller `terminal` event is final: no later state, action, or objective
events are emitted by that controller.

| Family | Events and purpose |
| ------ | ------------------ |
| Lifecycle | `lifecycle`, `state_transition`, `objective_transition`, `terminal` record ownership and controller state. |
| Goals | `goal_candidate`, `goal_selection`, `goal_result` expose arbitration evidence and decision IDs. |
| Rewards | `strategy_candidate`, `reward_inspection`, `strategy_selection`, `strategy_objective_result` expose bundle selection and verification. |
| Equipment | `equipment_offer_candidate` and `equipment_offer_shadow` expose loaded shop offers, loadout and hunt deltas, reserve and route checks, and non-mutating `would_buy` or `would_equip` decisions. Live `buy_equipment` goals use `strategy_selection`, `action_result`, `strategy_objective_result`, and `goal_result` to record purchase, carried-item recovery, displacement, equip verification, and fallback. |
| Spell training | `spell_trainer_discovered`, `spell_candidate`, `strategy_selection`, `action_result`, and `goal_result` expose loaded offers, eligibility rejections, provider/route choice, and exact payment verification. |
| Spell casting | `action_result` with `action="cast_spell"` records the need, semantic `policy_candidate`, selected method, mana reserve, normal-path request, engine result, observed outcome, fallback, captured engine bounds, monotonic observation age, target class, distinct synchronous spell-victim count, measured Haste condition ticks, and controller-local calibration counts, range, conservative value, ranking estimate, confidence, and evidence reason. `spell_calibration` separates `classifier_helper` and `profile_math` fixture evidence; `spell_calibration_eviction` records bounded-profile replacement. `legal_candidates` contains only normal-path casts confirmed by resource evidence. |
| Magic training | `goal_candidate`, `goal_selection`, and `goal_result` expose overflow arbitration. `action_result` with `action="magic_training"` emits a requested `engine_path` record and a success or failure `engine_verification` record with spell, reserve, current/maximum/predicted/lost mana, aggregated gain, aligned interval/remaining time, loaded cost, mana delta, and magic progression. `magic_training_fixture` with `source="authoritative_forecast"` is controlled fixture evidence, not a live cast. |
| Actions | `action_result`, `target_changed`, `service_discovered`, `service_provider_rejected`, `npc_reply`, `npc_capability_audit`, and `stuck` record externally relevant attempts, discovery health, provider fallback, and outcomes. |
| Hunting | `hunt_region_candidate`, `hunt_region_scan`, `hunt_region_selection`, `hunt_region_outcome`, `hunt_challenge_frontier`, `hunt_scope_exhausted`, and `hunt_region_patrol` expose planner inputs, recovery assumptions, optimistic-bound route deferral, active-combat and kill evidence, frontier updates, and bounded exhaustion. |
| Navigation | `navigation_progress` records bounded recovery such as oscillation suppression; `npc_travel` records verified travel success or failure. Lifecycle records include loaded shop-provider, spell-trainer, travel-offer, and opaque travel-offer counts. |
| Health | `summary` reports cumulative timing, action, failure, stuck, and suppression counters every 60 seconds. |

`action_result` records with `action="item_disposition"` report moves of
policy-approved loot from invalid equipment slots before sale. The numeric
`source_slot` uses `slots_t` values (`10` is the ammunition slot),
`provider_available` records whether a live seller was selected, and
`disposition` is `sell` or `deposit`. `attempt` counts move requests.
`result="deferred"` with `cooldown_ms` means an unverified move is suppressed
for 60 seconds before another disposition attempt. Real-depot `deposit` records
use the same `source_slot`, `provider_available`, and `disposition` fields.

Successful movement is not logged per tile. Repeated identical transitions,
target changes, and action failures are emitted at most once per 60 seconds;
`summary.suppressed_events` counts omissions. Counters cover one in-memory
controller lifetime. Docker retains three 10 MiB server log files.

`Game::combatChangeHealth` forwards global damage and healing notifications to
the playerbot manager. The controller attributes only synchronous effects that
match its pending normal speech cast. `classifier_helper` records cover
classification boundaries that cannot interleave on that single-threaded path;
they are not live engine-attribution evidence.

`hunt_challenge_frontier.result` is `escalated`, `backoff`, `hold`, `clamped`,
or `insufficient_active_combat`. Its `retreat` field is true only for the
`hunt_region_observed_danger` exit reason. It reports kills and the active-time
and kill thresholds used for qualification; `active_combat_uptime` is diagnostic.
`hunt_scope_exhausted` reports total,
scored, suitable, and reachable candidate counts plus the retry delay and either
`local_scope_exhausted` or `route_validation_budget_exhausted`.

`action_result` with `action="depot_discover"` reports indexed, in-scope, and
standable candidate counts. Success records the actual `depot_id`, locker and
approach positions, weighted distance, route steps, and expanded nodes.
`unavailable` records `no_local_locker`, `no_standable_approach`, or
`no_reachable_locker`; `continuing` records
`route_validation_budget_exhausted` while the bounded candidate queue remains.
Unavailable records include the retry attempt and currently suppressed approach
count; the fourth unavailable round emits the normal `depot_unavailable`
terminal event.

An unchanged backpack deposit retries three times. After the third rejection,
the bot moves the blocked item to its current tile through normal item movement,
verifies the inventory and ground deltas, emits `result="discarded"` with
`reason="depot_rejected"`, and continues the deposit workflow. Rejected slotted
deposits remain deferred rather than discarded.

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
