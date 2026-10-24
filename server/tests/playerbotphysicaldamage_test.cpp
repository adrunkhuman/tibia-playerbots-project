#include "playerbotphysicaldamage.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

namespace {
double fixedReference(int damage, int armor, int defense)
{
	const int armorLow = armor > 3 ? armor / 2 : armor > 0 ? 1 : 0;
	const int armorHigh = armor > 3 ? armor - (armor % 2 + 1) : armorLow;
	double result = 0;
	for (int shield = defense / 2; shield <= defense; ++shield)
		for (int plate = armorLow; plate <= armorHigh; ++plate)
			result += std::max(0, damage - shield - plate);
	return result / (defense - defense / 2 + 1) / (armorHigh - armorLow + 1);
}

void integerBlockRanges()
{
	for (int damage = 0; damage <= 100; ++damage)
		for (int armor = 0; armor <= 20; ++armor)
			for (int defense = 0; defense <= 30; defense += 5)
				assert(std::abs(playerBotExpectedPhysicalHit(damage, damage, armor, defense) -
				    fixedReference(damage, armor, defense)) < 1e-9);
	assert(playerBotExpectedPhysicalHit(0, 8, 13, 10) == 0);
	assert(playerBotExpectedPhysicalHit(0, 15, 13, 10) < 0.02);
	assert(playerBotExpectedPhysicalHit(0, 15, 13, 0) > 1);
	assert(std::abs(playerBotExpectedPhysicalHit(0, 40, 0, 0) - 20) < 1e-12);
}

void normalRandomReference()
{
	// Independent sampled reference follows tools::normal_random and the
	// integer ranges in Creature::blockHit. Deterministic seed, ample margin
	// for sampling error; fixed-hit cases above check the exact block algebra.
	std::mt19937 random(272);
	std::normal_distribution<float> normal(0.5f, 0.25f);
	struct Case { int minimum, maximum, armor, defense; };
	for (const Case sample : {Case{0,15,13,10}, Case{0,40,13,10}, Case{0,105,13,10},
	                         Case{2,95,8,0}, Case{0,32,8,8}, Case{5,98,15,0}}) {
		const int width = sample.maximum - sample.minimum;
		const int armorLow = sample.armor > 3 ? sample.armor / 2 : sample.armor > 0 ? 1 : 0;
		const int armorHigh = sample.armor > 3 ? sample.armor - (sample.armor % 2 + 1) : armorLow;
		std::uniform_int_distribution<int> armor(armorLow, armorHigh);
		std::uniform_int_distribution<int> shield(sample.defense / 2, sample.defense);
		double total = 0;
		constexpr int trials = 200000;
		for (int i = 0; i < trials; ++i) {
			const float v = normal(random);
			const int increment = v < 0 ? width / 2 : v > 1 ? (width + 1) / 2 : std::round(v * width);
			total += std::max(0, sample.minimum + increment - armor(random) - shield(random));
		}
		const double expected = playerBotExpectedPhysicalHit(sample.minimum, sample.maximum, sample.armor, sample.defense);
		assert(std::abs(total / trials - expected) < 0.2);
	}
}
}

int main()
{
	integerBlockRanges();
	normalRandomReference();
	std::cout << "playerbot physical damage contracts passed\n";
}
