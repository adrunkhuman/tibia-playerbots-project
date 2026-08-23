/** Engine extraction and object resolution for equipment decisions. */
#include "otpch.h"

#include "playerbotequipmentadapter.h"

#include "game.h"
#include "container.h"
#include "item.h"
#include "player.h"

namespace {
	PlayerBotEquipmentWeaponType weaponType(WeaponType_t type)
	{
		switch (type) {
			case WEAPON_SWORD: return PlayerBotEquipmentWeaponType::Sword;
			case WEAPON_CLUB: return PlayerBotEquipmentWeaponType::Club;
			case WEAPON_AXE: return PlayerBotEquipmentWeaponType::Axe;
			case WEAPON_DISTANCE: return PlayerBotEquipmentWeaponType::Distance;
			case WEAPON_AMMO: return PlayerBotEquipmentWeaponType::Ammo;
			case WEAPON_SHIELD: return PlayerBotEquipmentWeaponType::Shield;
			case WEAPON_NONE: return PlayerBotEquipmentWeaponType::None;
			default: return PlayerBotEquipmentWeaponType::Other;
		}
	}

	PlayerBotEquipmentItemSnapshot itemSnapshot(uint16_t itemId, bool removed)
	{
		const ItemType& type = Item::items[itemId];
		PlayerBotEquipmentItemSnapshot result;
		result.itemId = itemId;
		result.pickupable = type.isPickupable();
		result.removed = removed;
		result.minimumLevel = type.minReqLevel;
		result.minimumMagicLevel = type.minReqMagicLevel;
		result.premiumRequired = (type.wieldInfo & WIELDINFO_PREMIUM) != 0;
		result.vocationIds.assign(type.vocationIds.begin(), type.vocationIds.end());
		result.head = (type.slotPosition & SLOTP_HEAD) != 0;
		result.armorSlot = (type.slotPosition & SLOTP_ARMOR) != 0;
		result.legs = (type.slotPosition & SLOTP_LEGS) != 0;
		result.feet = (type.slotPosition & SLOTP_FEET) != 0;
		result.left = (type.slotPosition & SLOTP_LEFT) != 0;
		result.right = (type.slotPosition & SLOTP_RIGHT) != 0;
		result.twoHanded = (type.slotPosition & SLOTP_TWO_HAND) != 0;
		result.weaponType = weaponType(type.weaponType);
		result.armor = type.armor;
		result.defense = type.defense;
		result.extraDefense = type.extraDefense;
		result.attack = type.attack;
		result.weight = type.weight;
		return result;
	}
}

PlayerBotEquipmentPlayerSnapshot PlayerBotEquipmentAdapter::player(const Player& player)
{
	return {player.getLevel(), player.getMagicLevel(), player.getVocationId(), player.isPremium(),
	        player.getMaxHealth(), player.getSkillLevel(SKILL_FIST), player.getSkillLevel(SKILL_SWORD),
	        player.getSkillLevel(SKILL_CLUB), player.getSkillLevel(SKILL_AXE), player.getSkillLevel(SKILL_DISTANCE),
	        player.getSkillLevel(SKILL_SHIELD), player.getAttackFactor(), player.getDefenseFactor(),
	        player.getVocation()->armorMultiplier, player.getVocation()->defenseMultiplier};
}

PlayerBotEquipmentItemSnapshot PlayerBotEquipmentAdapter::item(const Item& item)
{
	PlayerBotEquipmentItemSnapshot result = itemSnapshot(item.getID(), item.isRemoved());
	result.armor = item.getArmor();
	result.defense = item.getDefense();
	result.attack = item.getAttack();
	result.count = item.getItemCount();
	result.container = item.getContainer() != nullptr;
	return result;
}

PlayerBotEquipmentItemSnapshot PlayerBotEquipmentAdapter::item(uint16_t itemId)
{
	return itemSnapshot(itemId, false);
}

PlayerBotEquipmentLoadout PlayerBotEquipmentAdapter::loadout(const Player& player)
{
	PlayerBotEquipmentLoadout loadout;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			loadout.itemIds[slot] = item->getID();
			loadout.items[slot] = PlayerBotEquipmentAdapter::item(*item);
			loadout.items[slot].equipped = true;
		}
	}
	return loadout;
}

bool PlayerBotEquipmentAdapter::findCarriedUpgrade(const PlayerBotEquipmentPolicy& policy, Player& player,
	Item*& selectedItem, PlayerBotEquipmentUpgrade& selectedUpgrade)
{
	std::vector<PlayerBotEquipmentCarriedCandidate> candidates;
	std::vector<Item*> items;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* root = player.getInventoryItem(static_cast<slots_t>(slot));
		Container* container = root ? root->getContainer() : nullptr;
		if (!container) continue;
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			Item* candidate = *it;
			Position source;
			uint8_t index = 0;
			g_game.internalGetPosition(candidate, source, index);
			PlayerBotEquipmentItemSnapshot facts = PlayerBotEquipmentAdapter::item(*candidate);
			facts.inContainer = true;
			candidates.push_back({std::move(facts), source.x == 0xFFFF && (source.y & 0x40) != 0});
			items.push_back(candidate);
		}
	}
	const auto selected = policy.findCarriedUpgrade(PlayerBotEquipmentAdapter::player(player), loadout(player), candidates);
	if (!selected) {
		selectedItem = nullptr;
		return false;
	}
	selectedItem = items[selected->index];
	selectedUpgrade = selected->upgrade;
	return true;
}
