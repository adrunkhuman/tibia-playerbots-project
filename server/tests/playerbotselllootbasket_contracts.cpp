#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "playerbotselllootbasket.h"

using playerbot::SellLootBasketItem;
using playerbot::playerBotSellLootBasket;

int main()
{
	// Six occupied backpack slots leave room for 14 non-stackable rapiers,
	// not the 15th rapier or any of the spears in the same sale basket.
	assert((playerBotSellLootBasket({{15, 1500, false, 0}, {10, 2000, false, 0}},
		100000, 14, 100) == std::vector<uint32_t>{14, 0}));
	assert((playerBotSellLootBasket({{12, 1500, false, 0}, {10, 2000, false, 0}},
		100000, 14, 100) == std::vector<uint32_t>{12, 2}));
	// Independent weight and slot limits apply cumulatively to both item types.
	assert((playerBotSellLootBasket({{3, 10, false, 0}, {10, 20, false, 0}},
		80, 5, 100) == std::vector<uint32_t>{3, 2}));
	assert((playerBotSellLootBasket({{10, 0, false, 0}, {10, 0, false, 0}},
		0, 3, 100) == std::vector<uint32_t>{3, 0}));
	// A stack can merge without a free slot; new stacks reserve slots for
	// later batches. Weight still applies to items merged into an old stack.
	assert((playerBotSellLootBasket({{20, 1, true, 12}, {10, 1, false, 0}},
		100, 0, 100) == std::vector<uint32_t>{12, 0}));
	assert((playerBotSellLootBasket({{20, 1, true, 12}, {10, 1, false, 0}},
		100, 1, 100) == std::vector<uint32_t>{20, 0}));
	assert((playerBotSellLootBasket({{5, 10, true, 5}, {5, 10, true, 0}},
		70, 1, 100) == std::vector<uint32_t>{5, 2}));
	assert((playerBotSellLootBasket({{1000, 0, true, 0}, {1, 0, false, 0}},
		0, 2, 100) == std::vector<uint32_t>{100, 1}));
	std::cout << "playerbotselllootbasket contracts passed\n";
}
