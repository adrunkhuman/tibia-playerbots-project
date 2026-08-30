local F = PlayerbotGameplayFixture

F.login = CreatureEvent("zzPlayerbotGameplayRegression")

function F.login.onLogin(player)
    if player:getName() ~= F.botName then
        return true
    end

	local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "cycle"
	F.selectHealthPotion(player)
    assert(mode == "mainland" or mode == "carlin_local_service" or mode == "cycle" or mode == "depot" or mode == "depot_risk_fallback" or mode == "slotted_loot_seller" or mode == "slotted_loot_no_seller" or mode == "sell_loot" or mode == "sell_loot_remote_depot" or mode == "navigation" or mode == "navigation_recovery" or mode == "carlin_service_route" or mode == "mutable_portal_route" or mode == "patrol_recovery" or mode == "target_approach" or mode == "target_approach_unreachable" or mode == "target_attacker_priority" or mode == "hunt_area_arrival" or mode == "corpse" or mode == "corpse_detour" or mode == "corpse_inaccessible" or mode == "death" or mode == "healing" or mode == "spell_use" or
        mode == "healing_resupply" or mode == "value" or mode == "progression" or mode == "progression_bundle" or
        mode == "progression_nested" or mode == "progression_resume" or mode == "progression_nested_resume" or
        mode == "progression_space" or mode == "arbitration" or mode == "arbitration_interrupt" or mode == "departure" or
        mode == "departure_interrupt" or mode == "departure_recovery" or mode == "spell_training" or
        mode == "spell_training_shortlist" or mode == "stamina_bonus" or
        mode == "stamina_boundary" or mode == "stamina_normal" or mode == "hunt_planning" or mode == "spell_calibration" or
        mode == "readiness_ready" or mode == "adaptive_challenge" or mode == "readiness_upgrade" or
        mode == "readiness_missing_weapon" or mode == "readiness_supplies" or mode == "readiness_no_food" or
        mode == "readiness_low_wealth" or mode == "readiness_food_capacity" or mode == "readiness_retention" or
        mode == "equipment_shadow" or mode == "equipment_shadow_unaffordable" or mode == "equipment_shadow_no_upgrade" or
        mode == "equipment_buy" or mode == "equipment_buy_resume" or mode == "equipment_buy_space" or
        mode == "equipment_buy_provider_moved" or
        mode == "equipment_buy_provider_unreachable" or
        mode == "equipment_buy_rejected" or mode == "mainland_reward" or mode == "magic_training_haste" or
        mode == "magic_training_great_light" or mode == "magic_training_light" or mode == "magic_training_refresh" or
        mode == "magic_training_reserve" or mode == "magic_training_failed" or mode == "magic_training_service" or
        mode == "magic_training_progression" or mode == "magic_training_hunt" or mode == "magic_training_pz" or
        mode == "magic_training_exact_full" or mode == "magic_training_restart" or mode == "magic_training_absent" or
        mode == "magic_training_expired" or mode == "magic_training_post_hunt" or mode == "magic_training_post_hunt_no_overflow",
        "unknown PLAYERBOT_GAMEPLAY_MODE: " .. mode)
    if mode == "equipment_shadow" or mode == "equipment_shadow_unaffordable" or mode == "equipment_shadow_no_upgrade" or
        mode == "equipment_buy" or mode == "equipment_buy_resume" or mode == "equipment_buy_space" or
        mode == "equipment_buy_provider_moved" or mode == "equipment_buy_provider_unreachable" or
        mode == "equipment_buy_rejected" then
        if mode ~= "equipment_buy_provider_moved" then
            local thais = Town(F.thaisTownId)
            assert(thais and player:setTown(thais) and player:teleportTo(thais:getTemplePosition()),
                "equipment fixture could not select Thais")
        end
        assert(player:getLevel() == 8 and player:getVocation():getId() == 4,
            "equipment shadow fixture did not load the level-8 mainland Knight")
        local currentLeft = player:getSlotItem(CONST_SLOT_LEFT)
        local currentRight = player:getSlotItem(CONST_SLOT_RIGHT)
        if mode == "equipment_buy" and ((currentLeft and currentLeft:getId() == F.equipmentPurchaseItemId) or
            (currentRight and currentRight:getId() == F.equipmentPurchaseItemId)) then
            assert(player:getItemCount(F.starterWeaponId) == 1 and player:getMoney() + player:getBankBalance() == 100,
                "equipment purchase restart did not preserve equipment and economy")
            assert(player:setStorageValue(F.fixtureReadyStorage, 2), "equipment restart fixture could not signal completion")
            print("PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS")
            return true
        end
        for _, slot in ipairs({CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET}) do
            local item = player:getSlotItem(slot)
            if item then assert(item:remove(), "equipment fixture could not clear slot " .. slot) end
        end
        for _, itemId in ipairs({2457, 2463, 2525, 2376, 2647, 2643, F.starterArmorId, F.starterWeaponId,
            F.equipmentPurchaseItemId, 2468, F.movedProviderPurchaseItemId}) do
            F.removeAll(player, itemId)
        end
        for _, equipment in ipairs({
            {2457, CONST_SLOT_HEAD}, {2463, CONST_SLOT_ARMOR}, {2525, CONST_SLOT_RIGHT},
            {mode == "equipment_buy_provider_moved" and 2468 or 2647, CONST_SLOT_LEGS}, {2643, CONST_SLOT_FEET}
        }) do
            assert(player:addItem(equipment[1], 1, false, 1, equipment[2]),
                "equipment fixture could not restore slot " .. equipment[2])
        end
        local fixtureWeaponId = mode == "equipment_buy_provider_moved" and 2376 or F.starterWeaponId
        assert(player:addItem(fixtureWeaponId, 1, false, 1, CONST_SLOT_LEFT),
            "equipment fixture could not equip the starter weapon")
        F.removeAll(player, F.potionItemId)
        assert(player:addItem(F.potionItemId, 10), "equipment fixture could not normalize healing supplies")
        if mode == "equipment_buy_provider_moved" then
            for _, spellName in ipairs({"Find Person", "Light"}) do
                if not player:hasLearnedSpell(spellName) then
                    assert(player:learnSpell(spellName), "moving-provider fixture could not suppress spell training")
                end
            end
        end
        if mode == "equipment_buy_space" then
            for _, slot in ipairs({CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_RIGHT, CONST_SLOT_LEGS, CONST_SLOT_FEET}) do
                local item = player:getSlotItem(slot)
                if item then assert(item:remove(), "equipment storage fixture could not clear slot " .. slot) end
            end
            assert(player:addItem(F.starterArmorId, 1, false, 1, CONST_SLOT_ARMOR),
                "equipment storage fixture could not equip the starter armor")
        elseif mode == "equipment_shadow_no_upgrade" then
            local left = player:getSlotItem(CONST_SLOT_LEFT)
            local right = player:getSlotItem(CONST_SLOT_RIGHT)
            assert(left and left:remove() and right and right:remove(),
                "equipment shadow fixture could not clear the one-handed loadout")
            assert(player:addItem(2377, 1, false, 1, CONST_SLOT_LEFT),
                "equipment shadow fixture could not equip the two-handed sword")
        end
        assert(player:setBankBalance(100), "equipment fixture could not normalize the bank balance")
        if mode == "equipment_shadow" or mode == "equipment_shadow_no_upgrade" then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(2152, 20), "equipment shadow fixture could not add surplus money")
        elseif mode == "equipment_shadow_unaffordable" then
            F.removeAll(player, ITEM_GOLD_COIN)
        elseif mode == "equipment_buy_resume" then
            F.removeAll(player, F.equipmentPurchaseItemId)
            F.removeAll(player, ITEM_GOLD_COIN)
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(player:getBankBalance() == 100 and backpack and backpack:addItem(F.equipmentPurchaseItemId, 1),
                "equipment resume fixture could not set persisted purchase state")
        else
            F.removeAll(player, F.equipmentPurchaseItemId)
            F.removeAll(player, ITEM_GOLD_COIN)
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            local surplus = mode == "equipment_buy_provider_moved" and 80 or 5
            assert(player:getBankBalance() == 100 and backpack and backpack:addItem(ITEM_GOLD_COIN, surplus),
                "equipment purchase fixture could not set the bounded surplus")
            if mode == "equipment_buy_space" then
                player:setCapacity(100000)
                while backpack:getSize() < backpack:getCapacity() - 1 do
                    assert(backpack:addItem(ITEM_BAG, 1), "equipment storage fixture could not fill the backpack")
                end
            end
        end
        F.suppressNearbyMonsters(player:getId())
        local left = player:getSlotItem(CONST_SLOT_LEFT)
        local right = player:getSlotItem(CONST_SLOT_RIGHT)
        local armor = player:getSlotItem(CONST_SLOT_ARMOR)
        assert(left and armor, "equipment shadow fixture lost required equipment")
        if mode == "equipment_buy_space" then
            addEvent(F.verifyEquipmentPurchaseSpace, 500, player:getId())
        elseif mode == "equipment_buy_provider_moved" then
            addEvent(F.verifyMovedProviderPurchase, 500, player:getId(), 360)
        elseif mode == "equipment_buy" or mode == "equipment_buy_resume" or
            mode == "equipment_buy_rejected" then
            addEvent(F.verifyEquipmentPurchase, 500, player:getId(), mode == "equipment_buy_rejected", mode == "equipment_buy_resume", 360)
        elseif mode ~= "equipment_buy_provider_unreachable" then
            addEvent(F.verifyEquipmentShadow, 250, player:getId(), mode, player:getMoney(), left:getId(),
                right and right:getId() or 0, armor:getId(), player:getPosition())
        end
        assert(player:setStorageValue(F.fixtureReadyStorage, 1), "equipment fixture could not signal readiness")
        if mode == "equipment_buy_provider_moved" then
            local start = player:getPosition()
            addEvent(F.moveEquipmentProvider, 100, player:getId(), start.x, start.y, start.z, 0)
        end
        print("PLAYERBOT_GAMEPLAY_TEST " .. string.upper(mode) .. "_START")
        return true
    end
    if mode == "carlin_local_service" then
        local carlin = Town(F.carlinTownId)
        assert(carlin and player:setTown(carlin) and player:setVocation(4),
            "Carlin service fixture could not select the mainland Knight")
        F.selectHealthPotion(player)
		assert(player:teleportTo(F.carlinTemplePosition), "Carlin service fixture could not reach the temple")
		local eva
		for _, creature in ipairs(Game.getSpectators(F.evaPosition, true, false, 3, 3, 3, 3)) do
			local npc = Npc(creature)
			if npc and npc:getName() == "Eva" then
				eva = npc
				break
			end
		end
		assert(eva and not eva:isRemoved(), "Carlin banker Eva was not spawned")
		F.removeAll(player, F.smallHealthPotionItemId)
        F.removeAll(player, F.healthPotionItemId)
        assert(player:addItem(F.healthPotionItemId, 1), "Carlin service fixture could not seed the return threshold")
		assert(player:removeMoney(player:getMoney()) and player:setBankBalance(505),
			"Carlin service fixture could not fund the restock target")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyCarlinLocalService, 250, player:getId(), 480)
        print("PLAYERBOT_GAMEPLAY_TEST CARLIN_LOCAL_SERVICE_START")
        return true
    end
    if mode == "mainland" then
        local town = player:getTown()
		if not town or town:getId() ~= F.thaisTownId then
			local thais = Town(F.thaisTownId)
			assert(thais and player:setTown(thais) and player:teleportTo(thais:getTemplePosition()),
				"mainland fixture could not select Thais")
			town = player:getTown()
		end
        assert(player:getLevel() == 8 and player:getVocation():getId() == 4 and town and town:getId() == F.thaisTownId,
            "mainland fixture did not load the level-8 Thais Knight")
        assert(player:getSlotItem(CONST_SLOT_ARMOR) and player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.seedArmorId,
            "mainland fixture did not load the seeded armor")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        local capacity = player:getCapacity()
        player:setCapacity(100000)
        local lootBag = backpack and backpack:addItem(ITEM_BAG, 1)
        assert(lootBag and lootBag:addItem(2696, 1), "mainland fixture could not seed local-service loot")
        assert(lootBag:addItem(2826, 1), "mainland fixture could not seed remote-buyer loot")
        player:setCapacity(capacity)
        print("PLAYERBOT_GAMEPLAY_TEST MAINLAND_START")
        return true
    end
    if mode == "mainland_reward" then
        local town = player:getTown()
		if not town or town:getId() ~= F.thaisTownId then
			local thais = Town(F.thaisTownId)
			assert(thais and player:setTown(thais) and player:teleportTo(thais:getTemplePosition()),
				"mainland reward fixture could not select Thais")
			town = player:getTown()
		end
        assert(player:getLevel() == 8 and player:getVocation():getId() == 4 and town and town:getId() == F.thaisTownId,
            "mainland reward fixture did not load the level-8 Thais Knight")
        if player:getStorageValue(F.mainlandRewardStorage) == 1 then
            local armor = player:getSlotItem(CONST_SLOT_ARMOR)
            assert(armor and armor:getId() == F.mainlandRewardItemId and player:getItemCount(F.starterArmorId) == 1 and
                player:getItemCount(1987) >= 1, "mainland reward restart did not preserve the complete equipped bundle")
            assert(player:setStorageValue(F.fixtureReadyStorage, 1), "mainland reward restart fixture could not signal readiness")
            print("PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_RESTART_PASS")
            return true
        end
        assert(player:getStorageValue(F.mainlandRewardStorage) == -1,
            "mainland reward fixture expected an unclaimed scale armor reward")
        local armor = player:getSlotItem(CONST_SLOT_ARMOR)
        if armor then assert(armor:remove(), "mainland reward fixture could not clear the armor slot") end
        F.removeAll(player, F.starterArmorId)
        assert(player:addItem(F.starterArmorId, 1, false, 1, CONST_SLOT_ARMOR), "mainland reward fixture could not equip the jacket")
        assert(player:setCapacity(100000), "mainland reward fixture could not increase capacity")
        assert(player:teleportTo(Position(32356, 32130, 9)), "mainland reward fixture could not approach the scale armor reward")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyMainlandReward, 500, player:getId(), 360)
        assert(player:setStorageValue(F.fixtureReadyStorage, 1), "mainland reward fixture could not signal readiness")
        print("PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_START")
        return true
    end
    if mode == "spell_use" or mode == "spell_calibration" then
        assert(player:setVocation(4), "spell-use fixture could not select Knight")
        F.removeAll(player, F.smallHealthPotionItemId)
        F.removeAll(player, F.healthPotionItemId)
        assert(player:addItem(F.healthPotionItemId, 5), "spell-use fixture could not add health potions")
        F.spellCalibrationFixture = mode == "spell_calibration"
        local requiredLevel = F.spellCalibrationFixture and 35 or 20
        local requiredExperience = Game.getExperienceForLevel(requiredLevel) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == requiredLevel, "spell-use fixture could not select the required level")
        assert(player:teleportTo(F.depotPosition), "spell-use fixture could not reach the depot")
        local spells = F.spellCalibrationFixture and {"Light Healing", "Haste", "Whirlwind Throw", "Berserk"} or
            {"Light Healing", "Haste", "Whirlwind Throw"}
        for _, spellName in ipairs(spells) do
            assert(player:learnSpell(spellName), "spell-use fixture could not prelearn " .. spellName)
        end
        assert(player:setMaxHealth(200), "spell-use fixture could not normalize health")
        assert(player:setHealth(110), "spell-use fixture could not lower health")
        assert(player:setMaxMana(200), "spell-use fixture could not normalize mana")
        assert(player:addMana(player:getMaxMana() - player:getMana()), "spell-use fixture could not restore mana")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.triggerSpellFallback, 4000, player:getId())
        addEvent(F.restoreLightHealing, 6000, player:getId())
        addEvent(F.waitForSpellSupport, 6000, player:getId(), 120)
        addEvent(F.verifySpellUse, 500, player:getId(), 120)
        print("PLAYERBOT_GAMEPLAY_TEST " .. string.upper(mode) .. "_START")
        return true
    end
    if mode:sub(1, 14) == "magic_training" then
        local restarting = mode == "magic_training_restart" and player:getStorageValue(50098) == 1
        if restarting then
            F.suppressNearbyMonsters(player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST MAGIC_TRAINING_RESTART_START")
            return true
        end
        local thais = Town(F.thaisTownId)
        assert(thais and player:setTown(thais) and player:setVocation(4), "magic training fixture could not select Thais Knight")
        local requiredExperience = Game.getExperienceForLevel(20) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 20, "magic training fixture could not select the required level")
        for _, spellName in ipairs({"Haste", "Great Light", "Light"}) do player:forgetSpell(spellName) end
        local spells = (mode == "magic_training_haste" or mode == "magic_training_refresh" or mode == "magic_training_hunt" or
            mode == "magic_training_post_hunt") and {"Haste", "Great Light", "Light"} or
            mode == "magic_training_great_light" and {"Great Light"} or {"Light"}
        for _, spellName in ipairs(spells) do
            assert(player:learnSpell(spellName), "magic training fixture could not learn " .. spellName)
        end
        local inProtectionZone = mode == "magic_training_pz"
        assert(player:teleportTo(inProtectionZone and thais:getTemplePosition() or Position(32084, 32144, 5)),
            "magic training fixture could not choose its safety context")
        assert(player:setMaxMana(1000) and player:addMana(player:getMaxMana() - player:getMana()), "magic training fixture could not restore mana")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        F.removeAll(player, ITEM_GOLD_COIN)
        assert(backpack and backpack:addItem(ITEM_GOLD_COIN, 100), "magic training fixture could not normalize cash reserve")
        player:removeCondition(CONDITION_REGENERATION)
        if mode ~= "magic_training_absent" then
            local regeneration = Condition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
            regeneration:setParameter(CONDITION_PARAM_MANAGAIN, mode == "magic_training_reserve" and 1000 or 10)
            regeneration:setParameter(CONDITION_PARAM_MANATICKS,
                (mode == "magic_training_post_hunt" or mode == "magic_training_post_hunt_no_overflow") and 10000 or 1000)
            regeneration:setParameter(CONDITION_PARAM_TICKS, mode == "magic_training_expired" and 0 or 60000)
            assert(player:addCondition(regeneration), "magic training fixture could not add regeneration")
        end
        if mode == "magic_training_reserve" then assert(player:addMana(39 - player:getMana()), "magic training fixture could not lower mana") end
        if mode == "magic_training_exact_full" then assert(player:addMana(990 - player:getMana()), "magic training fixture could not set exact-full forecast") end
        if mode == "magic_training_post_hunt_no_overflow" then assert(player:addMana(990 - player:getMana()), "magic training post-hunt fixture could not set non-overflow mana") end
        if mode == "magic_training_refresh" then
            local haste = Condition(CONDITION_HASTE)
            haste:setParameter(CONDITION_PARAM_TICKS, 60000)
            assert(player:addCondition(haste), "magic training fixture could not apply Haste")
            local light = Condition(CONDITION_LIGHT)
            light:setParameter(CONDITION_PARAM_LIGHT_LEVEL, 6)
            light:setParameter(CONDITION_PARAM_LIGHT_COLOR, 215)
            light:setParameter(CONDITION_PARAM_TICKS, 60000)
            assert(player:addCondition(light), "magic training fixture could not apply Light condition")
        end
        if mode == "magic_training_service" then
            assert(player:setCapacity(1), "magic training service fixture could not require a depot return")
        elseif mode == "magic_training_progression" then
            player:forgetSpell("Find Person")
            F.removeAll(player, F.potionItemId)
            F.removeAll(player, F.meatItemId)
            assert(player:addItem(F.potionItemId, 5) and player:addItem(F.meatItemId, 1), "magic training progression fixture could not prepare service reserves")
            assert(player:removeMoney(player:getMoney()) and player:addMoney(100) and player:setBankBalance(305), "magic training progression fixture could not normalize spell training funds")
            assert(player:getMoney() == 100 and player:getBankBalance() == 305, "magic training progression fixture has the wrong service and spell-training reserves")
            local trainer = Npc("Gregor")
            assert(trainer, "magic training progression fixture could not find Gregor")
            local trainerPosition = trainer:getPosition()
            assert(player:teleportTo(Position(trainerPosition.x - 1, trainerPosition.y, trainerPosition.z)),
                "magic training progression fixture could not approach Gregor")
        end
        if mode == "magic_training_post_hunt" or mode == "magic_training_post_hunt_no_overflow" then
            if not player:hasLearnedSpell("Find Person") then assert(player:learnSpell("Find Person"), "magic training post-hunt fixture could not remove spell-learning work") end
        end
        if mode == "magic_training_restart" then assert(player:setStorageValue(50098, 1), "magic training restart fixture could not persist its marker") end
        F.suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST " .. string.upper(mode) .. "_START")
        return true
    end
	F.restoreRookgaardBaseline(player, mode ~= "cycle")
	F.selectHealthPotion(player)
    if mode == "readiness_ready" or mode == "readiness_upgrade" or mode == "readiness_missing_weapon" or mode == "readiness_supplies" or mode == "readiness_no_food" or mode == "readiness_low_wealth" or mode == "readiness_food_capacity" or mode == "readiness_retention" then
        assert(player:setVocation(4), "readiness fixture could not select Knight vocation")
		F.selectHealthPotion(player)
        assert(player:teleportTo(F.depotPosition), "readiness fixture could not reach the depot")
        F.removeAll(player, F.potionItemId)
        assert(player:addItem(F.potionItemId, 10), "readiness fixture could not normalize healing supplies")
        F.suppressNearbyMonsters(player:getId())
        if mode == "readiness_upgrade" then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(F.pickupRewardId, 1), "readiness upgrade fixture could not add carried weapon")
            addEvent(F.verifyReadiness, 500, player:getId(), "upgrade", 120)
        elseif mode == "readiness_missing_weapon" then
            F.removeAll(player, F.starterWeaponId)
            F.removeAll(player, F.pickupRewardId)
            print("PLAYERBOT_GAMEPLAY_TEST READINESS_MISSING_WEAPON_START")
		elseif mode == "readiness_supplies" then
			local thais = Town(F.thaisTownId)
			assert(thais and player:setTown(thais) and player:teleportTo(Position(32354, 32226, 7)),
				"readiness supplies fixture could not reach the Thais depot")
            F.removeAll(player, F.potionItemId)
            F.removeAll(player, F.meatItemId)
            assert(player:addItem(F.potionItemId, 1), "readiness supplies fixture could not seed the return threshold")
            assert(player:removeMoney(player:getMoney()) and player:addMoney(100) and player:setBankBalance(405),
                "readiness supplies fixture could not fund the restock target")
            addEvent(F.verifyReadiness, 500, player:getId(), "supplies", 240)
        elseif mode == "readiness_no_food" then
            F.removeAll(player, F.meatItemId)
            local money = player:getMoney()
            if money > 0 then assert(player:removeMoney(money), "no-food fixture could not clear carried money") end
            assert(player:addMoney(100), "no-food fixture could not normalize carried money")
        elseif mode == "readiness_low_wealth" then
            local money = player:getMoney()
            if money > 0 then assert(player:removeMoney(money), "low-wealth fixture could not clear carried money") end
            assert(player:setBankBalance(50), "low-wealth fixture could not set the bank balance")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(F.pickupRewardId, 1), "low-wealth fixture could not add a carried upgrade")
            assert(backpack:addItem(F.saleItemId, 1) and backpack:addItem(F.saleItemId, 1) and backpack:addItem(F.saleItemId, 1), "low-wealth fixture could not add saleable capacity cargo")
            local usedCapacity = player:getCapacity() - player:getFreeCapacity()
            assert(player:setCapacity(usedCapacity), "low-wealth fixture could not exhaust physical capacity")
            addEvent(F.verifyLowWealth, 500, player:getId(), 240)
        elseif mode == "readiness_food_capacity" then
            F.removeAll(player, F.meatItemId)
            F.removeAll(player, 2696)
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            for _ = 1, 8 do assert(backpack and backpack:addItem(2696, 1), "food-capacity fixture could not add eight cheeses") end
            local usedCapacity = player:getCapacity() - player:getFreeCapacity()
            assert(player:setCapacity(usedCapacity), "food-capacity fixture could not exhaust physical capacity")
            addEvent(F.verifyReadiness, 500, player:getId(), "food_capacity", 120)
        elseif mode == "readiness_retention" then
            F.removeAll(player, F.meatItemId)
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(2050, 1), "retention fixture could not add unknown item")
            assert(backpack:addItem(F.saleItemId, 1), "retention fixture could not add sale item")
            addEvent(F.verifyReadiness, 500, player:getId(), "retention", 240)
        else
            F.removeAll(player, F.potionItemId)
            assert(player:addItem(F.potionItemId, 2), "readiness fixture could not seed the hunt-ready potion count")
            addEvent(F.verifyReadiness, 500, player:getId(), "ready", 120)
        end
        print("PLAYERBOT_GAMEPLAY_TEST READINESS_" .. string.upper(mode:gsub("readiness_", "")) .. "_START")
        return true
    end
    if mode == "hunt_planning" or mode == "hunt_area_arrival" then
        local carlin = Town(F.carlinTownId)
        assert(carlin and player:setTown(carlin) and player:setVocation(4),
            "hunt planning fixture could not select the Carlin Knight")
        F.selectHealthPotion(player)
		F.removeAllHealthPotions(player)
		assert(player:addItem(F.healthPotionItemId, 10),
			"hunt planning fixture could not prepare healing supplies")
        local requiredExperience = Game.getExperienceForLevel(8) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:setMaxHealth(185) and player:setHealth(185),
            "hunt planning fixture could not normalize Knight health")
        assert(player:getLevel() == 8 and player:teleportTo(F.carlinTemplePosition),
            "hunt planning fixture could not prepare the level-8 Carlin Knight")
        if mode == "hunt_planning" then F.suppressNearbyMonsters(player:getId()) end
        print(mode == "hunt_area_arrival" and "PLAYERBOT_GAMEPLAY_TEST HUNT_AREA_ARRIVAL_START" or
            "PLAYERBOT_GAMEPLAY_TEST HUNT_PLANNING_START")
        return true
    end
    if mode == "adaptive_challenge" then
        assert(player:setVocation(4), "adaptive challenge fixture could not select Knight")
		F.selectHealthPotion(player)
		F.removeAllHealthPotions(player)
		assert(player:addItem(F.healthPotionItemId, 10), "adaptive challenge fixture could not prepare healing supplies")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack and backpack:addItem(ITEM_GOLD_COIN, 100), "adaptive challenge fixture could not seed carried currency")
        local requiredExperience = Game.getExperienceForLevel(9) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 9 and player:learnSpell("Light Healing"), "adaptive challenge fixture could not prepare learned Light Healing")
        assert(player:setMaxMana(200) and player:addMana(player:getMaxMana() - player:getMana()), "adaptive challenge fixture could not prepare recovery mana")
        F.suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST ADAPTIVE_CHALLENGE_START")
        return true
    end
    if mode == "depot_risk_fallback" then
        assert(player:setVocation(4), "depot risk fallback fixture could not select Knight")
        local requiredExperience = Game.getExperienceForLevel(15) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 15 and player:setMaxHealth(290) and player:setHealth(290),
            "depot risk fallback fixture could not normalize the live bot")
        for _, slot in ipairs({CONST_SLOT_ARMOR, CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS}) do
            local item = player:getSlotItem(slot)
            if item then assert(item:remove(), "depot risk fallback fixture could not clear equipment slot " .. slot) end
        end
        assert(player:addItem(2464, 1, false, 1, CONST_SLOT_ARMOR) and
            player:addItem(2395, 1, false, 1, CONST_SLOT_LEFT) and
            player:addItem(2509, 1, false, 1, CONST_SLOT_RIGHT) and
            player:addItem(2648, 1, false, 1, CONST_SLOT_LEGS),
            "depot risk fallback fixture could not equip the live loadout")
        assert(player:teleportTo(Position(32947, 32171, 8)),
            "depot risk fallback fixture could not reach the swamp troll cave")
        addEvent(F.verifyDepotRiskFallback, 500, player:getId(), 1440)
        print("PLAYERBOT_GAMEPLAY_TEST DEPOT_RISK_FALLBACK_START")
        return true
    end
    if mode == "depot" then
        player:removeCondition(CONDITION_REGENERATION)
        local food = Condition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        food:setParameter(CONDITION_PARAM_TICKS, 1200000)
        assert(player:addCondition(food), "depot fixture could not fill the food timer")
        local fixtureState = player:getStorageValue(F.depotFixtureStorage)
        if fixtureState == -1 then
            for _, slot in ipairs({CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET}) do
                local item = player:getSlotItem(slot)
                if item then assert(item:remove(), "depot fixture could not clear equipment slot " .. slot) end
            end
            assert(player:addItem(F.starterArmorId, 1, false, 1, CONST_SLOT_ARMOR),
                "depot fixture could not equip starter armor")
            assert(player:addItem(F.starterWeaponId, 1, false, 1, CONST_SLOT_LEFT),
                "depot fixture could not equip starter weapon")
            if os.getenv("PLAYERBOT_DEPOT_MOVE_CASE") == "rejected" then
                F.removeAll(player, F.emptyPotionFlaskItemId)
                assert(player:addItem(F.emptyPotionFlaskItemId, 2, false, 1, CONST_SLOT_AMMO),
                    "depot fixture could not seed ammunition-slot cargo")
            end
            local fixtureBackpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(fixtureBackpack and fixtureBackpack:addItem(F.potionItemId, 10), "depot fixture could not add potion reserve")
            local thais = Town(F.thaisTownId)
            assert(thais and player:setTown(thais), "depot fixture could not select Thais")
            assert(player:teleportTo(Position(32345, 32225, 7)), "depot fixture could not reach Naji")
            local chest = player:getDepotChest(F.PlayerbotThaisLockerDepotId(), true)
            assert(chest and chest:addItem(ITEM_GOLD_COIN, 1), "depot fixture could not seed prior depot contents")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            local outer = backpack and backpack:addItem(ITEM_BAG, 1)
            local nested = outer and outer:addItem(ITEM_BAG, 1)
            assert(nested and nested:addItem(F.depotLootItemId, 1) and nested:addItem(F.depotLootItemId, 1) and nested:addItem(F.depotLootItemId, 1) and nested:addItem(F.depotLootItemId, 1), "depot fixture could not seed nested loot")
            assert(nested:addItem(2380, 1) and nested:addItem(F.seedWeaponId, 1) and nested:addItem(2461, 1) and nested:addItem(2643, 1), "depot fixture could not seed surplus equipment")
            assert(backpack:addItem(2050, 1), "depot fixture could not seed an unknown retained item")
            assert(backpack:addItem(2120, 1) and backpack:addItem(2120, 1) and
                backpack:addItem(2554, 1) and backpack:addItem(2554, 1),
                "depot fixture could not seed surplus navigation tools")
            local restartPhase = os.getenv("PLAYERBOT_DEPOT_RESTART_PHASE") or ""
            assert(player:setStorageValue(F.depotFixtureStorage, restartPhase == "" and 0 or 2), "depot fixture state could not persist")
        elseif fixtureState == 0 then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(F.depotLootItemId, 1), "depot fixture could not seed second-cycle loot")
            assert(player:setStorageValue(F.depotFixtureStorage, 2), "depot second-cycle state could not persist")
        end
        addEvent(F.verifyDepot, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST DEPOT_START")
        return true
    end
    if mode == "slotted_loot_seller" or mode == "slotted_loot_no_seller" then
        F.setupSlottedLoot(player, mode == "slotted_loot_seller")
        print("PLAYERBOT_GAMEPLAY_TEST " .. string.upper(mode) .. "_START")
        return true
    end
    if mode == "sell_loot" then
        local thais = Town(F.thaisTownId)
        assert(thais and player:setTown(thais) and player:setVocation(4), "sell loot fixture could not select Thais")
        assert(player:teleportTo(Position(32345, 32225, 7)), "sell loot fixture could not reach the Thais depot")
        local chest = player:getDepotChest(F.PlayerbotThaisLockerDepotId(), true)
        assert(chest, "sell loot fixture could not open the Thais depot chest")
        assert(chest:addItem(F.sellLootItemId, 10), "sell loot fixture could not seed the first local sale")
        assert(chest:addItem(7635, 9), "sell loot fixture could not seed the second local sale")
        assert(chest:addItem(2050, 1), "sell loot fixture could not seed an unrelated retained item")
		F.removeAll(player, F.smallHealthPotionItemId)
		F.removeAll(player, F.healthPotionItemId)
		F.removeAll(player, F.meatItemId)
		assert(player:addItem(F.meatItemId, 2), "sell loot fixture could not prepare food")
        assert(player:setStorageValue(50100, player:getMoney() + 1), "sell loot fixture could not require service before selling")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifySellLoot, 500, player:getId(), 480)
        print("PLAYERBOT_GAMEPLAY_TEST SELL_LOOT_START")
        return true
    end
    if mode == "sell_loot_remote_depot" then
        local thais = Town(F.thaisTownId)
        assert(thais and player:setTown(thais) and player:setVocation(4), "remote sell loot fixture could not select Thais")
        assert(player:teleportTo(Position(32345, 32225, 7)), "remote sell loot fixture could not reach the Thais depot")
        assert(player:getDepotChest(F.PlayerbotThaisLockerDepotId(), true), "remote sell loot fixture could not open the empty Thais depot")
        F.removeAll(player, F.smallHealthPotionItemId)
        F.removeAll(player, F.healthPotionItemId)
        F.removeAll(player, F.meatItemId)
        assert(player:addItem(F.meatItemId, 2), "remote sell loot fixture could not prepare food")
        assert(player:setStorageValue(50100, player:getMoney() + 1), "remote sell loot fixture could not require service")
        addEvent(F.fundRemoteSellLoot, 500, player:getId())
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyRemoteSellLoot, 500, player:getId(), 1200)
        print("PLAYERBOT_GAMEPLAY_TEST SELL_LOOT_REMOTE_DEPOT_START")
        return true
    end
    if mode == "stamina_bonus" or mode == "stamina_boundary" or mode == "stamina_normal" then
        local staminaMinutes = mode == "stamina_bonus" and 2520 or mode == "stamina_boundary" and 2401 or 2400
        assert(player:setStamina(staminaMinutes), "stamina projection fixture could not set stamina")
        F.suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST STAMINA_PROJECTION_START " .. staminaMinutes)
        return true
    end
    if mode == "death" then
        F.deathLoginCount = F.deathLoginCount + 1
        F.removeNearbyMonsters(player)
        local position = player:getPosition()
        assert(position.x == 32097 and position.y == 32219 and position.z == 7, "Bot One did not log in at the Rookgaard temple")
        if F.deathLoginCount <= 2 then
            addEvent(F.prepareDeath, 100, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_START " .. F.deathLoginCount)
        else
            addEvent(F.prepareDeath, 1500, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_RECOVERY_STATE_PASS")
        end
        return true
    end
    if mode == "departure" then
        if player:getVocation():getId() ~= 0 then
            local town = player:getTown()
            assert(player:getVocation():getId() == 4 and town and town:getId() == 2, "Oracle departure restart restored the wrong vocation or town")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_RESTART_PASS")
            return true
        end
        local requiredExperience = Game.getExperienceForLevel(8) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 8, "Oracle departure fixture could not advance Bot One to level 8")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyOracleDeparture, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_START")
        return true
    end
    if mode == "departure_interrupt" then F.prepareLevelEightCombat(player); return true end
    if mode == "departure_recovery" then
        if player:getStorageValue(F.departureRecoveryStorage) ~= 1 then
            local targetExperience = Game.getExperienceForLevel(9) - 1
            assert(player:getExperience() < targetExperience, "Oracle recovery fixture expected a low-level player")
            player:addExperience(targetExperience - player:getExperience())
            assert(player:getLevel() == 8, "Oracle recovery fixture could not prepare level 8")
            assert(player:setStorageValue(F.departureRecoveryStorage, 1), "Oracle recovery fixture could not persist its restart marker")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_PREPARED")
            return true
        end
        assert(player:getVocation():getId() == 0 and player:getLevel() == 8, "Oracle recovery fixture did not restore a vocationless level-8 player")
        assert(player:getExperience() == Game.getExperienceForLevel(9) - 1, "Oracle recovery fixture did not restore the level-9 experience boundary")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)), "Oracle recovery fixture could not lower Bot One's health")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyOracleDeparture, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_START")
        return true
    end
    if mode == "arbitration" or mode == "arbitration_interrupt" then
        local position = player:getPosition()
        assert(position.x == 32097 and position.y == 32219 and position.z == 7, "goal arbitration fixture did not start at the Rookgaard temple")
        assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.starterArmorId, "goal arbitration fixture expected starter armor")
        assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == F.starterWeaponId, "goal arbitration fixture expected starter weapon")
        F.suppressNearbyMonsters(player:getId())
        if mode == "arbitration_interrupt" then
            F.removeAll(player, F.potionItemId)
            addEvent(F.triggerArbitrationInterrupt, 500, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_INTERRUPT_START")
        else
            print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_START")
        end
        return true
    end
    if mode == "progression_bundle" then
        assert(player:addItem(2152, 5), "economic progression fixture could not seed an existing platinum stack")
        assert(player:teleportTo(Position(32034, 32277, 8)), "economic progression fixture could not approach the reward")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyEconomicPickupProgression, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_BUNDLE_START")
        return true
    end
    if mode == "spell_training" then
        if player:getStorageValue(F.spellTrainingStorage) == 1 then
            assert(player:hasLearnedSpell("Light Healing") and not player:hasLearnedSpell("Find Person"),
                "spell training priority did not persist across restart")
            assert(player:setStorageValue(F.fixtureReadyStorage, 1), "spell training restart fixture could not signal readiness")
            print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_RESTART_PASS")
            return true
        end
        local thais = Town(F.thaisTownId)
        assert(thais and player:setTown(thais), "spell training fixture could not select Thais")
        assert(player:setVocation(4), "spell training fixture could not select Knight")
		F.selectHealthPotion(player)
        local requiredExperience = Game.getExperienceForLevel(9) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 9, "spell training fixture could not select level 9")
		local trainer = Npc("Gregor")
		assert(trainer, "spell training fixture could not find Gregor")
		local trainerPosition = trainer:getPosition()
		assert(player:teleportTo(Position(trainerPosition.x - 1, trainerPosition.y, trainerPosition.z)),
			"spell training fixture could not approach Gregor")
        for _, spellName in ipairs({"Find Person", "Light", "Light Healing", "Cure Poison", "Great Light"}) do player:forgetSpell(spellName) end
        F.removeAll(player, F.potionItemId)
        F.removeAll(player, F.meatItemId)
        assert(player:addItem(F.potionItemId, 10), "spell training fixture could not supply potions")
        assert(player:addItem(F.meatItemId, 2), "spell training fixture could not supply food")
        player:removeCondition(CONDITION_REGENERATION)
        local food = Condition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        food:setParameter(CONDITION_PARAM_TICKS, 1200000)
        assert(player:addCondition(food), "spell training fixture could not fill the food timer")
        local totalMoney = player:getMoney() + player:getBankBalance()
        if totalMoney < 300 then assert(player:addMoney(300 - totalMoney), "spell training fixture could not fund Bot One") end
        if totalMoney > 300 then assert(player:removeTotalMoney(totalMoney - 300), "spell training fixture could not normalize funding") end
        assert(player:getMoney() + player:getBankBalance() == 300, "spell training fixture has the wrong starting money")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifySpellTraining, 500, player:getId(), 360)
        assert(player:setStorageValue(F.fixtureReadyStorage, 1), "spell training fixture could not signal readiness")
        print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_START")
        return true
    end
    if mode == "spell_training_shortlist" then
        local carlin = Town(F.carlinTownId)
        assert(carlin and player:setTown(carlin) and player:setVocation(4),
            "spell trainer shortlist fixture could not select the Carlin Knight")
        local requiredExperience = Game.getExperienceForLevel(9) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() >= 9 and player:forgetSpell("Light Healing"),
            "spell trainer shortlist fixture could not prepare Light Healing")
        F.removeAllHealthPotions(player)
        assert(player:addItem(F.healthPotionItemId, 10),
            "spell trainer shortlist fixture could not supply health potions")
        local totalMoney = player:getMoney() + player:getBankBalance()
        if totalMoney > 0 then assert(player:removeTotalMoney(totalMoney),
            "spell trainer shortlist fixture could not clear funding") end
        assert(player:addMoney(100) and player:getMoney() + player:getBankBalance() == 100,
            "spell trainer shortlist fixture could not set unaffordable funding")
        assert(Npc("Trisha") and player:teleportTo(Position(32679, 31683, 7)),
            "spell trainer shortlist fixture could not reach the Carlin mage guild")
        assert(player:setStorageValue(F.fixtureReadyStorage, 1),
            "spell trainer shortlist fixture could not signal readiness")
        print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_SHORTLIST_START")
        return true
    end
    if mode == "progression_nested" then
        assert(player:setStorageValue(50082, 1), "nested progression fixture could not suppress the nearby currency reward")
        local rewardChest = Container(F.nestedRewardStorage)
        local rewardBag = nil
        for _, item in ipairs(rewardChest and rewardChest:getItems() or {}) do
            if item:getId() == F.nestedRewardRootId then rewardBag = item; break end
        end
        assert(rewardBag and rewardBag:addItem(F.meatItemId, 2), "nested progression fixture could not add mutable food siblings")
        assert(player:teleportTo(Position(32034, 32275, 7)), "nested progression fixture could not approach the reward")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.verifyNestedPickupProgression, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_START")
        return true
    end
    if mode == "progression" or mode == "progression_resume" or mode == "progression_nested_resume" or mode == "progression_space" then
        local position = player:getPosition()
        if mode == "progression_resume" then
            assert(player:getStorageValue(F.pickupRewardStorage) == -1, "resume fixture expected an unclaimed pickup reward")
            assert(player:setStorageValue(F.pickupRewardStorage, 1), "resume fixture could not set claimed storage")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(F.pickupRewardId, 1), "resume fixture could not add claimed reward")
        elseif mode == "progression_nested_resume" then
            assert(player:getStorageValue(F.nestedRewardStorage) == -1, "nested resume fixture expected an unclaimed reward")
            assert(player:setStorageValue(F.nestedRewardStorage, 1), "nested resume fixture could not set claimed storage")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            local rewardBag = backpack and backpack:addItem(F.nestedRewardRootId, 1)
            assert(rewardBag, "nested resume fixture could not add the reward bag")
            assert(rewardBag:addItem(2175, 1), "nested resume fixture could not add the spellbook")
            assert(rewardBag:addItem(2380, 1), "nested resume fixture could not add the hand axe")
            assert(rewardBag:addItem(F.nestedRewardShieldId, 1), "nested resume fixture could not add the wooden shield")
        elseif mode == "progression_space" then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack, "inventory-space fixture expected a backpack")
            while backpack:getSize() < backpack:getCapacity() do assert(backpack:addItem(ITEM_BAG, 1), "inventory-space fixture could not fill the backpack") end
            print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_SPACE_START")
            return true
        elseif player:getStorageValue(F.pickupRewardStorage) == -1 then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(F.potionItemId, 10) and backpack:addItem(ITEM_GOLD_COIN, 100),
                "pickup progression fixture could not add service reserves")
            assert(position.x == 32097 and position.y == 32219 and position.z == 7, "pickup progression fixture did not start at the Rookgaard temple")
            assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.starterArmorId, "pickup progression fixture expected starter armor")
            assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == F.starterWeaponId, "pickup progression fixture expected starter weapon")
            assert(not player:getSlotItem(CONST_SLOT_RIGHT), "pickup progression fixture expected an empty shield slot")
            assert(not player:getSlotItem(CONST_SLOT_FEET), "pickup progression fixture expected an empty feet slot")
        else
            assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == F.pickupRewardId, "pickup progression fixture did not restore persisted equipment")
        end
        F.suppressNearbyMonsters(player:getId())
        local progressionRestart = mode == "progression" and player:getStorageValue(F.pickupRewardStorage) == 1
        if not progressionRestart then
            addEvent(mode == "progression_nested_resume" and F.verifyNestedPickupProgression or F.verifyPickupProgression, 500, player:getId(), 360)
        end
        local startMarker = mode == "progression_resume" and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_RESUME_START" or
            mode == "progression_nested_resume" and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_RESUME_START" or
            progressionRestart and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_RESTART_START" or "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_START"
        print(startMarker)
        return true
    end
    assert(player:teleportTo(F.depotPosition), "fake depot position could not be restored")
    if mode == "target_approach" or mode == "target_approach_unreachable" then
        F.suppressNearbyMonsters(player:getId())
        local position = player:getPosition()
        local monster = Game.createMonster(F.emptyMonsterName, Position(position.x + 4, position.y, position.z), true, true)
        assert(monster, "target approach fixture could not create a visible monster")
		if mode == "target_approach_unreachable" then
			local targetPosition = monster:getPosition()
			for x = -1, 1 do
				for y = -1, 1 do
					if x ~= 0 or y ~= 0 then
						local wallPosition = Position(targetPosition.x + x, targetPosition.y + y, targetPosition.z)
						local tile = Tile(wallPosition)
						if tile and tile:isWalkable() then
							assert(Game.createItem(1499, 1, wallPosition), "target approach fixture could not seal target")
						end
					end
				end
			end
		else
			local competitor = Game.createMonster(F.defensiveMonsterName, Position(position.x + 7, position.y, position.z), true, true)
			assert(competitor and competitor:selectTarget(player), "target approach fixture could not create its farther attacker")
			addEvent(F.removeTargetSelectionCompetitor, 1500, competitor:getId())
			print("PLAYERBOT_GAMEPLAY_TEST TARGET_APPROACH_START " .. monster:getId() .. " " .. competitor:getId())
			return true
		end
        print("PLAYERBOT_GAMEPLAY_TEST TARGET_APPROACH_START " .. monster:getId())
        return true
    end
    if mode == "navigation" then F.createTemporaryBlockers(); F.suppressNearbyMonsters(player:getId()); print("PLAYERBOT_GAMEPLAY_TEST NAVIGATION_START"); return true end
    if mode == "navigation_recovery" then F.suppressNearbyMonsters(player:getId()); print("PLAYERBOT_GAMEPLAY_TEST NAVIGATION_RECOVERY_START"); return true end
    if mode == "carlin_service_route" then
        assert(player:teleportTo(F.carlinServiceRouteStart), "Carlin service route fixture could not reach its start")
        F.suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST CARLIN_SERVICE_ROUTE_START")
        return true
    end
    if mode == "target_attacker_priority" then
        F.suppressNearbyMonsters(player:getId())
        local position = player:getPosition()
		local monster = Game.createMonster(F.emptyMonsterName, Position(position.x + 7, position.y, position.z), true, true)
        assert(monster, "target priority fixture could not create its initial target")
        addEvent(F.spawnTargetPriorityAttacker, 1100, player:getId(), monster:getId())
        print("PLAYERBOT_GAMEPLAY_TEST TARGET_PRIORITY_START " .. monster:getId())
        return true
    end
    if mode == "mutable_portal_route" then
        assert(player:teleportTo(F.mutablePortalRouteStart), "Mutable portal route fixture could not reach its start")
        if player:getItemCount(2554) == 0 then
            assert(player:addItem(2554, 1), "Mutable portal route fixture could not add a shovel")
        end
        F.suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST MUTABLE_PORTAL_ROUTE_START")
        return true
    end
    if mode == "patrol_recovery" then F.suppressNearbyMonsters(player:getId()); print("PLAYERBOT_GAMEPLAY_TEST PATROL_RECOVERY_START"); return true end
    if mode == "healing" then
        F.suppressNearbyMonsters(player:getId())
        local potions = player:getItemCount(F.potionItemId)
        if potions > 0 then assert(player:removeItem(F.potionItemId, potions), "healing fixture could not remove starter potions") end
        assert(player:getItemCount(F.potionItemId) == 0, "healing fixture expected no seeded potions")
        assert(player:addItem(F.potionItemId, F.healingPotionCount), "healing fixture could not add recovery potions")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)), "healing fixture could not lower Bot One's health")
        addEvent(F.verifyHealing, 250, player:getId(), 40)
        print("PLAYERBOT_GAMEPLAY_TEST HEALING_START")
        return true
    end
    if mode == "healing_resupply" then
        F.suppressNearbyMonsters(player:getId())
        local potions = player:getItemCount(F.potionItemId)
        if potions > 0 then assert(player:removeItem(F.potionItemId, potions), "healing resupply fixture could not remove starter potions") end
        F.removeAll(player, F.meatItemId)
        assert(player:getItemCount(F.potionItemId) == 0, "healing resupply fixture expected no seeded potions")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack and backpack:addItem(7636, 1), "healing resupply fixture could not add an empty potion flask")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)), "healing resupply fixture could not lower Bot One's health")
        addEvent(F.verifyHealingResupply, 250, player:getId(), 240)
        print("PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_START")
        return true
    end
    if mode == "corpse" then
        assert(not ItemType(ITEM_GOLD_COIN):isCorpse() and not ItemType(ITEM_GOLD_COIN):isContainer(), "non-lootable corpse fixture must not be an openable corpse")
        assert(not ItemType(ITEM_BAG):isCorpse() and ItemType(ITEM_BAG):isContainer(), "container death-item fixture must be a non-corpse container")
        F.suppressNearbyMonsters(player:getId())
        addEvent(F.spawnCorpseMonster, 500, player:getId(), F.nonlootableMonsterName)
        addEvent(F.spawnCorpseMonster, 5000, player:getId(), F.emptyMonsterName)
        addEvent(F.spawnCorpseMonster, 12500, player:getId(), F.lootMonsterName)
        addEvent(F.spawnCorpseMonster, 20000, player:getId(), F.containerDeathItemMonsterName)
        print("PLAYERBOT_GAMEPLAY_TEST CORPSE_START")
        return true
    end
    if mode == "corpse_detour" or mode == "corpse_inaccessible" then
        F.suppressNearbyMonsters(player:getId())
        F.PlayerbotSpawnInaccessibleCorpseMonster(player:getId())
        return true
    end
    if mode == "value" then
        player:removeCondition(CONDITION_REGENERATION)
        local food = Condition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        food:setParameter(CONDITION_PARAM_TICKS, 1200000)
        assert(player:addCondition(food), "value replacement fixture could not fill the food timer")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        F.removeAll(player, F.meatItemId)
        F.removeAll(player, 2671)
        F.removeAll(player, 2398)
        local cargoBag = backpack and backpack:addItem(ITEM_BAG, 1)
        assert(cargoBag and cargoBag:addItem(2671, 2) and cargoBag:addItem(2398, 1),
            "nested value replacement cargo could not be added")
		local incomingWeight = ItemType(ITEM_PLATINUM_COIN):getWeight()
        local usedCapacity = player:getCapacity() - player:getFreeCapacity()
        assert(incomingWeight > 1, "value replacement fixture has invalid incoming weight")
        player:setCapacity(usedCapacity + incomingWeight - 1)
        F.suppressNearbyMonsters(player:getId())
        F.spawnCorpseMonster(player:getId(), F.valueMonsterName)
        print("PLAYERBOT_GAMEPLAY_TEST VALUE_START")
        return true
    end
    assert(player:getMoney() == 0, "Bot One initial backpack purse was not empty")
    assert(player:getBankBalance() == 1000, "Bot One initial bank balance was not 1000 gp")
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
    local potions = player:getItemCount(F.potionItemId)
    if potions > 0 then assert(player:removeItem(F.potionItemId, potions), "cycle fixture could not remove starter potions") end
    local meat = player:getItemCount(F.meatItemId)
    if meat > 0 then assert(player:removeItem(F.meatItemId, meat), "cycle fixture could not remove starter meat") end
	assert(player:addItem(F.potionItemId, 10), "cycle fixture could not seed completed potion reserves")
	local money = player:getMoney()
	if money > 0 then assert(player:removeMoney(money), "cycle fixture could not clear carried money") end
	assert(player:addMoney(100) and player:setBankBalance(1000), "cycle fixture could not seed completed cash reserves")
    assert(backpack and backpack:getId() == ITEM_BACKPACK, "seeded backpack is missing")
    local lootBag = backpack:addItem(ITEM_BAG, 1)
    assert(lootBag, "test loot bag could not be added")
	assert(lootBag:addItem(F.saleItemId, 1), "depot loot could not be added")
    addEvent(F.spawnDefensiveMonsters, 100, player:getId())
    local depotTile = Tile(F.depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local initialDepotBagCount = depotTile:getItemCountById(ITEM_BAG)
	local initialDepotSaleItemCount = depotTile:getItemCountById(F.saleItemId)
	addEvent(F.verifyService, 500, player:getId(), initialDepotBagCount, initialDepotSaleItemCount, 500)
    print("PLAYERBOT_GAMEPLAY_TEST START")
    return true
end

F.login:register()
