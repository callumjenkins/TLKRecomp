-- BizHawk ground truth for tlk_headless replay sync.
-- Env: SHOT_MAX (stop after this frame, 0 = movie end + 120), SHOT_DIR (output dir), SHOT_EVERY (default 1000), SHOT_WIN (+-frames around each mark, default 4).
-- Writes <SHOT_DIR>/bh_<framecount>.png (framecount = frames emulated so far, same meaning as
-- tlk_headless --dump-every names) and <SHOT_DIR>/levels.txt (level/WRAM changes).
local dir = os.getenv("SHOT_DIR")
local every = tonumber(os.getenv("SHOT_EVERY") or "1000")
local win = tonumber(os.getenv("SHOT_WIN") or "4")
local stop = tonumber(os.getenv("SHOT_MAX") or "0")
local log = io.open(dir .. "/levels.txt", "w")
client.speedmode(6400)
local len = movie.length()
log:write(string.format("movie=%s length=%d\n", movie.filename(), len))
local last = -1
while emu.framecount() < len + 120 and (stop == 0 or emu.framecount() <= stop) do
  local f = emu.framecount()
  local lvl = memory.read_u16_le(0x1FF9E, "WRAM")
  if lvl ~= last then
    log:write(string.format("frame=%d level=%04X\n", f, lvl)); log:flush(); last = lvl
  end
  local r = f % every
  if f > 0 and (r <= win or r >= every - win) then
    client.screenshot(string.format("%s/bh_%06d.png", dir, f))
  end
  emu.frameadvance()
end
log:write(string.format("end frame=%d mode=%s\n", emu.framecount(), movie.mode()))
log:close()
client.exit()
