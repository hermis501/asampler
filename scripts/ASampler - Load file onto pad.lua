-- @description ASampler: Load file onto pad
-- @version 1.0.0
-- @about
--   Loads an audio file onto an ASampler pad from REAPER, without opening
--   ASampler's window. Also a worked example of ASampler's script protocol,
--   for writing your own actions.
--
--   Select the track that holds ASampler (or leave nothing selected if only
--   one track has it), then run this action. It asks for a file, then for a
--   pad number (1 to 32; leave it empty for the pad selected in ASampler).
--   ASampler loads the file into that pad's edit layer and speaks the result
--   itself, as if you had loaded it from its own window.
--
--   The protocol: write the request "<pad>|<file path>" into REAPER's
--   ExtState, section "ASampler", key "load " .. <track GUID>. ASampler polls
--   that key ten times a second, deletes it, and loads the file. Pad 0 or an
--   empty pad means the selected pad.

local SCRIPT_TITLE = "ASampler: Load file onto pad"
local NUM_PADS     = 32

local function tell(message)
  if reaper.osara_outputMessage then
    reaper.osara_outputMessage(message)
  end
  reaper.ShowMessageBox(message, SCRIPT_TITLE, 0)
end

local function hasASampler(track)
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, fx, "")
    if name:find("ASampler", 1, true) then
      return true
    end
  end
  return false
end

-- The ASampler track: the selected track if it has ASampler, otherwise the
-- only track in the project that has it.
local function findTrack()
  local selected = reaper.GetSelectedTrack(0, 0)
  if selected and hasASampler(selected) then
    return selected
  end

  local found, count = nil, 0
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    if hasASampler(track) then
      count = count + 1
      found = track
    end
  end

  if count == 1 then return found end
  if count > 1 then
    return nil, "Several tracks have ASampler. Select the one to load into, then run this action again."
  end
  return nil, "No track has ASampler on it. Insert ASampler on a track first."
end

local function main()
  local track, problem = findTrack()
  if not track then
    tell(problem)
    return
  end

  local picked, path = reaper.GetUserFileNameForRead("", "Choose a sample for ASampler",
                                                      "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg")
  if not picked or path == "" then
    return   -- cancelled
  end

  local ok, answer = reaper.GetUserInputs(SCRIPT_TITLE, 1,
    "Pad number (1 to " .. NUM_PADS .. ", empty for the selected pad)", "")
  if not ok then
    return
  end
  answer = answer:match("^%s*(.-)%s*$")

  local pad = 0
  if answer ~= "" then
    pad = tonumber(answer)
    if not pad or pad ~= math.floor(pad) or pad < 1 or pad > NUM_PADS then
      tell("\"" .. answer .. "\" is not a pad number. ASampler has pads 1 to " .. NUM_PADS .. ".")
      return
    end
  end

  -- ASampler announces the outcome itself; it reads this within 100 ms.
  reaper.SetExtState("ASampler", "load " .. reaper.GetTrackGUID(track),
                     math.floor(pad) .. "|" .. path, false)
end

main()
