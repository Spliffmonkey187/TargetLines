-- Every tunable in one place.
--
-- Lua owns the settings and pushes them into the module; the module holds no
-- state of its own that Lua cannot reproduce. That is what lets the config
-- panel render current values, and lets them survive a reload -- neither is
-- possible if a setting exists only inside the DLL, where nothing can read it
-- back.

local config = require('config')

local defaults = {
    -- General
    enabled = true,
    target_line = true,
    filter = 'all',       -- all | alliance | party
    attacks = 'first',    -- first | repeat | off

    -- Which kinds of entity draw anything at all
    show_me = true,
    show_party = true,
    show_trust = true,
    show_pet = true,
    show_alliance = true,
    show_others = true,
    show_enemy = true,

    -- Lines
    curved = true,
    depth = true,
    width = 3.00,
    arch = 0.18,
    bow = 11.25,
    orb = 22,

    -- Where lines attach
    chest_me = 0.70,
    chest_target = 0.70,
    lift = 0.00,

    -- Area of effect
    aoe = true,
    aoe_hold = 2.20,
    aoe_lift = 0.00,
    aoe_sweepwidth = 1.30,
    aoe_radius = 1.10,
    aoe_chest = 0.55,
    aoe_orbit = 1.30,
    aoe_tail = 2.60,
    aoe_width = 2.20,

    -- Config panel row height and character width, in pixels, used to map
    -- a click back to a row and column. Normally measured from what the
    -- font actually rendered; these are the fallback and the manual
    -- override, which //tlines ui switches to.
    ui_auto = true,
    -- Match the bracket columns exactly rather than splitting the control
    -- area in half. Off by default: it depends on the measured character
    -- width being accurate, where the split does not.
    ui_precise = false,

    -- Bold the title and section headings. Windower has no inline bold,
    -- so this is drawn by a second text object laid over the first.
    ui_bold_titles = true,
    ui_row = 16,
    ui_char = 8,

    -- The panel itself, in the shape Windower's texts library expects.
    -- Handing this table and the settings root to texts.new lets the
    -- library drag the panel and save where it was dropped, with no
    -- drag handling of our own.
    display = {
        pos = {x = 300, y = 200},
        text = {font = 'Consolas', size = 10, alpha = 255,
            red = 255, green = 255, blue = 255},
        bg = {alpha = 200, red = 0, green = 0, blue = 0, visible = true},
        flags = {draggable = true, bold = false},  -- bold comes from the overlay
        padding = 4,
    },
}

local settings = config.load(defaults)

