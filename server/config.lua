-- Custom Configs
lotterySystem = false
npcsUsingBankMoney = false
blockLogin = false
blockLoginText = "Server is closed for bug fixing."
showPackets = false
playerbotEnabled = os.getenv("PLAYERBOT_ENABLED") ~= "false"
playerbotHuntDurationSeconds = tonumber(os.getenv("PLAYERBOT_HUNT_DURATION_SECONDS")) or 300
playerbotRelogDelaySeconds = tonumber(os.getenv("PLAYERBOT_RELOG_DELAY_SECONDS")) or 5
playerbotMaxConsecutiveDeaths = tonumber(os.getenv("PLAYERBOT_MAX_CONSECUTIVE_DEATHS")) or 3
playerbotSpeedBonus = tonumber(os.getenv("PLAYERBOT_SPEED_BONUS")) or 300

-- Combat settings
-- NOTE: valid values for worldType are: "pvp", "no-pvp" and "pvp-enforced"
worldType = "pvp"
hotkeyAimbotEnabled = true
protectionLevel = 1
killsToRedSkull = 3
killsToBlackSkull = 6
pzLocked = 60000
removeChargesFromRunes = true
removeChargesFromPotions = true
removeWeaponAmmunition = true
removeWeaponCharges = true
timeToDecreaseFrags = 24 * 60 * 60
whiteSkullTime = 15 * 60
stairJumpExhaustion = 2000
experienceByKillingPlayers = false
expFromPlayersLevelRange = 75
pzLockSkullAttacker = false

-- Connection Config
-- NOTE: maxPlayers set to 0 means no limit
-- NOTE: allowWalkthrough is only applicable to players
ip = "127.0.0.1"
bindOnlyGlobalAddress = false
loginProtocolPort = 7171
gameProtocolPort = 7172
statusProtocolPort = 7171
maxPlayers = 0
motd = "Welcome to TFS!"
onePlayerOnlinePerAccount = false
allowClones = false
allowWalkthrough = true
serverName = "TFS"
statusTimeout = 5000
replaceKickOnLogin = true
maxPacketsPerSecond = 25
passwordType="sha1"

-- Deaths
-- NOTE: Leave deathLosePercent as -1 if you want to use the default
-- death penalty formula. For the old formula, set it to 10. For
-- no skill/experience loss, set it to 0.
deathLosePercent = -1

-- Houses
-- NOTE: set housePriceEachSQM to -1 to disable the ingame buy house functionality
-- NOTE: valid values for houseRentPeriod are: "daily", "weekly", "monthly", "yearly"
-- use any other value to disable the rent system
housePriceEachSQM = 1000
houseRentPeriod = "never"
houseOwnedByAccount = false
houseDoorShowPrice = true
onlyInvitedCanMoveHouseItems = true

-- Item Usage
timeBetweenActions = 200
timeBetweenExActions = 1000

-- Map
-- NOTE: set mapName WITHOUT .otbm at the end
mapName = "World"
mapAuthor = "Giorox"

-- Market
marketOfferDuration = 30 * 24 * 60 * 60
premiumToCreateMarketOffer = true
checkExpiredMarketOffersEachMinutes = 60
maxMarketOffersAtATimePerPlayer = 100

-- MySQL
mysqlHost = "database"
mysqlUser = "angelion"
mysqlPass = "angelion"
mysqlDatabase = "angelion"
mysqlPort = 3306
mysqlSock = ""

-- Misc.
-- NOTE: classicAttackSpeed set to true makes players constantly attack at regular
-- intervals regardless of other actions such as item (potion) use. This setting
-- may cause high CPU usage with many players and potentially affect performance!
-- NOTE: forceMonsterTypesOnLoad loads all monster types on startup to validate them.
-- You can disable it to save some memory if you don't see any errors at startup.
-- checkDuplicateStorageKeys checks the values stored in the variables for duplicates.
allowChangeOutfit = true
freePremium = os.getenv("FREE_PREMIUM") ~= "false"
kickIdlePlayerAfterMinutes = 15
maxMessageBuffer = 4
emoteSpells = false
classicEquipmentSlots = true
classicAttackSpeed = true
showScriptsLogInConsole = true
showOnlineStatusInCharlist = false
yellMinimumLevel = 2
yellAlwaysAllowPremium = false
minimumLevelToSendPrivate = 1
premiumToSendPrivate = false
forceMonsterTypesOnLoad = true
cleanProtectionZones = false
luaItemDesc = false
checkDuplicateStorageKeys = false

-- VIP and Depot limits
-- NOTE: you can set custom limits per group in data/XML/groups.xml
vipFreeLimit = 20
vipPremiumLimit = 100
depotFreeLimit = 2000
depotPremiumLimit = 10000

-- World Light
-- NOTE: if defaultWorldLight is set to true the world light algorithm will
-- be handled in the sources. set it to false to avoid conflicts if you wish
-- to make use of the function setWorldLight(level, color)
defaultWorldLight = true

-- Server Save
-- NOTE: serverSaveNotifyDuration in minutes
serverSaveNotifyMessage = true
serverSaveNotifyDuration = 5
serverSaveCleanMap = false
serverSaveClose = false
serverSaveShutdown = true

-- Experience stages
-- NOTE: to use a flat experience multiplier, set experienceStages to nil
-- minlevel and multiplier are MANDATORY
-- maxlevel is OPTIONAL, but is considered infinite by default
-- to disable stages, create a stage with minlevel 1 and no maxlevel
experienceStages = {
	{ minlevel = 1, multiplier = 1 }
}

-- Rates
-- NOTE: rateExp is not used if you have enabled stages above
rateExp = 1
rateSkill = 1
rateLoot = 1
rateMagic = 1
rateSpawn = 1

-- Monster Despawn Config
-- despawnRange is the amount of floors a monster can be from its spawn position
-- despawnRadius is how many tiles away it can be from its spawn position
-- removeOnDespawn will remove the monster if true or teleport it back to its spawn position if false
-- walkToSpawnRadius is the allowed distance that the monster will stay away from spawn position when left with no targets, 0 to disable
deSpawnRange = 2
deSpawnRadius = 50
removeOnDespawn = true
walkToSpawnRadius = 15

-- Stamina
staminaSystem = true

-- Scripts
warnUnsafeScripts = true
convertUnsafeScripts = true

-- Startup
-- NOTE: defaultPriority only works on Windows and sets process
-- priority, valid values are: "normal", "above-normal", "high"
defaultPriority = "high"
startupDatabaseOptimization = false

-- Status Server Information
ownerName = ""
ownerEmail = ""
url = "https://otland.net/"
location = "Sweden"

-- Server Statistics
-- time in seconds: 30 = 30 seconds, 0 = disabled
statsDumpInterval = 30
-- time in milliseconds: 10 = 0.01 sec, 0 = disabled
statsSlowLogTime = 10
-- time in milliseconds: 50 = 0.05 sec, 0 = disabled
statsVerySlowLogTime = 50
