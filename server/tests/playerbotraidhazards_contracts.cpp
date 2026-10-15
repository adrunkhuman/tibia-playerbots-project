#include "playerbotraidhazards.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <type_traits>

namespace {
using Records = std::vector<PlayerBotRaidHazard>;

const PlayerBotRaidHazard& at(const Records& records, std::string_view raid, const Position& from) {
	const auto found = std::find_if(records.begin(), records.end(), [&](const auto& hazard) {
		return hazard.raid == raid && hazard.from == from;
	});
	assert(found != records.end());
	return *found;
}

uint32_t count(const PlayerBotRaidHazard& hazard, const std::string& name) {
	const auto found = std::find_if(hazard.monsters.begin(), hazard.monsters.end(), [&](const auto& monster) {
		return monster.name == name;
	});
	assert(found != hazard.monsters.end());
	return found->count;
}

std::string body(const std::string& source, const std::string& signature) {
	const auto signatureStart = source.find(signature);
	assert(signatureStart != std::string::npos);
	const auto start = source.find('{', signatureStart);
	assert(start != std::string::npos);
	size_t depth = 1;
	for (size_t i = start + 1; i < source.size(); ++i) {
		if (source[i] == '{') ++depth;
		if (source[i] == '}' && --depth == 0) return source.substr(start + 1, i - start - 1);
	}
	assert(false && "unterminated function body");
	return {};
}

// Scheduling/reset contracts need the engine; check their production call sites
// without linking a second game world into this pure registry executable.
void sourceContracts(const char* filename) {
	std::ifstream file(filename);
	assert(file.good());
	const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	const auto start = body(source, "void Raid::startRaid()");
	const auto collect = start.find("event->appendPlayerBotHazards(footprints)");
	const auto publish = start.find("PlayerBotRaidHazards::instance().activate(name, footprints, true)");
	const auto schedule = start.find("g_scheduler.addEvent");
	assert(start.find("for (const RaidEvent* event : raidEvents)") < collect);
	assert(collect < publish && publish < schedule && schedule != std::string::npos);

	const auto reset = body(source, "void Raid::resetRaid()");
	assert(reset.find("PlayerBotRaidHazards::instance().retirePending(name)") != std::string::npos);
	assert(reset.find("clear(") == std::string::npos);
	const auto stop = body(source, "void Raid::stopEvents()");
	assert(stop.find("PlayerBotRaidHazards::instance().retirePending(name)") != std::string::npos);
	const auto clear = body(source, "void Raids::clear()");
	const auto reload = body(source, "bool Raids::reload()");
	assert(clear.find("if (running) running->stopEvents()") != std::string::npos);
	assert(clear.find("raid->stopEvents()") < clear.find("delete raid"));
	assert(reload.find("PlayerBotRaidHazards") == std::string::npos);
	assert(reload.find("clear()") != std::string::npos);
	assert(reload.find("loadFromXml()") != std::string::npos);
	// No lifecycle path may erase memory of surviving monsters.
	assert(source.find("PlayerBotRaidHazards::instance().clear()") == std::string::npos);

	const auto wave = body(source, "void Raid::executeRaidEvent(");
	const auto waveCollect = wave.find("raidEvent->appendPlayerBotHazards(footprints)");
	const auto wavePublish = wave.find("PlayerBotRaidHazards::instance().activate(name, allFootprints)");
	const auto pendingPublish = wave.find("PlayerBotRaidHazards::instance().setPending(name, remainingFootprints)");
	const auto execute = wave.find("raidEvent->executeEvent()");
	assert(waveCollect < wavePublish && wavePublish < pendingPublish && pendingPublish < execute);
	assert(execute != std::string::npos);
	const auto spawnOnly = body(wave, "if (!footprints.empty())");
	assert(spawnOnly.find("activate(name, allFootprints)") != std::string::npos);
	assert(spawnOnly.find("setPending(name, remainingFootprints)") != std::string::npos);
	assert(spawnOnly.find("for (size_t i = 0; i < raidEvents.size(); ++i)") != std::string::npos);
	assert(spawnOnly.find("raidEvents[i]->appendPlayerBotHazards(allFootprints)") != std::string::npos);
	assert(spawnOnly.find("if (i > nextEvent) raidEvents[i]->appendPlayerBotHazards(remainingFootprints)") != std::string::npos);

	const auto single = body(source, "void SingleSpawnEvent::appendPlayerBotHazards(");
	assert(single.find("{position, position, {{monsterName, 1}}}") != std::string::npos);
	const auto area = body(source, "void AreaSpawnEvent::appendPlayerBotHazards(");
	assert(area.find("footprint{fromPos, toPos, {}}") != std::string::npos);
	assert(area.find("for (const MonsterSpawn& spawn : spawnList)") != std::string::npos);
	assert(area.find("{spawn.name, std::max(spawn.minAmount, spawn.maxAmount)}") != std::string::npos);
	assert(single.find("g_game") == std::string::npos && area.find("g_game") == std::string::npos);
	assert(single.find("createMonster") == std::string::npos && area.find("createMonster") == std::string::npos);
}

void waveContracts() {
	auto& registry = PlayerBotRaidHazards::instance();
	registry.clear();
	const Position point(200, 300, 7), from(210, 310, 7), to(220, 320, 7);
	const PlayerBotRaidFootprint single{point, point, {{"Giant Spider", 1}}};
	const PlayerBotRaidFootprint firstWave{from, to, {{"Dragon", 3}}};
	const PlayerBotRaidFootprint secondWave{from, to, {{"Rat", 4}, {"Wolf", 2}}};
	const std::vector<PlayerBotRaidFootprint> forecast{firstWave, secondWave, single};
	const auto assertForecast = [&](const Records& records) {
		assert(records.size() == 2);
		const auto& area = at(records, "waves", from);
		assert(area.monsters.size() == 3);
		assert(count(area, "Dragon") == 3 && count(area, "Rat") == 4 && count(area, "Wolf") == 2);
		assert(count(at(records, "waves", point), "Giant Spider") == 1);
	};
	const auto publishWave = [&](const std::vector<PlayerBotRaidFootprint>& remaining) {
		// Same full forecast plus independent pending update as executeRaidEvent.
		registry.activate("waves", forecast);
		registry.setPending("waves", remaining);
		assertForecast(*registry.snapshot());
	};
	registry.activate("waves", forecast, true);
	const auto initial = registry.snapshot();
	assertForecast(*initial);
	assert(at(*initial, "waves", from).pending && at(*initial, "waves", point).pending);
	const auto initialRevision = registry.revision();

	publishWave({secondWave, single});
	const auto first = registry.snapshot();
	assert(registry.revision() == initialRevision + 2);
	assert(at(*first, "waves", from).id > at(*initial, "waves", from).id);
	assert(at(*first, "waves", from).pending && at(*first, "waves", point).pending);

	// An announcement/script has no static footprint: source contracts ensure
	// the wave publication path is skipped, and an empty activation is a no-op.
	const auto beforeAnnouncementRevision = registry.revision();
	registry.activate("waves", {});
	assert(registry.snapshot() == first && registry.revision() == beforeAnnouncementRevision);

	publishWave({single}); // rats spawn later; surviving dragons remain in metadata
	const auto second = registry.snapshot();
	assert(registry.revision() == initialRevision + 4);
	assert(at(*second, "waves", from).id > at(*first, "waves", from).id);
	assert(!at(*second, "waves", from).pending && at(*second, "waves", point).pending);

	publishWave({});
	const auto final = registry.snapshot();
	assert(registry.revision() == initialRevision + 5);
	assert(!at(*final, "waves", from).pending && !at(*final, "waves", point).pending);
	assert(at(*final, "waves", from).id > at(*second, "waves", from).id);
	const auto completedRevision = registry.revision();
	registry.retirePending("waves"); // normal completion needs no further mutation
	assert(registry.revision() == completedRevision && registry.snapshot() == final);
	assertForecast(*initial);
	assert(at(*initial, "waves", from).pending && at(*first, "waves", from).pending);

	// A repeat gets fresh IDs and full counts, not totals accumulated per signal.
	registry.activate("waves", forecast, true);
	assertForecast(*registry.snapshot());
	assert(registry.revision() == initialRevision + 6);
	assert(at(*registry.snapshot(), "waves", from).pending && at(*registry.snapshot(), "waves", point).pending);
	assert(at(*registry.snapshot(), "waves", from).id > final->back().id);
	registry.clear();
}

void cancellationContracts() {
	auto& registry = PlayerBotRaidHazards::instance();
	registry.clear();
	const Position from(300, 400, 7), to(310, 410, 7);
	const PlayerBotRaidFootprint dragon{from, to, {{"Dragon", 1}}};
	const PlayerBotRaidFootprint rat{to, from, {{"Rat", 5}}}; // same normalized rectangle
	registry.activate("cancelled", {dragon, rat}, true);
	registry.activate("unrelated", {dragon}, true);
	const auto before = registry.snapshot();
	const auto revision = registry.revision();
	const auto cancelledId = at(*before, "cancelled", from).id;
	const auto otherId = at(*before, "unrelated", from).id;

	// Cancellation before the first wave (also failure/reset and reload/stop).
	registry.retirePending("cancelled");
	const auto cancelled = registry.snapshot();
	assert(cancelled->size() == 2 && registry.revision() == revision + 1);
	assert(!at(*cancelled, "cancelled", from).pending);
	assert(at(*cancelled, "cancelled", from).id == cancelledId);
	assert(count(at(*cancelled, "cancelled", from), "Dragon") == 1);
	assert(count(at(*cancelled, "cancelled", from), "Rat") == 5);
	assert(at(*cancelled, "unrelated", from).pending && at(*cancelled, "unrelated", from).id == otherId);
	assert(at(*before, "cancelled", from).pending); // old snapshot is unchanged
	registry.retirePending("cancelled");
	registry.retirePending("absent");
	assert(registry.snapshot() == cancelled && registry.revision() == revision + 1);

	// Restart, then cancel between mixed-species waves; counts survive both.
	registry.activate("cancelled", {dragon, rat});
	registry.setPending("cancelled", {rat});
	const auto between = registry.snapshot();
	assert(at(*between, "cancelled", from).pending);
	assert(at(*between, "cancelled", from).id > cancelledId);
	const auto betweenRevision = registry.revision();
	registry.retirePending("cancelled");
	assert(registry.revision() == betweenRevision + 1);
	assert(!at(*registry.snapshot(), "cancelled", from).pending);
	assert(count(at(*registry.snapshot(), "cancelled", from), "Dragon") == 1);
	assert(count(at(*registry.snapshot(), "cancelled", from), "Rat") == 5);
	// Zero-count future spawns do not promise another wave at this footprint.
	registry.setPending("cancelled", {{from, to, {{"Rat", 0}}}});
	assert(registry.revision() == betweenRevision + 1);
	registry.clear();
}
}

