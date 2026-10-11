-- Echo Arcade touch bridge for Balatro (LOVE 11.5).
-- Loaded at the end of main.lua in EchoArcade's prepared copies only; the Steam
-- install is never modified. Inert unless ArcadeHost set ECHO_ARCADE_PORT.
--
-- ArcadeHost sends "down|move|up U V" (0..1 in the window) and "quit" over UDP
-- on 127.0.0.1. The bridge becomes the mouse: Balatro reads the cursor through
-- love.mouse, and clicks arrive as normal mousepressed/mousereleased events.
local port = tonumber(os.getenv("ECHO_ARCADE_PORT") or "")
if not port then return end

-- Steam: the Steam copy uses the real Steam API (achievements, stats) when Steam is
-- running. Otherwise love.load gets a stub
-- instead of quitting because luasteam could not start.
do
    local real = false
    if os.getenv("ECHO_ARCADE_STEAM") == "1" then
        local ok, st = pcall(require, "luasteam")
        real = ok and type(st) == "table" and st.init ~= nil and st.init() == true
    end
    if not real then
        -- Only explicit fields at the top: Balatro walks every table in G and
        -- probes fields like `is`, so a catch-all __index here would break it.
        local fail = function() return false end
        package.loaded.luasteam = {
            init = function() return true end,
            shutdown = function() end,
            runCallbacks = function() end,
            user = {getSteamID = function() return 0 end},
            userStats = {getAchievement = fail, setAchievement = fail, getStatInt = fail, setStatInt = fail,
                         storeStats = fail, requestCurrentStats = fail, resetAllStats = fail},
        }
    end
end

local socket = require("socket")
local udp = socket.udp()
udp:settimeout(0)
assert(udp:setsockname("127.0.0.1", port))

local x, y, pressed = 0, 0, false

love.mouse.getPosition = function() return x, y end
love.mouse.getX = function() return x end
love.mouse.getY = function() return y end
love.mouse.isDown = function(...)
    for _, b in ipairs({...}) do if b == 1 and pressed then return true end end
    return false
end
love.mouse.setPosition = function(nx, ny) x, y = nx, ny end
love.window.hasFocus = function() return true end
love.window.hasMouseFocus = function() return true end

-- The tablet is the only input; ignore the real desktop mouse entirely. Balatro's
-- love.run re-dispatches mousepressed with fewer arguments, so instead of tagging
-- events we count the ones we queued and let exactly that many through.
local handlers = love.handlers
local queued = {mousepressed = 0, mousereleased = 0, mousemoved = 0}
for name in pairs(queued) do
    local original = handlers[name]
    handlers[name] = function(...)
        if queued[name] > 0 then
            queued[name] = queued[name] - 1
            return original(...)
        end
    end
end
local function push(name, ...)
    queued[name] = queued[name] + 1
    love.event.push(name, ...)
end
handlers.focus = function() if love.focus then love.focus(true) end end

-- Stay windowed without rewriting the player's saved video settings.
if Game and Game.init_window then
    local init_window = Game.init_window
    function Game:init_window(...)
        local saved = self.SETTINGS and self.SETTINGS.WINDOW and self.SETTINGS.WINDOW.screenmode
        if saved then self.SETTINGS.WINDOW.screenmode = "Windowed" end
        local result = init_window(self, ...)
        if saved then self.SETTINGS.WINDOW.screenmode = saved end
        return result
    end
end

local function apply(kind, u, v)
    local w, h = love.graphics.getDimensions()
    local nx, ny = u * w, v * h
    local dx, dy = nx - x, ny - y
    x, y = nx, ny
    push("mousemoved", nx, ny, dx, dy, false)
    if kind == "down" and not pressed then
        pressed = true
        push("mousepressed", nx, ny, 1, false, 1)
    elseif kind == "up" and pressed then
        pressed = false
        push("mousereleased", nx, ny, 1, false, 1)
    end
end

local update = love.update
love.update = function(dt, ...)
    for _ = 1, 64 do
        local message = udp:receive()
        if not message then break end
        if message == "quit" then love.event.quit() end
        local kind, u, v = message:match("^(%a+) ([%d%.]+) ([%d%.]+)$")
        if kind then apply(kind, tonumber(u), tonumber(v)) end
    end
    if update then return update(dt, ...) end
end
