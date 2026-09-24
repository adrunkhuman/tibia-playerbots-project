-- Runtime-only Action precedence fixture. The C++ driver creates the items and
-- verifies the real Actions maps after a global reload.
if os.getenv("PLAYERBOT_GAMEPLAY_MODE") == "door_passages" then
    local actionOverride = Action()
    function actionOverride.onUse() return true end
    assert(actionOverride:passage(1210, 1211, "ordinary"), "fixture AID passage declaration failed")
    actionOverride:aid(65000)
    assert(actionOverride:register(), "fixture AID action registration failed")

    local uniqueOverride = Action()
    function uniqueOverride.onUse() return true end
    uniqueOverride:uid(65001)
    assert(uniqueOverride:register(), "fixture UID action registration failed")
end
