-- Determinism checksum recorder (UNSYNCED gadget, headless-compatible).
--
-- Echoes the engine's whole-game sync checksum (the value used for
-- multiplayer desync detection) every frame from 1..LAST, then forces a
-- quit. Runs as an unsynced LuaRules gadget so it works in spring-headless
-- (which has no LuaUI widgets). It reads sim state but never writes it, so
-- it cannot perturb determinism. Two runs of the same seeded game on
-- different architectures must produce identical CHKREC streams for the
-- build to be multiplayer-compatible.

function gadget:GetInfo()
    return {
        name    = "Determinism Checksum Recorder",
        desc    = "Echoes per-frame sync checksum for cross-platform determinism testing",
        author  = "macos-port",
        date    = "2026",
        license = "GPLv2+",
        layer   = 1000,
        enabled = true,
    }
end

if gadgetHandler:IsSyncedCode() then
    return
end

local LAST = 300

function gadget:GameFrame(frame)
    if frame >= 1 and frame <= LAST then
        local c = Spring.GetPrevFrameSyncChecksum and Spring.GetPrevFrameSyncChecksum() or "NOAPI"
        Spring.Echo("CHKREC " .. frame .. " " .. tostring(c))
    elseif frame == LAST + 5 then
        Spring.Echo("CHKREC done")
        Spring.SendCommands("quitforce")
    end
end
