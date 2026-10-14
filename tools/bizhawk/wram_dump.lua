-- Dump all 128 KiB WRAM at chosen frames. Env: DUMP_FRAMES="f1,f2,..." (emu.framecount() after the
-- frame ran), DUMP_DIR. Writes bh_wram_<f>.bin, then exits after the last one.
local dir = os.getenv("DUMP_DIR")
local want, last = {}, 0
for f in string.gmatch(os.getenv("DUMP_FRAMES"), "%d+") do want[tonumber(f)] = true; if tonumber(f) > last then last = tonumber(f) end end
client.speedmode(6400)
while emu.framecount() < last do
  emu.frameadvance()
  local f = emu.framecount()
  if want[f] then
    local t = memory.read_bytes_as_array(0, 0x20000, "WRAM")
    local out = io.open(string.format("%s/bh_wram_%d.bin", dir, f), "wb")
    local CH = 4096
    for i = 1, #t, CH do out:write(string.char(table.unpack(t, i, math.min(i + CH - 1, #t)))) end
    out:close()
  end
end
client.exit()
