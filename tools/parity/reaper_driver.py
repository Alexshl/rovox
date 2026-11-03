#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Renders a job in the running REAPER through its file bridge (~/Library/Application Support/REAPER/Scripts/mcp_bridge_data):
#   request_<id>.json  {"func": "<ReaScript API function>", "args": [...]}   (id 1..990)
#   response_<id>.json {"ok": true, "ret": ...}
# The bridge cannot pass MediaTrack pointers, so the work is done by a generated Lua script that is registered as an action
# (AddRemoveReaScript), run (Main_OnCommand) and unregistered again. The script works in a NEW project tab and closes only that tab;
# the user's projects are never saved or modified (a project that is opened for `project` mode is saved under a copy name into the work
# directory first, so closing the tab does not ask anything).
#
# The Lua writes <name>.reaper.json: the FX settings in the format of tools/reaper/rovox_export_fx.lua (what Rovox's import.rpp reads),
# plus how each requested value was found and what REAPER reported back. It renders <name>.wav (stereo, 48 kHz, 32 bit float).
import argparse
import json
import os
import random
import subprocess
import sys
import time

BRIDGE = os.path.expanduser("~/Library/Application Support/REAPER/Scripts/mcp_bridge_data")


def lua_literal(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r") + '"'
    if isinstance(v, (list, tuple)):
        return "{" + ",".join(lua_literal(x) for x in v) + "}"
    if isinstance(v, dict):
        return "{" + ",".join("[%s]=%s" % (lua_literal(str(k)), lua_literal(x)) for k, x in v.items() if x is not None) + "}"
    raise TypeError(type(v))


LUA = r'''
local J = @JOB@
local reaper = reaper
local ARRAY = {}
local function arr(t) return setmetatable(t or {}, ARRAY) end
local function enc(v, out)
  local k = type(v)
  if k == "table" then
    if getmetatable(v) == ARRAY or (#v > 0) then
      out[#out+1] = "["
      for i = 1, #v do if i > 1 then out[#out+1] = "," end enc(v[i], out) end
      out[#out+1] = "]"
    else
      out[#out+1] = "{"
      local first = true
      for key, val in pairs(v) do
        if not first then out[#out+1] = "," end
        first = false
        enc(tostring(key), out) out[#out+1] = ":" enc(val, out)
      end
      out[#out+1] = "}"
    end
  elseif k == "string" then
    out[#out+1] = '"' .. v:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"'
  elseif k == "number" then
    if v ~= v or v == math.huge or v == -math.huge then out[#out+1] = "null"
    else out[#out+1] = string.format("%.9g", v) end
  elseif k == "boolean" then out[#out+1] = v and "true" or "false"
  else out[#out+1] = "null" end
end
local function json(v) local out = {} enc(v, out) return table.concat(out) end

local LOG = { steps = arr(), settings = arr(), errors = arr() }
local function step(s) LOG.steps[#LOG.steps+1] = s end
local function writefile(path, s) local f = io.open(path, "wb") if f then f:write(s) f:close() end end

local function fmtnum(s)
  if not s then return nil end
  if s:lower():find("^%s*%-%s*inf") then return -math.huge end
  if s:lower():find("^%s*%+?%s*inf") then return math.huge end
  return tonumber(s:match("^%s*([%-%+]?%d*%.?%d+)"))
end

local function fxtext(tr, fx, p, n)
  reaper.TrackFX_SetParamNormalized(tr, fx, p, n)
  local _, s = reaper.TrackFX_GetFormattedParamValue(tr, fx, p, "")
  return s
end

-- Find the normalized value whose displayed text is `target` (number: bisection, assuming monotone; string: scan).
local function solve(tr, fx, p, target)
  if type(target) == "table" then return target.n, "normalized" end
  local num = type(target) == "number" and target or fmtnum(tostring(target))
  if num ~= nil and type(target) == "number" or (num ~= nil and tostring(target):match("^%s*[%-%+]?[%d%.]+%s*$")) then
    local v0, v1 = fmtnum(fxtext(tr, fx, p, 0.0)), fmtnum(fxtext(tr, fx, p, 1.0))
    if v0 == nil or v1 == nil then return nil, "no numeric text" end
    local lo, hi = 0.0, 1.0
    local inc = v1 >= v0
    for _ = 1, 60 do
      local mid = (lo + hi) / 2
      local v = fmtnum(fxtext(tr, fx, p, mid))
      if v == nil then break end
      if (v < num) == inc then lo = mid else hi = mid end
    end
    local best, bestd = lo, math.huge
    for _, c in ipairs({ lo, hi, (lo + hi) / 2 }) do
      local v = fmtnum(fxtext(tr, fx, p, c))
      if v ~= nil then
        local d = math.abs(v - num)
        if v == num then d = 0 end
        if d < bestd then best, bestd = c, d end
      end
    end
    return best, "formatted"
  end
  local want = tostring(target):lower()
  local N = 1024
  for i = 0, N do
    local t = fxtext(tr, fx, p, i / N)
    if t and t:lower() == want then
      local j = i
      while j < N and (fxtext(tr, fx, p, (j + 1) / N) or ""):lower() == want do j = j + 1 end
      return ((i + j) / 2) / N, "text"
    end
  end
  for i = 0, N do
    local t = fxtext(tr, fx, p, i / N)
    if t and t:lower():find(want, 1, true) then return i / N, "text-contains" end
  end
  return nil, "text not found"
end

local function export_fx(track, fx)
  local _, name = reaper.TrackFX_GetFXName(track, fx, "")
  local params = arr()
  for p = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, pname = reaper.TrackFX_GetParamName(track, fx, p, "")
    local _, formatted = reaper.TrackFX_GetFormattedParamValue(track, fx, p, "")
    params[#params+1] = { index = p, name = pname, normalized = reaper.TrackFX_GetParamNormalized(track, fx, p), formatted = formatted }
  end
  local config = {}
  if reaper.TrackFX_GetNamedConfigParm then
    -- reported plugin delay compensation (latency in samples), when the FX reports one
    local okp, pdc = reaper.TrackFX_GetNamedConfigParm(track, fx, "pdc")
    if okp then config.pdc = pdc end
  end
  if name:lower():find("reaeq", 1, true) and reaper.TrackFX_GetNamedConfigParm then
    for band = 0, 63 do
      for _, key in ipairs({ "BANDTYPE", "BANDENABLED" }) do
        local ok, value = reaper.TrackFX_GetNamedConfigParm(track, fx, key .. band)
        if ok then config[key .. band] = value end
      end
      if config["BANDTYPE" .. band] == nil and config["BANDENABLED" .. band] == nil then break end
    end
  end
  return { index = fx, name = name, guid = reaper.TrackFX_GetFXGUID(track, fx) or "", enabled = reaper.TrackFX_GetEnabled(track, fx),
           params = params, config = config }
end
local function export_chain(track)
  local list = arr()
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do list[#list+1] = export_fx(track, fx) end
  return list
end
local function export_project()
  local tracks = arr()
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
    tracks[#tracks+1] = { index = i, name = name, guid = reaper.GetTrackGUID(track), fx = export_chain(track) }
  end
  return { version = 1, project = J.label, tracks = tracks, master = { name = "MASTER", fx = export_chain(reaper.GetMasterTrack(0)) } }
end

local function fail(msg) LOG.errors[#LOG.errors+1] = msg error(msg, 0) end

local proj
local opened = false
local ok, err = pcall(function()
  step("new tab")
  local before = reaper.EnumProjects(-1)
  reaper.Main_OnCommand(40859, 0)
  proj = reaper.EnumProjects(-1)
  if proj == before then fail("could not create a new project tab") end
  opened = true
  LOG.reaper_version = reaper.GetAppVersion()

  if J.rpp then
    step("open rpp")
    reaper.Main_openProject("noprompt:" .. J.rpp)
    if J.copy_rpp and reaper.Main_SaveProjectEx then reaper.Main_SaveProjectEx(proj, J.copy_rpp, 0) end
    LOG.fx = export_project()
  else
    step("clean tab")
    for i = reaper.CountTracks(0) - 1, 0, -1 do reaper.DeleteTrack(reaper.GetTrack(0, i)) end
    local master = reaper.GetMasterTrack(0)
    for i = reaper.TrackFX_GetCount(master) - 1, 0, -1 do reaper.TrackFX_Delete(master, i) end
    reaper.GetSetProjectInfo(proj, "PROJECT_SRATE", J.srate or 48000, true)
    reaper.GetSetProjectInfo(proj, "PROJECT_SRATE_USE", 1, true)

    step("track + item")
    reaper.InsertTrackAtIndex(0, true)
    local tr = reaper.GetTrack(0, 0)
    reaper.SetMediaTrackInfo_Value(tr, "D_VOL", 10 ^ ((J.track_vol_db or 0) / 20))
    reaper.SetMediaTrackInfo_Value(tr, "D_PAN", J.track_pan or 0)
    LOG.track = { pan_mode = reaper.GetMediaTrackInfo_Value(tr, "I_PANMODE"), pan_law = reaper.GetMediaTrackInfo_Value(tr, "D_PANLAW"),
                  nchan = reaper.GetMediaTrackInfo_Value(tr, "I_NCHAN") }
    local src = reaper.PCM_Source_CreateFromFile(J.signal)
    if not src then fail("cannot open " .. J.signal) end
    local len = reaper.GetMediaSourceLength(src)
    local item = reaper.AddMediaItemToTrack(tr)
    local take = reaper.AddTakeToMediaItem(item)
    reaper.SetMediaItemTake_Source(take, src)
    reaper.SetMediaItemInfo_Value(item, "D_POSITION", 0)
    reaper.SetMediaItemInfo_Value(item, "D_LENGTH", len)
    reaper.SetMediaItemInfo_Value(item, "D_VOL", 10 ^ ((J.item_vol_db or 0) / 20))
    reaper.SetMediaItemInfo_Value(item, "D_FADEINLEN", J.fade_in or 0)
    reaper.SetMediaItemInfo_Value(item, "D_FADEOUTLEN", J.fade_out or 0)
    -- item properties in sorted key order (C_FADE*SHAPE before D_FADE*DIR: setting a shape can reset the curve), read back into the log
    local keys = {} for key in pairs(J.item_info or {}) do keys[#keys+1] = key end
    table.sort(keys)
    LOG.item_info = {}
    for _, key in ipairs(keys) do reaper.SetMediaItemInfo_Value(item, key, J.item_info[key]) end
    for _, key in ipairs(keys) do LOG.item_info[key] = reaper.GetMediaItemInfo_Value(item, key) end
    for key, value in pairs(J.take_info or {}) do reaper.SetMediaItemTakeInfo_Value(take, key, value) end
    LOG.source_length = len

    if J.fx then
      step("add fx")
      local fx = reaper.TrackFX_AddByName(tr, J.fx, false, -1)
      if fx < 0 then fx = reaper.TrackFX_AddByName(tr, "VST: " .. J.fx .. " (Cockos)", false, -1) end
      if fx < 0 then fail("cannot add FX " .. J.fx) end
      for key, value in pairs(J.config or {}) do
        local okc = reaper.TrackFX_SetNamedConfigParm(tr, fx, key, tostring(value))
        LOG.settings[#LOG.settings+1] = { config = key, requested = value, ok = okc }
      end
      local names, lower = {}, {}
      local np = reaper.TrackFX_GetNumParams(tr, fx)
      for p = 0, np - 1 do
        local _, n = reaper.TrackFX_GetParamName(tr, fx, p, "")
        names[n] = names[n] or p
        lower[n:lower()] = lower[n:lower()] or p
      end
      for _, s in ipairs(J.params or {}) do
        local p = names[s.name] or lower[s.name:lower()]
        if p == nil then
          local list = {} for n in pairs(names) do list[#list+1] = n end
          fail("no parameter '" .. s.name .. "' in " .. J.fx .. "; have: " .. table.concat(list, " | "))
        end
        local n, how = solve(tr, fx, p, s.value)
        if n == nil then fail("cannot set '" .. s.name .. "' to " .. tostring(s.value) .. ": " .. how) end
        reaper.TrackFX_SetParamNormalized(tr, fx, p, n)
        local _, got = reaper.TrackFX_GetFormattedParamValue(tr, fx, p, "")
        LOG.settings[#LOG.settings+1] = { name = s.name, index = p, requested = type(s.value) == "table" and s.value.n or s.value, how = how, normalized = n, got = got }
      end
      if J.scan and fx then
        -- formatted text of every parameter at J.scan+1 evenly spaced normalized values (restores the value afterwards)
        LOG.scan = {}
        for p = 0, reaper.TrackFX_GetNumParams(tr, fx) - 1 do
          local keep = reaper.TrackFX_GetParamNormalized(tr, fx, p)
          local _, pn = reaper.TrackFX_GetParamName(tr, fx, p, "")
          local col = arr()
          for i = 0, J.scan do col[#col+1] = fxtext(tr, fx, p, i / J.scan) or "" end
          reaper.TrackFX_SetParamNormalized(tr, fx, p, keep)
          LOG.scan[#LOG.scan+1] = { index = p, name = pn, text = col }
        end
      end
    end
    LOG.fx = export_project()
  end

  step("render settings")
  local sr = reaper.GetSetProjectInfo
  sr(proj, "RENDER_SETTINGS", 0, true)
  sr(proj, "RENDER_SRATE", J.srate or 48000, true)
  sr(proj, "RENDER_CHANNELS", 2, true)
  sr(proj, "RENDER_ADDTOPROJ", 0, true)
  sr(proj, "RENDER_DITHER", 0, true)
  sr(proj, "RENDER_NORMALIZE", 0, true)
  if J.range then
    sr(proj, "RENDER_BOUNDSFLAG", 0, true)
    sr(proj, "RENDER_STARTPOS", J.range[1], true)
    sr(proj, "RENDER_ENDPOS", J.range[2], true)
    sr(proj, "RENDER_TAILFLAG", 1, true)
  else
    sr(proj, "RENDER_BOUNDSFLAG", 1, true)
    sr(proj, "RENDER_TAILFLAG", 2, true)
  end
  sr(proj, "RENDER_TAILMS", J.tail_ms or 0, true)
  reaper.GetSetProjectInfo_String(proj, "RENDER_FILE", J.out_dir, true)
  reaper.GetSetProjectInfo_String(proj, "RENDER_PATTERN", J.name, true)
  reaper.GetSetProjectInfo_String(proj, "RENDER_FORMAT", J.format, true)
  LOG.render_format_read = select(2, reaper.GetSetProjectInfo_String(proj, "RENDER_FORMAT", "", false))

  step("render")
  reaper.Main_OnCommand(42230, 0)
  step("rendered")
end)
if not ok then LOG.errors[#LOG.errors+1] = tostring(err) end

-- close only our tab, never save anything of the user's
pcall(function()
  if opened and proj and reaper.EnumProjects(-1) == proj then
    -- save our own copy last, so the project is not dirty and closing the tab shows no "save changes?" question
    -- Main_SaveProjectEx writes a copy but leaves the tab dirty (and, in rpp mode, still pointing at the user's file), so closing it
    -- would ask "save changes?". Reload our own saved copy into this same tab ("noprompt:"), which leaves it clean, then close it.
    local copy = J.copy_rpp or (J.out_dir .. "/" .. J.name .. ".tab.RPP")
    if reaper.Main_SaveProjectEx then reaper.Main_SaveProjectEx(proj, copy, 0) end
    if reaper.IsProjectDirty(proj) ~= 0 then reaper.Main_openProject("noprompt:" .. copy) end
    local cur, curfn = reaper.EnumProjects(-1)
    if reaper.IsProjectDirty(cur) ~= 0 or (curfn ~= copy and cur ~= proj) then step("still dirty, tab left open") return end
    reaper.Main_OnCommand(40860, 0)
    step("tab closed")
  end
end)
LOG.ok = ok
writefile(J.log, json(LOG))
writefile(J.done, ok and "ok" or "error")
'''

# 32 bit float WAV (the base64 of REAPER's 'evaw' sink configuration)
FORMAT_WAV_32F = "ZXZhdyAAAAABAAAAAAAAAA=="


def _call(func, args, timeout=30.0):
    os.makedirs(BRIDGE, exist_ok=True)
    rid = random.randint(100, 990)
    req = os.path.join(BRIDGE, "request_%d.json" % rid)
    resp = os.path.join(BRIDGE, "response_%d.json" % rid)
    if os.path.exists(resp):
        os.remove(resp)
    tmp = req + ".tmp"
    with open(tmp, "w") as f:
        f.write(json.dumps({"func": func, "args": args}))
    os.rename(tmp, req)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(resp):
            time.sleep(0.05)
            try:
                with open(resp) as f:
                    d = json.load(f)
            except ValueError:
                continue
            os.remove(resp)
            return d
        time.sleep(0.1)
    raise TimeoutError("REAPER bridge did not answer %s (is the mcp_bridge_file_v2.lua script running?)" % func)


def run_job(job, timeout=900.0):
    """job: dict for the Lua (see LUA). Returns {"wav": path, "log": parsed JSON written by the script}."""
    work = job["out_dir"]
    os.makedirs(work, exist_ok=True)
    name = job["name"]
    job = dict(job)
    job.setdefault("format", FORMAT_WAV_32F)
    job["log"] = os.path.join(work, name + ".reaper.json")
    job["done"] = os.path.join(work, name + ".reaper.done")
    wav = os.path.join(work, name + ".wav")
    for p in (job["log"], job["done"], wav):
        if os.path.exists(p):
            os.remove(p)
    script = os.path.join(work, name + ".reaper.lua")
    with open(script, "w") as f:
        f.write(LUA.replace("@JOB@", lua_literal(job)))
    r = _call("AddRemoveReaScript", [True, 0, script, True])
    cmd = r.get("ret")
    if not r.get("ok") or not isinstance(cmd, (int, float)) or cmd == 0:
        raise RuntimeError("AddRemoveReaScript failed: %r" % r)
    try:
        _call("Main_OnCommand", [int(cmd), 0], timeout=timeout)
        t0 = time.time()
        while not os.path.exists(job["done"]):
            if time.time() - t0 > timeout:
                raise TimeoutError("REAPER render did not finish")
            time.sleep(0.2)
    finally:
        try:
            _call("AddRemoveReaScript", [False, 0, script, True])
        except Exception:
            pass
    with open(job["log"], encoding="utf-8", errors="replace") as f:     # some JS effects return Latin-1 text
        log = json.load(f)
    if not log.get("ok") or not os.path.exists(wav):
        raise RuntimeError("REAPER job failed: %s (steps: %s)" % (log.get("errors"), log.get("steps")))
    return {"wav": wav, "log": log}


def run_project(rpp, out_dir, name, start=None, end=None, tail_ms=0, timeout=3600.0):
    """Renders an existing .RPP in a new tab (a copy is saved into out_dir; the original is not touched)."""
    job = {"name": name, "out_dir": out_dir, "rpp": os.path.abspath(rpp), "label": os.path.basename(rpp), "tail_ms": tail_ms,
           "copy_rpp": os.path.join(out_dir, name + ".copy.RPP")}
    if start is not None:
        job["range"] = [start, end]
    return run_job(job, timeout)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--probe", help="FX name: print its parameter names and default formatted values")
    ap.add_argument("--work", default=os.path.expanduser("~/.rovox-parity/work"))
    a = ap.parse_args()
    if a.probe:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import gen_signals
        sig = gen_signals.write("impulse", 2, a.work)
        res = run_job({"name": "probe_" + a.probe, "out_dir": a.work, "signal": sig, "fx": a.probe, "label": "probe"})
        for fx in res["log"]["fx"]["tracks"][0]["fx"]:
            print(fx["name"], fx["config"])
            for p in fx["params"]:
                print("  %2d %-28s n=%.4f  %s" % (p["index"], p["name"], p["normalized"], p["formatted"]))
