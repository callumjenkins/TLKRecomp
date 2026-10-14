-- Logs "PC cycles" for every executed opcode from poll TRACE_FROM to poll TRACE_TO (1-based, a poll
-- = execution of $C0A07C). Env: TRACE_OUT, TRACE_FROM, TRACE_TO. Pairs with tlk_headless TLK_TRACE_OPCODES.
-- The execute-any hook is registered only inside the window (it is slow).
--
-- Clock: emu.totalexecutedcycles() is 0 on BizHawk's SNES cores (task O), so the clock is built from the
-- beam: the core's V/H registers at each opcode (H in dots, 0..340: 4 master clocks each, dots 323 and 327
-- 6; so the clock has 4-clock resolution), accumulated as deltas. A
-- field wrap adds 357366 (the NTSC field average; real fields alternate 357368/357364, so each wrap is
-- within 2 clocks). Set TRACE_RAW=1 to log "PC V H" instead.
local log = io.open(os.getenv("TRACE_OUT") or "bh_trace.txt", "w")
client.speedmode(6400)
local from = tonumber(os.getenv("TRACE_FROM")) or 1482
local to = tonumber(os.getenv("TRACE_TO")) or 1483
local raw = os.getenv("TRACE_RAW") == "1"
local polls, anyid = 0, nil
local getreg = emu.getregister
local clock, last = 0, nil
local function now()
    local h = getreg("H")
    local p = getreg("V") * 1364 + h * 4 + (h > 323 and 2 or 0) + (h > 327 and 2 or 0)
    if last then
        local d = p - last
        if d < 0 then d = d + 357366 end
        clock = clock + d
    end
    last = p
    return clock
end
local function arm()
    anyid = event.onmemoryexecuteany(function(addr)
        if raw then
            log:write(string.format("%06X %d %d\n", addr, getreg("V"), getreg("H")))
        else
            log:write(string.format("%06X %d\n", addr, now()))
        end
    end)
end
if from == 0 then arm() end
event.onmemoryexecute(function()
    polls = polls + 1
    if polls == from and not anyid then
        arm()
    end
    if polls >= to then log:write(string.format("C0A07C %d\n", now())); log:close(); client.exit() end
end, 0xC0A07C)
while true do emu.frameadvance() end
