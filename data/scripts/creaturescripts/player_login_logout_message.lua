local loginMessage = CreatureEvent("loginMessage")



function loginMessage.onLogin(player)
    print(os.date("[%H:%M:%S]") .. " - " .. player:getName() .. " has logged in. IP: " .. Game.convertIpToString(player:getIp()))
    return true
end

loginMessage:register()

local logoutMessage = CreatureEvent("logoutMessage")

function logoutMessage.onLogout(player)
    print(os.date("[%H:%M:%S]") .. " - " .. player:getName() .. " has logged out.")
    return true
end

logoutMessage:register()