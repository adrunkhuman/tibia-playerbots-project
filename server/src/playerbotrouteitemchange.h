#ifndef FS_PLAYERBOTROUTEITEMCHANGE_H
#define FS_PLAYERBOTROUTEITEMCHANGE_H

#include <cstdint>
#include <tuple>

// Route-relevant state of a tile item. Runtime builds this from ItemType and
// the item's permission attributes. Ordinary decorative IDs are deliberately
// not identity-bearing; only supported passages, ground and interactive items
// can change navigation by identity alone. Coarse connectivity tracks only
// changes that might open a route; local route watches still see every change.
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

inline bool playerBotRouteItemMayOpenConnectivity(const PlayerBotRouteItemSignature& item, bool removing)
{
	// Added floors, transitions, doors and height can introduce new arcs.
	// Removing a blocker or a redirect can uncover an existing walking arc.
	const uint16_t id = item.passageId;
	const bool passage = id == 1386 || id == 3678 || id == 5543 || id == 430 ||
	    id == 384 || id == 418 || id == 8278 || id == 8592 ||
	    id == 468 || id == 469 || (id >= 481 && id <= 484) ||
	    id == 7932 || id == 7933;
	return item.ground || item.door || item.teleport || item.floorChange || item.hasHeight ||
	       passage || (removing && item.blockSolid);
}

inline bool playerBotRouteItemUpdateMayOpenConnectivity(
    PlayerBotRouteItemSignature oldItem, PlayerBotRouteItemSignature newItem)
{
	// A ground-to-ground decay cannot create a floor. Changes in blockers,
	// passages, height or redirects remain relevant to connectivity.
	if (oldItem.ground && newItem.ground) oldItem.ground = newItem.ground = false;
	return playerBotRouteItemMayOpenConnectivity(oldItem, true) ||
	       playerBotRouteItemMayOpenConnectivity(newItem, false);
}

inline bool playerBotRouteItemUpdateAffectsNavigation(
    const PlayerBotRouteItemSignature& oldItem, const PlayerBotRouteItemSignature& newItem)
{
	// A field may change damage/condition even without changing its item ID.
	return oldItem.magicField || newItem.magicField || oldItem.fields() != newItem.fields();
}

#endif
