-- The config panel.
--
-- A monospace text object plus click hit-testing, which is all Windower
-- provides and all this needs.
--
-- Rendering records what each line is as it builds them, so a click resolves by
-- looking the row up rather than by assuming the layout. Dividers and headings
-- can then move around without the hit test knowing or caring.
--
-- Windower has no inline bold, only colour, so bolding just the titles takes a
-- second text object laid exactly over the first: the base carries every
-- ordinary row with the title and heading lines left blank, and the overlay
-- carries only those, in bold. Same font, same line height, same origin, so the
-- two composite into one panel.
--
-- Dragging is not handled here. Passing the settings root to texts.new lets the
-- library move the panel and save where it was dropped by itself; the overlay
-- follows via the library's own drag event.
--
-- Nothing here touches the module. The panel edits settings; whoever owns them
-- pushes them onward.

local texts = require('texts')

local M = {}

local config = nil
local box = nil
local overlay = nil
local visible = false
local pressed = nil
local on_change = nil

-- Character columns. The label occupies the left, the control the right.
local LABEL_WIDTH = 22
local CONTROL_WIDTH = 15
local ROW_WIDTH = LABEL_WIDTH + CONTROL_WIDTH + 2
-- [x] sits hard against the right edge, and is the only thing on the title
-- row that responds, so dragging by the title cannot close the panel.
local CLOSE_COLUMN = ROW_WIDTH - 3

-- Inline colour codes, rendered by the text primitive itself. Everything after
-- one stays tinted until the next, so each field ends by going back to white.
local WHITE = '\\cs(255,255,255)'
local GREEN = '\\cs(80,235,120)'
local RED = '\\cs(240,90,90)'
local PURPLE = '\\cs(200,120,255)'
local BLUE = '\\cs(80,200,255)'
local RULE = '\\cs(45,105,150)'
local GREY = '\\cs(150,150,150)'

-- What each rendered line is, keyed by line number. Built during render and
-- read by the hit test, so layout changes cannot desynchronise the two.
local row_map = {}

-- Measured from what the font actually rendered rather than assumed. Read at
-- click time, never straight after box:text(): the primitive has not
-- necessarily redrawn by then, so measuring immediately returns the size of the
-- previous text.
local measured = {width = nil, row = nil, lines = nil, longest = nil}

-- Set by //tlines uidebug. Reports what each click resolved to, which turns a
-- miss into a number rather than a guess.
local debugging = false
local report = nil

local function settings()
    return config.settings
end

-- Centre text in a field, so a value changing width does not shift the
-- brackets around it.
local function centred(text, width)
    local pad = width - #text
    if pad < 0 then
        return text
    end

    local left = math.floor(pad / 2)
    return (' '):rep(left) .. text .. (' '):rep(pad - left)
end

-- Keep the bold overlay sitting exactly on the base panel.
local function sync_overlay()
    if not box or not overlay then
        return
    end

    local x, y = box:pos()
    local ox, oy = overlay:pos()
    if ox ~= x or oy ~= y then
        overlay:pos(x, y)
    end
end

M.sync = sync_overlay

