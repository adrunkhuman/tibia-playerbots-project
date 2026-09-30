#ifndef FS_PLAYERBOTROUTEITEMCHANGE_H
#define FS_PLAYERBOTROUTEITEMCHANGE_H

#include <cstdint>
#include <tuple>

// Route-relevant state of a tile item. Runtime builds this from ItemType and
// the item's permission attributes. Ordinary decorative IDs are deliberately
// not identity-bearing; only supported passages, ground and interactive items
// can change navigation by identity alone.
struct PlayerBotRouteItemSignature {
	uint16_t passageId = 0;
	uint16_t groundSpeed = 0;
	uint16_t actionId = 0;
	uint16_t uniqueId = 0;
	uint32_t floorChange = 0;
	uint32_t combatType = 0;
	bool ground = false, door = false, magicField = false, teleport = false;
	bool blockSolid = false, blockPath = false, blockProjectile = false;
	bool hasHeight = false, moveable = false, pickupable = false;
	bool ignoreBlocking = false, lookThrough = false, useable = false;
	bool forceUse = false, alwaysOnTop = false, blockPickupable = false;
	bool vertical = false, horizontal = false, hangable = false;

	auto fields() const {
		return std::tie(passageId, groundSpeed, actionId, uniqueId, floorChange, combatType,
		    ground, door, magicField, teleport, blockSolid, blockPath, blockProjectile,
		    hasHeight, moveable, pickupable, ignoreBlocking, lookThrough, useable,
		    forceUse, alwaysOnTop, blockPickupable, vertical, horizontal, hangable);
	}
};

// Adding or removing an item changes routes only for these kinds: damaging
// fields, blockers (movable ones too, since bots cannot push items), and items
// with passage identity (ground, doors, teleports, floor changes, usable and
// supported passages). Corpses, splashes, loot, and other walkable items never
// restart route searches or invalidate cached routes.
inline bool playerBotRouteItemPresenceAffectsNavigation(const PlayerBotRouteItemSignature& item)
{
	return item.magicField || item.blockSolid || item.blockPath || item.passageId != 0;
}

inline bool playerBotRouteItemUpdateAffectsNavigation(
    const PlayerBotRouteItemSignature& oldItem, const PlayerBotRouteItemSignature& newItem)
{
	// A field may change damage/condition even without changing its item ID.
	return oldItem.magicField || newItem.magicField || oldItem.fields() != newItem.fields();
}

#endif
