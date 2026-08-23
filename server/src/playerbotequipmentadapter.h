/** Engine adapter for immutable equipment-policy observations. */
#ifndef FS_PLAYERBOTEQUIPMENTADAPTER_H
#define FS_PLAYERBOTEQUIPMENTADAPTER_H

#include "playerbotequipmentpolicy.h"

class Item;
class Player;

class PlayerBotEquipmentAdapter
{
	public:
		static PlayerBotEquipmentPlayerSnapshot player(const Player& player);
		static PlayerBotEquipmentItemSnapshot item(const Item& item);
		static PlayerBotEquipmentItemSnapshot item(uint16_t itemId);
		static PlayerBotEquipmentLoadout loadout(const Player& player);
		static bool findCarriedUpgrade(const PlayerBotEquipmentPolicy& policy, Player& player,
		                               Item*& selectedItem, PlayerBotEquipmentUpgrade& selectedUpgrade);
};

#endif
