-- Lua declaration contract for playerbot door passages. Run from repository root:
-- lua server/tests/playerbot_door_passages_contracts.lua
local nativeDofile = dofile

-- global.lua normally loads this dependency before defining the door tables.
-- The declaration contract only needs those tables, not the game runtime.
dofile = function(path)
    if path == "data/lib/lib.lua" then
        return
    end
    return nativeDofile(path)
end

Position = function(x, y, z) return {x = x, y = y, z = z} end
actionIds = {levelDoor = 1000}
table.contains = function(values, wanted)
    for _, value in ipairs(values) do
        if value == wanted then return true end
    end
    return false
end

local passages = {}
local itemActions, actionIdActions, uniqueIdActions = {}, {}, {}
Action = function()
    local action = {ids = {}, aids = {}, uids = {}, passages = {}}
    function action:passage(closed, open, access, levelActionIdOffset)
        assert(not passages[closed], "duplicate passage declaration: " .. closed)
        local passage = {open = open, access = access, levelActionIdOffset = levelActionIdOffset}
        passages[closed] = passage
        self.passages[closed] = passage
        return true
    end
    function action:id(...) for _, id in ipairs({...}) do table.insert(self.ids, id) end return true end
    function action:aid(...) for _, id in ipairs({...}) do table.insert(self.aids, id) end return true end
    function action:uid(...) for _, id in ipairs({...}) do table.insert(self.uids, id) end return true end
    function action:register()
        for _, id in ipairs(self.ids) do itemActions[id] = self end
        for _, id in ipairs(self.aids) do actionIdActions[id] = self end
        for _, id in ipairs(self.uids) do uniqueIdActions[id] = self end
        return true
    end
    return action
end

local function resolvedAction(item)
    return item.uid and uniqueIdActions[item.uid] or actionIdActions[item.aid] or itemActions[item.id]
end

nativeDofile("server/data/global.lua")
nativeDofile("server/data/scripts/actions/others/doors.lua")

local function assertPairs(closed, open, access)
    assert(#closed == #open, "source door tables have different lengths")
    for index, closedId in ipairs(closed) do
        local passage = passages[closedId]
        assert(passage and passage.open == open[index] and passage.access == access,
            string.format("missing %s passage %d -> %d", access, closedId, open[index]))
    end
end

assertPairs(closedDoors, openDoors, "ordinary")
assertPairs(closedExtraDoors, openExtraDoors, "ordinary")
for _, closedId in ipairs(closedLevelDoors) do
    local passage = passages[closedId]
    assert(passage and passage.open == closedId + 1 and passage.access == "level" and
        passage.levelActionIdOffset == actionIds.levelDoor,
        string.format("missing level passage %d -> %d", closedId, closedId + 1))
end
assertPairs(closedHouseDoors, openHouseDoors, "house")

for _, itemId in ipairs(lockedDoors) do assert(not passages[itemId], "locked door is a passage") end
for _, itemId in ipairs(closedQuestDoors) do assert(not passages[itemId], "quest door is a passage") end

-- The resolved action owns its descriptor. An AID or UID override without one
-- must never inherit the item action's ordinary-door declaration.
assert(resolvedAction({id = 1210}).passages[1210], "ordinary item action lost its passage")
local actionOverride = Action()
actionOverride:aid(9000)
actionOverride:register()
local uniqueOverride = Action()
uniqueOverride:uid(9001)
uniqueOverride:register()
assert(not resolvedAction({id = 1210, aid = 9000}).passages[1210], "AID override inherited a passage")
assert(not resolvedAction({id = 1210, aid = 9000, uid = 9001}).passages[1210], "UID override lost precedence")

-- Reloading scripts replaces the action map and recreates descriptors from the
-- current global tables; it must not retain an override or stale descriptor.
local firstDoorAction = resolvedAction({id = 1210})
passages, itemActions, actionIdActions, uniqueIdActions = {}, {}, {}, {}
nativeDofile("server/data/global.lua")
nativeDofile("server/data/scripts/actions/others/doors.lua")
assert(resolvedAction({id = 1210}) ~= firstDoorAction and resolvedAction({id = 1210}).passages[1210],
    "door descriptors were not recreated on reload")
assert(resolvedAction({id = 1210, aid = 9000}).passages[1210],
    "stale AID override survived reload")

-- windows.lua is intentionally action-only: it must not acquire a passage declaration.
nativeDofile("server/data/scripts/actions/others/windows.lua")
for _, itemId in ipairs({6440, 6442}) do assert(not passages[itemId], "window is a passage") end

print("Playerbot door passage declaration contract PASS")