-- Rows of the config panel, in order. A row carrying only a section name is a
-- heading and is not clickable.
--
--   toggle  flipped by clicking the control
--   choice  cycled through its options by the arrows
--   value   nudged by step, clamped between min and max
local spec = {
    {section = 'General'},
    {name = 'enabled',      label = 'Enable Lines',   type = 'toggle'},
    {name = 'target_line',  label = 'Target Line',    type = 'toggle'},
    {name = 'filter',       label = 'Filter',         type = 'choice',
        options = {'all', 'alliance', 'party'}},
    {name = 'attacks',      label = 'Auto-attacks',   type = 'choice',
        options = {'first', 'repeat', 'off'}},

    {section = 'Show'},
    {name = 'show_me',       label = 'Me',            type = 'toggle'},
    {name = 'show_party',    label = 'Party',         type = 'toggle'},
    {name = 'show_trust',    label = 'Trusts',        type = 'toggle'},
    {name = 'show_pet',      label = 'Pets',          type = 'toggle'},
    {name = 'show_alliance', label = 'Alliance',      type = 'toggle'},
    {name = 'show_others',   label = 'Other Players', type = 'toggle'},
    {name = 'show_enemy',    label = 'Enemies',       type = 'toggle'},

    {section = 'Lines'},
    {name = 'curved', label = 'Curved Arcs', type = 'toggle'},
    {name = 'depth',  label = 'World Depth', type = 'toggle'},
    {name = 'width',  label = 'Line Width',  type = 'value', step = 0.5,  min = 1,   max = 16},
    {name = 'arch',   label = 'Arc Height',  type = 'value', step = 0.02, min = 0,   max = 2},
    {name = 'bow',    label = 'Arc Lean',    type = 'value', step = 2.5,  min = -90, max = 90},
    {name = 'orb',    label = 'Orb Size',    type = 'value', step = 2,    min = 0,   max = 64},

    {section = 'Attach Height'},
    {name = 'chest_me',     label = 'On Me',      type = 'value', step = 0.05, min = 0, max = 1},
    {name = 'chest_target', label = 'On Others',  type = 'value', step = 0.05, min = 0, max = 1},
    {name = 'lift',         label = 'Extra Lift', type = 'value', step = 0.1,  min = 0, max = 20},

    {section = 'Area of Effect'},
    {name = 'aoe',            label = 'AoE Rings',    type = 'toggle'},
    {name = 'aoe_hold',       label = 'Duration',     type = 'value', step = 0.2,  min = 0.4, max = 8},
    {name = 'aoe_lift',       label = 'Sweep Lift',   type = 'value', step = 0.1,  min = 0,   max = 10},
    {name = 'aoe_sweepwidth', label = 'Sweep Width',  type = 'value', step = 0.1,  min = 0.2, max = 8},
    {name = 'aoe_radius',     label = 'Comet Radius', type = 'value', step = 0.1,  min = 0.2, max = 10},
    {name = 'aoe_chest',      label = 'Comet Height', type = 'value', step = 0.05, min = 0,   max = 1},
    {name = 'aoe_orbit',      label = 'Comet Speed',  type = 'value', step = 0.1,  min = 0,   max = 6},
    {name = 'aoe_tail',       label = 'Comet Tail',   type = 'value', step = 0.2,  min = 0.2, max = 6.2},
    {name = 'aoe_width',      label = 'Comet Width',  type = 'value', step = 0.1,  min = 0.2, max = 8},
}

local by_name = {}
for _, row in ipairs(spec) do
    if row.name then
        by_name[row.name] = row
    end
end

local M = {
    settings = settings,
    defaults = defaults,
    spec = spec,
}

function M.row(name)
    return by_name[name]
end

function M.save()
    config.save(settings, 'all')
end

local function clamp(row, value)
    if row.min and value < row.min then
        return row.min
    end

    if row.max and value > row.max then
        return row.max
    end

    return value
end

-- Nudge a value row, or step a choice row along its options. delta is -1 or 1.
function M.nudge(name, delta)
    local row = by_name[name]
    if not row then
        return
    end

    if row.type == 'toggle' then
        settings[name] = not settings[name]
    elseif row.type == 'choice' then
        local current = 1
        for index, option in ipairs(row.options) do
            if settings[name] == option then
                current = index
            end
        end

        local next_index = ((current - 1 + delta) % #row.options) + 1
        settings[name] = row.options[next_index]
    else
        -- Rounded to the step so repeated clicks land on clean numbers rather
        -- than accumulating floating point drift.
        local stepped = (tonumber(settings[name]) or 0) + row.step * delta
        settings[name] = clamp(row, math.floor(stepped / row.step + 0.5) * row.step)
    end
end

function M.set(name, value)
    local row = by_name[name]
    if not row then
        return false
    end

    if row.type == 'toggle' then
        settings[name] = value and true or false
    elseif row.type == 'choice' then
        for _, option in ipairs(row.options) do
            if option == value then
                settings[name] = option
                return true
            end
        end

        return false
    else
        local number = tonumber(value)
        if not number then
            return false
        end

        settings[name] = clamp(row, number)
    end

    return true
end

function M.reset()
    for name, value in pairs(defaults) do
        -- Panel placement and font belong to the player, not to the look,
        -- so they survive a reset.
        if not name:match('^ui_') and name ~= 'display' then
            settings[name] = value
        end
    end
end

-- Render a value the way both the panel and the chat commands want it.
function M.display(name)
    local row = by_name[name]
    local value = settings[name]

    if not row then
        return tostring(value)
    elseif row.type == 'toggle' then
        return value and 'ON' or 'OFF'
    elseif row.type == 'choice' then
        return tostring(value)
    elseif row.step and row.step >= 1 then
        return ('%d'):format(value)
    end

    return ('%.2f'):format(value)
end

return M
