# Contributing

Read [AGENTS.md](AGENTS.md) first. It contains the technical rules and current
project contracts.

## Pace

Development here is both fast and slow.

AI makes implementation fast. The project itself is intentionally structured:
playerbots should operate through normal game mechanics, derive knowledge from
the world where practical, recover from failure, and eventually scale beyond
one bot.

Do not mistake commit velocity for a preference for quick patches without clear
boundaries.

## Style

The maintainer tends to generalize early and aggressively. You do not have to.

A small, local solution is fine when the general abstraction is not yet
justified. Prefer code that is easy to replace over speculative machinery. If
you introduce temporary hand-authored world knowledge or a special case, keep
it explicit and isolated so it cannot silently become permanent policy.

Do not be intimidated by the existing architecture. Improve the system you
understand.

## Pull requests

Be concise.

A PR description should explain the change, not retell the diff.

A PR description should normally say:

- what behavior changed;
- why it changed, when that is not obvious;
- the important evidence;
- any material limitation or unfinished case.

Include implementation details only when they help reviewers assess risk.

Do not add generic summaries, implementation walkthroughs, ceremonial sections,
repeated test output, or AI-generated filler. Match the existing repository
style: short, technical, and specific.

Keep contribution descriptions concise without omitting evidence or material
limits.
