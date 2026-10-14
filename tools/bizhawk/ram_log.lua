-- Per-frame lag + key WRAM log for the whole movie (task E, poll-aligned sync).
-- Env: LAG_OUT (lag_log.lua format: "lag N" per lagged frame, then "lagcount N"),
--      RAM_OUT (one line per frame: "f lag rng hp x y lvl lives pad", hex fields;
--      f = emu.framecount() after the frame ran, same as tlk_headless --ram-log).
-- RAM ($7E/$7F as WRAM offsets): $1E00 RNG seed (3 bytes), $2004 health, $B21B/$B21D Simba X/Y,
-- $1FF9E level, $1FFAA lives, $005A pad word copied from $4218 by $C0A07C.
local lag = io.open(os.getenv("LAG_OUT"), "w")
local ram = io.open(os.getenv("RAM_OUT"), "w")
client.speedmode(6400)
local stop = movie.length() + 120
local function w(a) return memory.read_u16_le(a, "WRAM") end
while emu.framecount() < stop do
  emu.frameadvance()
  local f, l = emu.framecount(), emu.islagged()
  if l then lag:write(string.format("lag %d\n", f)) end
  ram:write(string.format("%d %d %06X %04X %04X %04X %04X %04X %04X\n", f, l and 1 or 0,
    memory.read_u24_le(0x1E00, "WRAM"), w(0x2004), w(0xB21B), w(0xB21D), w(0x1FF9E), w(0x1FFAA), w(0x5A)))
end
lag:write(string.format("lagcount %d\n", emu.lagcount()))
lag:close(); ram:close()
client.exit()
