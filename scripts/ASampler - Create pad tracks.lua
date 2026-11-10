-- @description ASampler: Create pad tracks
-- @version 1.0.0
-- @about
--   Gives each ASampler pad its own REAPER track, for separate faders, FX
--   and stems.
--
--   Select the track that holds ASampler (or leave nothing selected if only
--   one track has it), then run this action. It asks how many pads to route
--   (1 to 32; the default 16 is bank A) and, for each pad:
--     * widens the ASampler track to carry that pad's output channels,
--     * creates a track named "ASampler Pad N" below it,
--     * sends the pad's stereo output to that track.
--   Running it again only adds what is missing, so it is safe to repeat.
--
--   Every result is spoken through OSARA when it is installed, and shown in
--   a message box otherwise. The whole action is one undo step.

local SCRIPT_TITLE = "ASampler: Create pad tracks"
local NUM_PADS     = 32                 -- two banks of 16
local DEFAULT_PADS = 16                 -- bank A
local MAX_CHANNELS = 2 + 2 * NUM_PADS   -- Main pair + one pair per pad

-- Speak through OSARA if available, otherwise show a message box. A message
-- box is fully readable with a screen reader, but OSARA's speech doesn't
-- interrupt the user's flow.
local function tell(message, isError)
  if reaper.osara_outputMessage then
    reaper.osara_outputMessage(message)
    if isError then
      reaper.ShowMessageBox(message, SCRIPT_TITLE, 0)
    end
  else
    reaper.ShowMessageBox(message, SCRIPT_TITLE, 0)
  end
end

local function findASamplerFx(track)
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, fx, "")
    if name:find("ASampler", 1, true) then
      return fx
    end
  end
  return nil
end

local function trackName(track)
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  if name == "" then
    name = "Track " .. math.floor(reaper.GetMediaTrackInfo_Value(track, "IP_TRACKNUMBER"))
  end
  return name
end

-- The ASampler track: the selected track if it has ASampler, otherwise the
-- only track in the project that has it.
local function findSourceTrack()
  local selected = reaper.GetSelectedTrack(0, 0)
  if selected then
    local fx = findASamplerFx(selected)
    if fx then return selected, fx end
  end

  local found, foundFx, count = nil, nil, 0
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local fx = findASamplerFx(track)
    if fx then
      count = count + 1
      found, foundFx = track, fx
    end
  end

  if count == 1 then return found, foundFx end
  if count > 1 then
    return nil, nil, "Several tracks have ASampler. Select the one to route, then run this action again."
  end
  return nil, nil, "No track has ASampler on it. Insert ASampler on a track first."
end

-- Does `source` already send the pad's channel pair somewhere?
local function existingSendForPad(source, firstChannel)
  for s = 0, reaper.GetTrackNumSends(source, 0) - 1 do
    local src = reaper.GetTrackSendInfo_Value(source, 0, s, "I_SRCCHAN")
    if math.floor(src) == firstChannel then
      return true
    end
  end
  return false
end

-- Map plugin output pin `pin` straight to track channel `pin`. REAPER only
-- maps the pins that fit the track's channel count when the plugin loads, so
-- after widening the track the pad pins have to be connected explicitly.
local function mapOutputPinOneToOne(track, fx, pin)
  local low, high = 0, 0
  if pin < 32 then
    low = 1 << pin
  else
    high = 1 << (pin - 32)
  end
  reaper.TrackFX_SetPinMappings(track, fx, 1, pin, low, high)
end

local function main()
  local source, fx, problem = findSourceTrack()
  if not source then
    tell(problem, true)
    return
  end

  local _, _, outputPins = reaper.TrackFX_GetIOSize(source, fx)
  if outputPins < 4 then
    tell("ASampler on " .. trackName(source) .. " reports only " .. outputPins
         .. " output channels, so REAPER has not enabled its pad outputs. "
         .. "Check that this is the VST3 version of ASampler.", true)
    return
  end

  local ok, answer = reaper.GetUserInputs(SCRIPT_TITLE, 1,
                                          "Number of pads to route (1 to " .. NUM_PADS .. "):",
                                          tostring(DEFAULT_PADS))
  if not ok then return end

  local count = tonumber(answer)
  if not count or count < 1 or count > NUM_PADS or count ~= math.floor(count) then
    tell("Please enter a whole number from 1 to " .. NUM_PADS .. ".", true)
    return
  end

  local available = math.floor((outputPins - 2) / 2)
  count = math.min(count, available)

  reaper.Undo_BeginBlock()
  reaper.PreventUIRefresh(1)

  -- Widen the ASampler track to carry Main plus the routed pads' pairs.
  local needed  = 2 + 2 * count
  local current = math.floor(reaper.GetMediaTrackInfo_Value(source, "I_NCHAN"))
  if current < needed then
    reaper.SetMediaTrackInfo_Value(source, "I_NCHAN", needed)
  end
  for pin = 0, needed - 1 do
    mapOutputPinOneToOne(source, fx, pin)
  end

  local sourceIndex = math.floor(reaper.GetMediaTrackInfo_Value(source, "IP_TRACKNUMBER")) - 1
  local insertAt    = sourceIndex + 1
  local created     = 0

  for pad = 1, count do
    local firstChannel = 2 * pad   -- pad 1 is channels 3/4 (0-based 2)
    if not existingSendForPad(source, firstChannel) then
      reaper.InsertTrackAtIndex(insertAt, true)
      local padTrack = reaper.GetTrack(0, insertAt)
      reaper.GetSetMediaTrackInfo_String(padTrack, "P_NAME", "ASampler Pad " .. pad, true)

      local send = reaper.CreateTrackSend(source, padTrack)
      reaper.SetTrackSendInfo_Value(source, 0, send, "I_SRCCHAN", firstChannel)
      reaper.SetTrackSendInfo_Value(source, 0, send, "I_DSTCHAN", 0)
      -- Post-FX, pre-fader: the ASampler track's fader then controls only
      -- the pads still playing through Main, as with any multi-out
      -- instrument.
      reaper.SetTrackSendInfo_Value(source, 0, send, "I_SENDMODE", 3)

      created = created + 1
    end
    insertAt = insertAt + 1
  end

  reaper.PreventUIRefresh(-1)
  reaper.TrackList_AdjustWindows(false)
  reaper.Undo_EndBlock("ASampler: create pad tracks", -1)

  if created == 0 then
    tell("All " .. count .. " pads on " .. trackName(source) .. " already have their own tracks.")
  else
    tell("Created " .. created .. " pad tracks below " .. trackName(source)
         .. ". Each pad now plays on its own track; pads you did not route stay on "
         .. trackName(source) .. ".")
  end
end

main()
