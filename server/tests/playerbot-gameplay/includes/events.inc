local F = PlayerbotGameplayFixture

F.inaccessibleCorpseDeath = CreatureEvent("PlayerbotInaccessibleCorpseDeath")

function F.inaccessibleCorpseDeath.onDeath(creature, corpse)
    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or ""
    if creature:getName() ~= F.lootMonsterName or (mode ~= "corpse_detour" and mode ~= "corpse_inaccessible") then
        return true
    end
    local player = Player(F.botName)
    assert(player and corpse, "inaccessible corpse fixture lost Bot One or the corpse")
    local origin = corpse:getPosition()
    for _, position in ipairs({
        Position(origin.x - 3, origin.y, origin.z),
        Position(origin.x + 3, origin.y, origin.z),
        Position(origin.x, origin.y - 3, origin.z),
        Position(origin.x, origin.y + 3, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() and player:teleportTo(position) then
            local stepX = origin.x == position.x and 0 or origin.x > position.x and 1 or -1
            local stepY = origin.y == position.y and 0 or origin.y > position.y and 1 or -1
            local blockerPosition = Position(position.x + stepX, position.y + stepY, position.z)
            local blocker = Game.createMonster(F.corpseBlockerMonsterName, blockerPosition, true, true)
            assert(blocker and blocker:selectTarget(player),
                "inaccessible corpse fixture could not create its attacking blocker")
			if mode == "corpse_inaccessible" then
				for x = -1, 1 do
					for y = -1, 1 do
						local wallPosition = Position(position.x + x, position.y + y, position.z)
						if (x ~= 0 or y ~= 0) and wallPosition ~= blockerPosition then
							local tile = Tile(wallPosition)
							if tile and tile:isWalkable() then
								assert(Game.createItem(1499, 1, wallPosition),
									"inaccessible corpse fixture could not seal its corridor")
							end
						end
					end
				end
			end
            print("PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED")
            return true
        end
    end
    error("inaccessible corpse fixture could not displace Bot One")
end

F.inaccessibleCorpseDeath:register()

F.cargoCorpseDeath = CreatureEvent("PlayerbotCargoCorpseDeath")

function F.cargoCorpseDeath.onDeath(creature, corpse)
    if os.getenv("PLAYERBOT_GAMEPLAY_MODE") ~= "cargo_partial" or
        creature:getName() ~= F.cargoCoinStackMonsterName then
        return true
    end
    assert(corpse, "partial cargo fixture lost its corpse")
    for _, item in ipairs(corpse:getItems() or {}) do
        assert(item:remove(), "partial cargo fixture could not normalize corpse contents")
    end
    assert(corpse:addItem(ITEM_GOLD_COIN, 10), "partial cargo fixture could not add ten coins")
    return true
end

F.cargoCorpseDeath:register()

F.supplyRecoveryCorpseDeath = CreatureEvent("PlayerbotSupplyRecoveryCorpseDeath")

function F.supplyRecoveryCorpseDeath.onDeath(creature, corpse)
    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE")
    local incomeRecovery = mode == "spear_recovery_income"
    if not incomeRecovery and (mode ~= "supply_recovery" or creature:getName():lower() ~= "spider") then
        return true
    end
    assert(corpse, "supply recovery fixture lost the combat corpse")
    if incomeRecovery then
        -- The corpse already exists here. Disabling loot before death would
        -- suppress the corpse too; now it only skips the later random dropLoot.
        assert(creature:setDropLoot(false), "spear recovery fixture could not disable random loot")
        -- One controlled coin keeps funds below a spear price, and cannot be
        -- confused with a reward grant or a claim about sustainable economics.
        for _, item in ipairs(corpse:getItems() or {}) do
            assert(item:remove(), "spear recovery fixture could not normalize corpse contents")
        end
        assert(corpse:addItem(ITEM_GOLD_COIN, 1), "spear recovery fixture could not ensure combat income")
        F.spearIncomeCorpses = (F.spearIncomeCorpses or 0) + 1
        print(string.format('{"component":"playerbot","event":"spear_income_corpse",' ..
            '"source":"lua_setup_verifier","scenario":"spear_recovery_income","target_id":%d,"gold":1}',
            creature:getId()))
        return true
    end
    for _, item in ipairs(corpse:getItems() or {}) do
        if item:getId() == ITEM_GOLD_COIN then
            return true
        end
    end
    assert(corpse:addItem(ITEM_GOLD_COIN, 1), "supply recovery fixture could not ensure combat income")
    return true
end

F.supplyRecoveryCorpseDeath:register()