int main(int argc, char** argv) {
	assert(argc == 2);
	static_assert(std::is_same_v<decltype(PlayerBotRaidHazards::instance().snapshot()),
	                            std::shared_ptr<const Records>>);
	auto& registry = PlayerBotRaidHazards::instance();
	assert(&registry == &PlayerBotRaidHazards::instance());
	registry.clear();
	const auto empty = registry.snapshot();
	const auto initialRevision = registry.revision();
	registry.clear();
	registry.activate("script only", {});
	registry.activate("empty", {{{1, 2, 7}, {1, 2, 7}, {{"rat", 0}, {"", 1}}}});
	assert(registry.revision() == initialRevision && registry.snapshot() == empty);

	const Position point(100, 200, 7), from(90, 190, 7), to(110, 210, 8);
	registry.activate("wolves", {
		{point, point, {{"War Wolf", 1}}},
		{to, from, {{"Wolf", 55}, {"War Wolf", 70}}}, // normalized, multiple floors
		{from, to, {{"Wolf", 5}, {"War Wolf", 2}, {"Wolf", 3}}},
	});
	const auto first = registry.snapshot();
	assert(empty->empty());
	assert(registry.revision() == initialRevision + 1 && first->size() == 2);
	assert(!at(*first, "wolves", point).pending && !at(*first, "wolves", from).pending);
	const auto pointId = at(*first, "wolves", point).id;
	const auto areaId = at(*first, "wolves", from).id;
	assert(pointId != 0 && areaId > pointId);
	assert(at(*first, "wolves", from).to == to);
	assert(at(*first, "wolves", from).monsters.size() == 2);
	assert(count(at(*first, "wolves", from), "Wolf") == 63);
	assert(count(at(*first, "wolves", from), "War Wolf") == 72);
	for (int i = 0; i < 1000; ++i) assert(registry.snapshot() == first);

	registry.activate("wolves", {{point, point, {{"Rat", 4}}}});
	const auto second = registry.snapshot();
	assert(registry.revision() == initialRevision + 2 && second->size() == 2);
	assert(at(*second, "wolves", point).id > areaId);
	assert(at(*second, "wolves", point).monsters.size() == 1);
	assert(count(at(*second, "wolves", point), "Rat") == 4);
	assert(at(*second, "wolves", from).id == areaId); // unrelated footprint unchanged
	assert(at(*first, "wolves", point).id == pointId && count(at(*first, "wolves", point), "War Wolf") == 1);

	registry.activate("other raid", {{point, point, {{"Dragon", 2}}}});
	assert(registry.snapshot()->size() == 3); // same rectangle, different raid
	registry.activate("wolves", {{point, to, {{"Wolf", 1}}}});
	assert(registry.snapshot()->size() == 4); // overlapping is not matching
	const auto maximum = std::numeric_limits<uint32_t>::max();
	registry.activate("saturating", {{point, point, {{"Rat", maximum}, {"Rat", 1}}}});
	assert(count(at(*registry.snapshot(), "saturating", point), "Rat") == maximum);

	const auto beforeClear = registry.snapshot();
	const auto lastId = beforeClear->back().id;
	const auto beforeClearRevision = registry.revision();
	registry.clear();
	assert(registry.snapshot()->empty() && registry.revision() == beforeClearRevision + 1);
	assert(beforeClear->size() == 5 && first->size() == 2 && second->size() == 2);
	registry.activate("after clear", {{point, point, {{"Rat", 1}}}});
	assert(registry.snapshot()->front().id > lastId);
	registry.clear();

	// Oldest-footprint eviction, including more than one capacity in one call.
	std::vector<PlayerBotRaidFootprint> many;
	for (uint16_t i = 0; i < 300; ++i) many.push_back({{i, 10, 7}, {i, 10, 7}, {{"Rat", 1}}});
	registry.activate("bounded", many, true);
	const auto bounded = registry.snapshot();
	assert(std::all_of(bounded->begin(), bounded->end(), [](const auto& hazard) { return hazard.pending; }));
	assert(bounded->size() == PlayerBotRaidHazards::MaxFootprints);
	assert(bounded->front().from.x == 44 && bounded->back().from.x == 299);
	const auto evictId = bounded->front().id;
	const auto refreshId = at(*bounded, "bounded", {100, 10, 7}).id;
	const auto stableId = at(*bounded, "bounded", {101, 10, 7}).id;
	const auto boundedRevision = registry.revision();
	registry.activate("bounded", {{{100, 10, 7}, {100, 10, 7}, {{"Rat", 2}}}});
	assert(registry.snapshot()->size() == PlayerBotRaidHazards::MaxFootprints);
	assert(registry.snapshot()->front().id == evictId); // refresh does not evict a neighbor
	assert(registry.snapshot()->back().id > refreshId && !registry.snapshot()->back().pending);
	assert(at(*registry.snapshot(), "bounded", {101, 10, 7}).id == stableId);
	assert(at(*registry.snapshot(), "bounded", {101, 10, 7}).pending);
	assert(at(*bounded, "bounded", {100, 10, 7}).pending);
	assert(registry.revision() == boundedRevision + 1);
	for (int i = 0; i < 300; ++i) {
		registry.activate("bounded", {{{100, 10, 7}, {100, 10, 7}, {{"Rat", 2}}}});
		assert(registry.snapshot()->size() == PlayerBotRaidHazards::MaxFootprints);
	}
	registry.activate("new", {{point, point, {{"Dragon", 1}}}});
	assert(registry.snapshot()->size() == PlayerBotRaidHazards::MaxFootprints);
	assert(registry.snapshot()->front().from.x == 45);
	assert(bounded->front().id == evictId && count(at(*bounded, "bounded", {100, 10, 7}), "Rat") == 1);
	registry.clear();

	sourceContracts(argv[1]);
	waveContracts();
	cancellationContracts();
	std::cout << "playerbot raid hazards contracts passed\n";
}
