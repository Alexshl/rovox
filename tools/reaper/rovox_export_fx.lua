-- SPDX-License-Identifier: AGPL-3.0-or-later
-- Copyright (C) 2026 Rovox contributors
--
-- Rovox: export the FX settings of the open REAPER project for the Rovox RPP import.
--
-- Usage: open the project in REAPER, then Actions > Show action list > ReaScript: Load... and pick this file (or run it from there).
-- It writes <project>.rovoxfx.json next to the .RPP (the project must be saved once). Put that file beside the .RPP that you give to
-- Rovox (`import.rpp`): Rovox then translates the settings of ReaEQ, ReaComp, ReaLimit, ReaGate, ReaVerbate and ReaDelay into its own
-- stock effects (Rovox EQ, Comp, Limit, Gate, Verb, Delay) instead of leaving them at their defaults. The same file can be passed
-- explicitly as the `fx_sidecar` parameter of `import.rpp`.
--
-- What is written, per track (index, name, GUID) and for the master, per FX: name, GUID, enabled, and every parameter with its
-- index, name, normalized value and the text REAPER shows for it (no unit: "-6.0", "768.3", "4.00"). For ReaEQ also the band types
-- and enable flags (named config BANDTYPE<n> / BANDENABLED<n>), when this REAPER version offers them. Only the public ReaScript API is
-- used; nothing in the project is changed.

local reaper = reaper

---------------------------------------------------------------------------------------------------------------------- JSON
local ARRAY = {}

local function array(t)
  return setmetatable(t or {}, ARRAY)
end

local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

-- REAPER returns non-UTF-8 text from some JS effects (Latin-1, e.g. byte 0xE9); JSON must be UTF-8: every byte that is not part of a
-- valid UTF-8 sequence is taken as Latin-1 and converted.
local function to_utf8(s)
  if not s:find('[\128-\255]') then return s end
  local out, i, n = {}, 1, #s
  while i <= n do
    local c = s:byte(i)
    local len = 0
    if c < 0x80 then len = 1
    elseif c >= 0xC2 and c <= 0xDF then len = 2
    elseif c >= 0xE0 and c <= 0xEF then len = 3
    elseif c >= 0xF0 and c <= 0xF4 then len = 4 end
    local ok = len > 0 and i + len - 1 <= n
    for k = 1, len - 1 do
      if not ok then break end
      local cc = s:byte(i + k)
      if cc < 0x80 or cc > 0xBF then ok = false end
    end
    if ok and len > 1 then
      local c2 = s:byte(i + 1)
      if (c == 0xE0 and c2 < 0xA0) or (c == 0xED and c2 > 0x9F) or (c == 0xF0 and c2 < 0x90) or (c == 0xF4 and c2 > 0x8F) then ok = false end
    end
    if ok then
      out[#out + 1] = s:sub(i, i + len - 1)
      i = i + len
    else
      out[#out + 1] = string.char(0xC0 + math.floor(c / 64), 0x80 + c % 64)
      i = i + 1
    end
  end
  return table.concat(out)
end

local function encode_string(s)
  s = to_utf8(tostring(s)):gsub('[%c"\\]', function(c)
    return escapes[c] or string.format('\\u%04x', c:byte())
  end)
  return '"' .. s .. '"'
end

local function encode_number(n)
  if n ~= n or n == math.huge or n == -math.huge then
    return 'null'
  end
  if n == math.floor(n) and math.abs(n) < 1e15 then
    return string.format('%d', n)
  end
  return string.format('%.9g', n)
end

local encode

local function encode_array(t, out)
  out[#out + 1] = '['
  for i = 1, #t do
    if i > 1 then out[#out + 1] = ',' end
    encode(t[i], out)
  end
  out[#out + 1] = ']'
end

local function encode_object(t, out)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = tostring(k) end
  table.sort(keys)
  out[#out + 1] = '{'
  for i, k in ipairs(keys) do
    if i > 1 then out[#out + 1] = ',' end
    out[#out + 1] = encode_string(k)
    out[#out + 1] = ':'
    encode(t[k], out)
  end
  out[#out + 1] = '}'
end

encode = function(v, out)
  local kind = type(v)
  if kind == 'table' then
    if getmetatable(v) == ARRAY then
      encode_array(v, out)
    else
      encode_object(v, out)
    end
  elseif kind == 'string' then
    out[#out + 1] = encode_string(v)
  elseif kind == 'number' then
    out[#out + 1] = encode_number(v)
  elseif kind == 'boolean' then
    out[#out + 1] = v and 'true' or 'false'
  else
    out[#out + 1] = 'null'
  end
end

local function to_json(v)
  local out = {}
  encode(v, out)
  return table.concat(out)
end

------------------------------------------------------------------------------------------------------------------- export
local function fx_config(track, fx, name)
  -- ReaEQ: band types and enable flags are not parameters; ask for them through the named config, when available
  local config = {}
  if not name:lower():find('reaeq', 1, true) then return config end
  if not reaper.TrackFX_GetNamedConfigParm then return config end
  for band = 0, 63 do
    for _, key in ipairs({ 'BANDTYPE', 'BANDENABLED' }) do
      local ok, value = reaper.TrackFX_GetNamedConfigParm(track, fx, key .. band)
      if ok then config[key .. band] = value end
    end
    if config['BANDTYPE' .. band] == nil and config['BANDENABLED' .. band] == nil then break end
  end
  return config
end

local function export_fx(track, fx)
  local _, name = reaper.TrackFX_GetFXName(track, fx, '')
  local params = array()
  for p = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, pname = reaper.TrackFX_GetParamName(track, fx, p, '')
    local _, formatted = reaper.TrackFX_GetFormattedParamValue(track, fx, p, '')
    params[#params + 1] = {
      index = p,
      name = pname,
      normalized = reaper.TrackFX_GetParamNormalized(track, fx, p),
      formatted = formatted,
    }
  end
  return {
    index = fx,
    name = name,
    guid = reaper.TrackFX_GetFXGUID(track, fx) or '',
    enabled = reaper.TrackFX_GetEnabled(track, fx),
    params = params,
    config = fx_config(track, fx, name),
  }
end

local function export_chain(track)
  local list = array()
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    list[#list + 1] = export_fx(track, fx)
  end
  return list
end

local function main()
  local project, project_path = reaper.EnumProjects(-1, '')
  if not project_path or project_path == '' then
    reaper.MB('Save the project first: Rovox writes the FX file next to the .RPP.', 'Rovox FX export', 0)
    return
  end

  local tracks = array()
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', '', false)
    tracks[#tracks + 1] = {
      index = i,
      name = name,
      guid = reaper.GetTrackGUID(track),
      fx = export_chain(track),
    }
  end

  local master = reaper.GetMasterTrack(0)
  local doc = {
    version = 1,
    project = project_path:match('[^/\\]+$') or project_path,
    tracks = tracks,
    master = { name = 'MASTER', fx = export_chain(master) },
  }

  local out_path = project_path:gsub('%.[Rr][Pp][Pp]$', '') .. '.rovoxfx.json'
  local file, err = io.open(out_path, 'wb')
  if not file then
    reaper.MB('Cannot write ' .. out_path .. ': ' .. tostring(err), 'Rovox FX export', 0)
    return
  end
  file:write(to_json(doc))
  file:close()

  local count = 0
  for _, t in ipairs(tracks) do count = count + #t.fx end
  count = count + #doc.master.fx
  reaper.MB(string.format('Wrote %d FX of %d tracks to\n%s', count, #tracks, out_path), 'Rovox FX export', 0)
end

main()
