-- Logs BizHawk lag frames (game did not poll the pad) up to LAG_MAX. Env: LAG_OUT, LAG_MAX.
local out = io.open(os.getenv("LAG_OUT"), "w")
local stop = tonumber(os.getenv("LAG_MAX") or "3000")
client.speedmode(6400)
while emu.framecount() <= stop do
  emu.frameadvance()
  if emu.islagged() then out:write(string.format("lag %d\n", emu.framecount())) end
end
out:write(string.format("lagcount %d\n", emu.lagcount()))
out:close()
client.exit()