local function render()
    if not box then
        return
    end

    if not visible then
        box:hide()
        if overlay then
            overlay:hide()
        end
        return
    end

    -- Titles go bold, so they are rendered by the overlay and left blank in the
    -- base. Both lists stay the same length, which is what keeps the two layers
    -- lined up row for row.
    local bold_titles = settings().ui_bold_titles ~= false
    local base = {}
    local top = {}
    row_map = {}

    local function add(line, entry, is_title)
        local index = #base + 1
        if is_title and bold_titles then
            base[index] = ''
            top[index] = line
        else
            base[index] = line
            top[index] = ''
        end
        row_map[index] = entry
    end

    local title = 'TargetLines Config UI'
    add(('%s%s%s[x]'):format(WHITE, title,
        (' '):rep(math.max(CLOSE_COLUMN - #title, 1))), 'title', true)
    add(RULE .. ('='):rep(ROW_WIDTH), nil, false)

    local first_section = true
    for _, row in ipairs(config.spec) do
        if row.section then
            -- A rule between sections, but not above the first: the title
            -- already has one under it.
            if not first_section then
                add(RULE .. ('-'):rep(ROW_WIDTH), nil, false)
            end
            first_section = false

            add(('%s %s'):format(BLUE, row.section), nil, true)
        else
            local label = row.label
            if #label > LABEL_WIDTH - 2 then
                label = label:sub(1, LABEL_WIDTH - 2)
            end

            local control
            if row.type == 'toggle' then
                -- On reads green and off reads red, so the state of a whole
                -- section is legible without reading any of the words.
                local tint = settings()[row.name] and GREEN or RED
                control = ('%s[%s%s%s]'):format(
                    WHITE, tint,
                    centred(config.display(row.name), CONTROL_WIDTH - 2), WHITE)
            else
                -- Arrows step through a list of choices, plus and minus nudge a
                -- number: the glyph says what the click will do.
                local less, more = '[-]', '[+]'
                if row.type == 'choice' then
                    less, more = '[<]', '[>]'
                end

                control = ('%s%s%s%s%s%s'):format(
                    WHITE, less, PURPLE,
                    centred(config.display(row.name), CONTROL_WIDTH - 6),
                    WHITE, more)
            end

            add(('%s  %s%s'):format(
                WHITE, label .. (' '):rep(LABEL_WIDTH - #label), control),
                row, false)
        end
    end

    add('', nil, false)
    local hint = 'drag to move, [x] to close'
    add(GREY .. (' '):rep(math.max(math.floor((ROW_WIDTH - #hint) / 2), 0))
        .. hint, nil, false)

    -- box:text() sets the displayed string. Assigning box.text instead goes
    -- through the __newindex metamethod and merely defines an interpolation
    -- variable named text, which nothing references, so the panel renders
    -- empty.
    box:text(table.concat(base, '\n'))
    box:show()

    if overlay then
        overlay:text(table.concat(top, '\n'))
        overlay:bold(bold_titles)
        sync_overlay()

        if bold_titles then
            overlay:show()
        else
            overlay:hide()
        end
    end

    measured.lines = #base

    -- Colour codes take no width on screen, so they come out before a line is
    -- measured.
    local longest = 0
    for _, line in ipairs(base) do
        local plain = line:gsub('\\cs%(%d+,%d+,%d+%)', '')
        if #plain > longest then
            longest = #plain
        end
    end

    measured.longest = longest
end

M.render = render

function M.visible()
    return visible
end

function M.toggle()
    visible = not visible
    render()
    return visible
end

function M.hide()
    visible = false
    if box then
        box:hide()
    end
    if overlay then
        overlay:hide()
    end
end

-- Which control, if any, sits under this pixel.
local function control_at(x, y)
    if not visible or not box then
        return nil
    end

    local set = settings()
    local origin_x, origin_y = box:pos()
    local padding = tonumber(set.display and set.display.padding) or 4

    -- Measured here rather than at render time, so the primitive has certainly
    -- drawn and the extents are its real size.
    local extent_w, extent_h = box:extents()
    if extent_w and extent_w > 80 then
        measured.width = extent_w - padding * 2
    end
    if extent_h and extent_h > 20 and measured.lines and measured.lines > 0 then
        measured.row = (extent_h - padding * 2) / measured.lines
    end

    local width = measured.width
    if not width or width < 80 then
        width = 300
    end

    local row_height = tonumber(set.ui_row) or 16
    if set.ui_auto ~= false and measured.row then
        row_height = measured.row
    end

    local row = math.floor((y - origin_y - padding) / row_height) + 1
    local rel = x - origin_x - padding

    if rel < 0 or rel > width then
        return nil
    end

    -- The control column occupies the right of the row. Everything left of it
    -- is label, and stays free for dragging.
    local control_start = width * 0.55

    local char_width = nil
    if measured.longest and measured.longest > 0 then
        char_width = width / measured.longest
    end

    local entry = row_map[row]
    if entry == 'title' then
        -- Deliberately tight. The title row is the natural place to grab
        -- the panel, so anything looser closes it while you are dragging.
        if not char_width then
            return nil
        end

        local column = math.floor(rel / char_width)
        if column >= CLOSE_COLUMN - 1 and column <= CLOSE_COLUMN + 3 then
            return {kind = 'close'}
        end

        return nil
    end

    if type(entry) ~= 'table' or not entry.name then
        return nil
    end

    if entry.type == 'toggle' then
        if rel >= control_start then
            return {kind = 'toggle', name = entry.name}
        end

        return nil
    end

    -- Precise mode matches the bracket columns exactly, so clicking the readout
    -- between them does nothing. It needs the character width to be right,
    -- which is why the forgiving split is the fallback rather than the reverse.
    if set.ui_precise and char_width then
        local column = math.floor(rel / char_width)
        local first = LABEL_WIDTH + 2

        if column >= first and column <= first + 2 then
            return {kind = 'nudge', name = entry.name, delta = -1}
        elseif column >= first + CONTROL_WIDTH - 3
            and column <= first + CONTROL_WIDTH then
            return {kind = 'nudge', name = entry.name, delta = 1}
        end

        return nil
    end

    if rel < control_start then
        return nil
    end

    -- Left half of the control column decrements, right half increments.
    if rel < (control_start + width) / 2 then
        return {kind = 'nudge', name = entry.name, delta = -1}
    end

    return {kind = 'nudge', name = entry.name, delta = 1}
end

-- Windower mouse events: 0 move, 1 left down, 2 left up.
--
-- Acting on release rather than press, and only when press and release land on
-- the same control, means a click that drifts off is cancelled the way a real
-- button behaves. Returning true swallows the click so it does not reach the
-- game underneath -- but only for clicks that landed on a control, so a press
-- anywhere else falls through to the library and drags the panel.
function M.mouse(kind, x, y)
    if not visible then
        return false
    end

    if kind == 1 then
        pressed = control_at(x, y)

        if debugging and report then
            local origin_x, origin_y = box:pos()
            report(('click %d,%d -> panel %d,%d, width %s, row %s, hit %s')
                :format(x, y, x - origin_x, y - origin_y,
                    tostring(measured.width), tostring(measured.row),
                    pressed and (pressed.kind .. ' ' .. tostring(pressed.name))
                    or 'nothing'))
        end

        return pressed ~= nil
    end

    if kind == 0 then
        -- Backstop for the drag event, in case a move lands between callbacks.
        sync_overlay()
        return pressed ~= nil
    end

    if kind ~= 2 then
        return false
    end

    sync_overlay()

    local was = pressed
    pressed = nil
    if not was then
        return false
    end

    local now = control_at(x, y)
    if not now or now.kind ~= was.kind or now.name ~= was.name then
        return true
    end

    if now.kind == 'close' then
        visible = false
    elseif now.kind == 'toggle' then
        config.nudge(now.name, 1)
    elseif now.delta == was.delta then
        config.nudge(now.name, now.delta)
    end

    render()
    config.save()
    if on_change then
        on_change()
    end

    return true
end

function M.move(x, y)
    if not box then
        return
    end

    box:pos(x, y)
    sync_overlay()
    config.save()
end

-- Bold on the title and the section headings only. Windower has no inline
-- bold, so this switches the overlay layer on or off.
function M.bold(on)
    local set = settings()
    if on == nil then
        set.ui_bold_titles = not (set.ui_bold_titles ~= false)
    else
        set.ui_bold_titles = on and true or false
    end

    render()
    config.save()
    return set.ui_bold_titles
end

function M.debug(reporter)
    debugging = not debugging
    report = reporter
    return debugging
end

-- Copy the panel settings, minus the background, for the overlay: two opaque
-- backgrounds stacked would darken the panel and the upper one would cover the
-- rows beneath it.
local function overlay_settings(d)
    return {
        pos = {x = d.pos.x, y = d.pos.y},
        text = {font = d.text.font, size = d.text.size, alpha = d.text.alpha,
            red = d.text.red, green = d.text.green, blue = d.text.blue},
        bg = {alpha = 0, red = 0, green = 0, blue = 0, visible = false},
        flags = {draggable = false, bold = true},
        padding = d.padding,
    }
end

function M.init(settings_module, changed)
    config = settings_module
    on_change = changed

    -- The third argument is what makes dragging persist: on release the library
    -- saves the settings root for us, so there is no drag handling and no
    -- position bookkeeping here at all.
    local d = config.settings.display
    box = texts.new('', d, config.settings)

    -- Given a settings root, texts.new registers its apply_settings as a reload
    -- callback *instead of* calling it, and config.register only stores the
    -- callback -- it does not invoke it. So nothing applies until the next
    -- login, and the primitive renders with no font, size or position at all.
    -- Applying it here is what fills that gap.
    box:pos(d.pos.x, d.pos.y)
    box:font(d.text.font)
    box:size(d.text.size)
    box:color(d.text.red, d.text.green, d.text.blue)
    box:alpha(d.text.alpha)
    box:bg_color(d.bg.red, d.bg.green, d.bg.blue)
    box:bg_alpha(d.bg.alpha)
    box:bg_visible(d.bg.visible)
    box:pad(d.padding)
    box:bold(false)
    box:italic(d.flags.italic)
    box:draggable(d.flags.draggable)

    -- The bold layer. Not draggable and not saved: it is not a panel in its own
    -- right, only the emphasised rows of this one.
    local o = overlay_settings(d)
    overlay = texts.new('', o)
    overlay:pos(o.pos.x, o.pos.y)
    overlay:font(o.text.font)
    overlay:size(o.text.size)
    overlay:color(o.text.red, o.text.green, o.text.blue)
    overlay:alpha(o.text.alpha)
    overlay:bg_visible(false)
    overlay:pad(o.padding)
    overlay:bold(true)
    overlay:draggable(false)

    -- Follow the base panel whenever the library drags it.
    box:register_event('drag', function()
        sync_overlay()
    end)

    render()
end

return M
