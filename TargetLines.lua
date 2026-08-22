_addon.name = 'TargetLines'
_addon.author = "ported from Jyouya's Ashita addon"
_addon.version = '0.5.0'
_addon.commands = {'tlines', 'targetlines'}

-- FFXII style target lines. Lua tracks who is acting on whom and decides what
-- should be showing; the module in libs/ does the drawing, inside the game's
-- own 3D scene so the arcs and rings are occluded by the world.

local addon_path = windower.addon_path:gsub('\\', '/')
package.cpath = package.cpath .. ';' .. addon_path .. '/libs/?.dll'

local loaded, load_error = pcall(require, '_TargetLines')

local tracker = require('tracker')
local config = require('settings')
local ui = require('ui')

local arcs = tracker.arcs
local TIMEOUTS = tracker.timeouts
local set = config.settings

-- Ashita's palette, from targetlines.lua.
local COLOURS = {
    player = 0xFF0088FF,           -- you or an ally, onto a monster
    enemy = 0xFFFF1133,            -- a monster, onto you
    player_friendly = 0xFF00FF66,  -- a cure or buff between allies
    enemy_friendly = 0xFFFF8800,   -- a monster buffing another monster
}

-- Seconds for an area-of-effect wavefront to reach its full radius.
local AOE_SWEEP = 0.45

local function chat(message)
    windower.add_to_chat(207, 'TargetLines: ' .. message)
end

local function available()
    return loaded and _TargetLines ~= nil
end

-- Push every setting the module needs into it. Called whenever anything
-- changes, which keeps the module a pure renderer with no state to drift.
local function apply()
    if not available() then
        return
    end

    _TargetLines.width(set.width)
    _TargetLines.arch(set.arch)
    _TargetLines.bow(set.bow)
    _TargetLines.orb(set.orb)
    _TargetLines.lift(set.lift)
    _TargetLines.chest(1, set.chest_me)
    _TargetLines.chest(2, set.chest_target)

    -- Both of these also cycle when called bare, for the chat commands; given
    -- a value they set directly, which is what lets Lua stay authoritative.
    _TargetLines.arc(set.curved and 0 or 1)
    _TargetLines.depth(set.depth and 0 or 1)
    _TargetLines.glow(set.glow and 1 or 0)

    tracker.mode(set.attacks)
end

local function mob_by_index(index)
    if not index then
        return nil
    end

    local ok, mob = pcall(windower.ffxi.get_mob_by_index, index)
    if not ok then
        return nil
    end

    return mob
end

-- Roles come from the tracker, which is the single source of truth: the same
-- answer drives line colour, visibility and opacity. Keeping a second copy here
-- is what let a trust be filtered one way and coloured another.
local ROLE_ORDER = {'me', 'party', 'trust', 'pet', 'alliance', 'others', 'object', 'enemy'}

local function show_entity(mob)
    local role = tracker.role_of(mob)
    return role == nil or set['show_' .. role] ~= false
end

-- Opacity is grouped more coarsely than visibility: three dials rather than
-- seven, because in practice you want your own lines readable, everyone else's
-- quieter, and incoming attacks somewhere between.
local OPACITY_GROUP = {
    me = 'opacity_me',
    party = 'opacity_ally',
    trust = 'opacity_ally',
    pet = 'opacity_ally',
    alliance = 'opacity_ally',
    others = 'opacity_ally',
    object = 'opacity_ally',
    enemy = 'opacity_enemy',
}

local function opacity_for(mob)
    local key = OPACITY_GROUP[tracker.role_of(mob)]
    return key and tonumber(set[key]) or 1
end

-- Fade an ARGB colour by a 0..1 factor.
local function faded(colour, alpha)
    if alpha <= 0 then
        return nil
    end

    local a = math.floor(((colour / 0x1000000) % 0x100) * math.min(alpha, 1))
    if a < 1 then
        return nil
    end

    return (a * 0x1000000) + (colour % 0x1000000)
end

-- The index is what lets the module read the live render position; the
-- coordinates are only a fallback for entities it cannot resolve itself.
local function submit(src_index, dst_index, colour, progress, reverse)
    local src = mob_by_index(src_index)
    local dst = mob_by_index(dst_index)
    if not src or not dst then
        return
    end

    -- Both ends must be wanted. Hiding trusts should hide the line a trust
    -- draws onto a mob as well as any line drawn onto the trust.
    if not show_entity(src) or not show_entity(dst) then
        return
    end

    -- Faded by whichever end is doing the acting, so incoming attacks can
    -- be quieted without dimming your own lines.
    local tint = faded(COLOURS[colour] or COLOURS.player, opacity_for(src))
    if not tint then
        return
    end

    -- Incoming lines get their own lean. The two still swing to opposite
    -- sides on their own, because the axes are opposite; this only sets
    -- how far each one goes.
    local incoming = colour == 'enemy' or colour == 'enemy_friendly'
    local lean = incoming and set.bow_enemy or set.bow

    _TargetLines.add(
        src_index, src.x or 0, src.y or 0, src.z or 0,
        dst_index, dst.x or 0, dst.y or 0, dst.z or 0,
        tint,
        progress,
        reverse and 1 or 0,
        lean)
end

-- The set of entity indices the party filter cares about: party or alliance
-- members, plus whatever they are engaged with, so an incoming line from the
-- mob your tank is holding still shows.
local function relevant_indices()
    local wanted = {}
    local ok, party = pcall(windower.ffxi.get_party)
    if not ok or not party then
        return nil
    end

    local slots = set.filter == 'party'
        and {'p0', 'p1', 'p2', 'p3', 'p4', 'p5'}
        or {'p0', 'p1', 'p2', 'p3', 'p4', 'p5',
            'a10', 'a11', 'a12', 'a13', 'a14', 'a15',
            'a20', 'a21', 'a22', 'a23', 'a24', 'a25'}

    for _, slot in ipairs(slots) do
        local member = party[slot]
        local mob = member and member.mob
        if mob and mob.index then
            wanted[mob.index] = true
            if mob.target_index and mob.target_index ~= 0 then
                wanted[mob.target_index] = true
            end
        end
    end

    return wanted
end

-- How much of the arc should be showing, and which end it is growing from.
--
-- Ashita's three phases, unchanged:
--   fresh      grows out from the actor over half a second
--   sustained  a player line held on one target for 2.5s retracts and goes,
--              so a long fight does not leave a permanent beam
--   expiring   retracts back into the target over the last half second
--
-- Retracting arcs are drawn from the target end, which is what reverse says.
local function phase(arc, now)
    local timeout = TIMEOUTS[arc.colour]
    local age = now - arc.clock
    local held = arc.first_clock and (now - arc.first_clock)

    if arc.colour == 'player' and held and held > 2.5 then
        return math.max((3 - held) * 2, 0), true
    end

    if age > timeout - 0.5 then
        return math.min(1 - (0.5 - math.min(timeout - age, 1)) * 2, 1), true
    end

    return math.min(1 - (0.5 - math.min(age, 1)) * 2, 1), false
end

-- Area-of-effect bursts.
--
-- A wavefront expands to the radius the action actually reached, and each
-- entity it catches gets a comet as the front passes over it. That ordering is
-- the point: the sweep shows the extent, the comets show who was in it.
local function draw_bursts(now)
    if not set.aoe then
        return
    end

    for key, burst in pairs(tracker.bursts) do
        local age = now - burst.clock
        if age > set.aoe_hold then
            tracker.bursts[key] = nil
        else
            local sweep = math.min(age / AOE_SWEEP, 1)
            local base = COLOURS[burst.colour] or COLOURS.player

            -- Something landing on you is worth noticing; your own is
            -- confirmation. So incoming bursts draw heavier.
            local incoming = burst.colour == 'enemy'
                or burst.colour == 'enemy_friendly'
            local weight = incoming and (tonumber(set.aoe_enemy_scale) or 1) or 1

            -- The front holds full brightness while it travels and only fades
            -- once it has arrived, so the eye follows the expansion outward
            -- rather than watching the whole thing dim.
            local settle = math.max(set.aoe_hold - AOE_SWEEP, 0.01)
            local front = sweep < 1 and 1
                or math.max(1 - (age - AOE_SWEEP) / settle, 0)

            local colour = faded(base, front)
            if colour then
                _TargetLines.ring(
                    burst.centre_index or 0,
                    burst.centre_x, burst.centre_y, burst.centre_z,
                    burst.radius * sweep,
                    colour,
                    0, set.aoe_lift, 0, 0, set.aoe_sweepwidth * weight)
            end

            -- The head angle advances with time, which is what makes the
            -- comets orbit.
            local reached = burst.radius * sweep
            local head = age * set.aoe_orbit * 6.2831853

            for _, target in ipairs(burst.targets) do
                local mob = mob_by_index(target.index)
                if mob and show_entity(mob) then
                    local dx = (tonumber(mob.x) or 0) - burst.centre_x
                    local dy = (tonumber(mob.y) or 0) - burst.centre_y
                    local distance = math.sqrt(dx * dx + dy * dy)

                    if reached >= distance then
                        -- Hold full brightness, then fade. Decaying from
                        -- the instant it appears made these read as a
                        -- flicker rather than a mark.
                        local hold = set.aoe_hold * (tonumber(set.aoe_fade) or 0.55)
                        local alpha = 1
                        if age > hold then
                            alpha = math.max(
                                1 - (age - hold) / math.max(set.aoe_hold - hold, 0.01), 0)
                        end

                        local hit = faded(base, alpha)
                        if hit then
                            _TargetLines.ring(target.index,
                                mob.x or 0, mob.y or 0, mob.z or 0,
                                set.aoe_radius, hit,
                                1, set.aoe_chest,
                                head, set.aoe_tail, set.aoe_width * weight)
                        end
                    end
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Events
-- ---------------------------------------------------------------------------

windower.register_event('load', function()
    if not available() then
        chat('the module failed to load: ' .. tostring(load_error))
        return
    end

    chat(_TargetLines.start())
    apply()
    ui.init(config, apply)
end)

windower.register_event('unload', function()
    ui.hide()

    if available() then
        _TargetLines.clear()
        -- Leave the shared scene hook, rather than just stopping. The module
        -- repeats this from DllMain as a backstop, but doing it here is what
        -- allows it to wait for an in-flight frame first.
        _TargetLines.release()
    end
end)

windower.register_event('mouse', function(kind, x, y)
    return ui.mouse(kind, x, y)
end)

windower.register_event('prerender', function()
    if not available() or not set.enabled then
        return
    end

    _TargetLines.clear()

    -- Suppress while zoning; the entity array is being rebuilt underneath us.
    local player = windower.ffxi.get_player()
    if player and player.status == 4 then
        return
    end

    -- Tell the module which entity is us, so "me" can be told apart from
    -- everyone else at either end of a line. Refreshed every frame because the
    -- index changes on a zone.
    local self_mob = windower.ffxi.get_mob_by_target('me')
    local self_index = self_mob and self_mob.index or 0
    tracker.set_player(self_index)
    _TargetLines.player(self_index)

    local now = os.clock()
    local wanted = set.filter ~= 'all' and relevant_indices() or nil

    for src_index, arc in pairs(arcs) do
        if now - arc.clock > TIMEOUTS[arc.colour] then
            arcs[src_index] = nil
            -- The engagement is over, so let a fresh one draw again rather
            -- than staying suppressed for the rest of the session.
            tracker.forget(src_index)
        elseif not wanted or wanted[src_index] or wanted[arc.dst] then
            local progress, reverse = phase(arc, now)
            if progress > 0 then
                submit(src_index, arc.dst, arc.colour, progress, reverse)
            end
        end
    end

    draw_bursts(now)

    if set.target_line then
        local ok, me = pcall(windower.ffxi.get_mob_by_target, 'me')
        local ok2, target = pcall(windower.ffxi.get_mob_by_target, 't')
        if ok and ok2 and me and target and me.index ~= target.index
            and not arcs[me.index] then
            submit(me.index, target.index,
                target.is_npc and 'player' or 'player_friendly', 1, false)
        end
    end
end)

-- ---------------------------------------------------------------------------
-- Commands
--
-- Everything the panel can do is reachable by typing too, and both go through
-- the same settings model, so neither can drift from the other.
-- ---------------------------------------------------------------------------

-- Settings whose command name differs from the setting name, or which take a
-- scope word. Everything else is matched directly against the settings model.
local ALIASES = {
    arc = 'curved',
    aoerings = 'aoe',
    target = 'target_line',
    curve = 'curved',
}

local function report(name)
    local row = config.row(name)
    local fallback = config.defaults[name]
    if row and row.type == 'toggle' then
        fallback = fallback and 'ON' or 'OFF'
    end

    chat(('%s: %s (default %s)'):format(
        row and row.label or name,
        config.display(name),
        tostring(fallback)))
end

local function assign(name, word)
    if not word then
        report(name)
        return
    end

    local row = config.row(name)
    local lowered = word:lower()

    if lowered == 'default' or lowered == 'reset' then
        config.set(name, config.defaults[name])
    elseif row and row.type == 'toggle' then
        if lowered == 'on' then
            config.set(name, true)
        elseif lowered == 'off' then
            config.set(name, false)
        else
            config.nudge(name, 1)
        end
    elseif not config.set(name, tonumber(word) or lowered) then
        chat(('%s: %s is not valid'):format(name, word))
        return
    end

    apply()
    config.save()
    report(name)
end

windower.register_event('addon command', function(command, ...)
    if not available() then
        chat('the module failed to load: ' .. tostring(load_error))
        return
    end

    local args = {...}
    command = command and command:lower() or 'status'
    command = ALIASES[command] or command

    if command == 'config' or command == 'menu' then
        chat('config panel: ' .. (ui.toggle() and 'shown' or 'hidden'))
    elseif command == 'on' then
        config.set('enabled', true)
        _TargetLines.start()
        apply()
        config.save()
        chat('enabled')
    elseif command == 'off' then
        config.set('enabled', false)
        _TargetLines.clear()
        _TargetLines.stop()
        config.save()
        chat('disabled')
    elseif command == 'reset' then
        config.reset()
        apply()
        config.save()
        ui.render()
        chat('every setting back to default')
    elseif command == 'bold' then
        -- Windower has no inline bold, so this is the whole panel.
        chat('panel bold: ' .. (ui.bold() and 'on' or 'off'))
    elseif command == 'aoedebug' then
        chat('aoe centring report: '
            .. (tracker.aoe_debug(chat) and 'on' or 'off'))
    elseif command == 'uidebug' then
        chat('click diagnostics: ' .. (ui.debug(chat) and 'on' or 'off'))
    elseif command == 'ui' then
        -- Row height and character width in pixels, used only to map a
        -- click back to a control. If clicks land on the wrong row or
        -- miss the arrows, these are what to correct.
        local first = args[1] and args[1]:lower()
        if first == 'precise' or first == 'zones' then
            set.ui_precise = first == 'precise'
            config.save()
            chat(('click targets: %s'):format(set.ui_precise
                and 'exact brackets' or 'half the control column each'))
        elseif first == 'auto' then
            set.ui_auto = true
            config.save()
            chat('panel grid: measured from the rendered font')
        else
            local rows = tonumber(args[1])
            local chars = tonumber(args[2])
            if rows then
                set.ui_row = rows
            end
            if chars then
                set.ui_char = chars
            end
            -- Giving numbers means taking manual control; ui auto hands
            -- it back to the measurement.
            set.ui_auto = not (rows or chars)
            config.save()
            chat(('panel grid: %s, %.2f px rows, %.2f px characters')
                :format(set.ui_auto and 'measured' or 'manual',
                    set.ui_row, set.ui_char))
        end
    elseif command == 'move' then
        local x = tonumber(args[1])
        local y = tonumber(args[2])
        if x and y then
            ui.move(x, y)
            chat(('config panel moved to %d, %d'):format(x, y))
        else
            chat('usage: //tlines move <x> <y>')
        end
    elseif command == 'show' then
        -- //tlines show <role> [on|off], or with no role, list them all.
        local role = args[1] and args[1]:lower()
        if role == 'all' or role == 'none' then
            for _, name in ipairs(ROLE_ORDER) do
                config.set('show_' .. name, role == 'all')
            end
            config.save()
            ui.render()
            chat('all roles: ' .. (role == 'all' and 'on' or 'off'))
        elseif role and config.row('show_' .. role) then
            assign('show_' .. role, args[2] or 'toggle')
            ui.render()
        else
            local parts = {}
            for _, name in ipairs(ROLE_ORDER) do
                parts[#parts + 1] = ('%s %s'):format(name, config.display('show_' .. name))
            end
            chat(table.concat(parts, ', '))
        end
    elseif command == 'aoe' then
        -- //tlines aoe            toggles the rings
        -- //tlines aoe <name> <n> adjusts one of the aoe settings
        local name = args[1] and ('aoe_' .. args[1]:lower())
        if name and config.row(name) then
            assign(name, args[2])
            ui.render()
        elseif args[1] then
            chat('aoe settings: hold, lift, sweepwidth, radius, chest, orbit, tail, width')
        else
            assign('aoe', 'toggle')
            ui.render()
        end
    elseif command == 'chest' then
        -- //tlines chest [me|target] <value>
        local first = args[1] and args[1]:lower()
        if first == 'me' or first == 'self' or first == 'player' then
            assign('chest_me', args[2])
        elseif first == 'target' or first == 'tgt' or first == 'other' then
            assign('chest_target', args[2])
        elseif args[1] then
            assign('chest_me', args[1])
            assign('chest_target', args[1])
        else
            report('chest_me')
            report('chest_target')
        end
        ui.render()
    elseif config.row(command) then
        -- Anything named the same as a setting is handled generically.
        assign(command, args[1])
        ui.render()
    elseif command == 'anchor' then
        chat(_TargetLines.anchor())
    elseif command == 'bones' then
        chat(args[1] and _TargetLines.bones(tonumber(args[1])) or _TargetLines.bones())
    elseif command == 'bone' then
        chat(args[1] and _TargetLines.bone(tonumber(args[1])) or _TargetLines.bone())
    elseif command == 'perf' then
        -- //tlines perf          report
        -- //tlines perf reset    clear the averages
        -- //tlines perf on|off   culling and adaptive sampling
        local word = args[1] and args[1]:lower()
        if word == 'reset' then
            chat(_TargetLines.perf(-1))
        elseif word == 'on' or word == 'off' then
            chat(_TargetLines.perf(word == 'on' and 1 or 0))
        else
            chat(_TargetLines.perf())
        end
    elseif command == 'scan' then
        chat(_TargetLines.scan())
    elseif command == 'probe' then
        local index = tonumber(args[1])
        if not index then
            local who = (args[1] and args[1]:lower() == 'me') and 'me' or 't'
            local ok, mob = pcall(windower.ffxi.get_mob_by_target, who)
            if ok and mob then
                index = mob.index
            end
        end

        if not index then
            chat('probe: target something, or pass an index, or "me"')
        else
            chat(_TargetLines.probe(index))

            -- How this entity is classified, and whether that is why it
            -- is or is not drawing. Guessing at spawn_type is what made
            -- lines vanish once the classifier got stricter.
            local mob = mob_by_index(index)
            if mob then
                local role = tracker.role_of(mob)
                chat(('role %s, shown %s | npc %s, spawn_type %s, party %s, '
                    .. 'alliance %s, charmed %s')
                    :format(tostring(role),
                        tostring(show_entity(mob)),
                        tostring(mob.is_npc),
                        tostring(mob.spawn_type),
                        tostring(mob.in_party),
                        tostring(mob.in_alliance),
                        tostring(mob.charmed)))
            end
        end
    elseif command == 'help' then
        chat('//tlines config            open the settings panel')
        chat('//tlines on | off          start or stop drawing')
        chat('//tlines reset             every setting back to default')
        chat('//tlines show <role>       me party trust pet alliance others enemy')
        chat('//tlines aoe [name] [n]    area-of-effect rings')
        chat('//tlines chest [me|target] <0-1>   where lines attach')
        chat('//tlines <setting> [value] any setting by name, see the panel')
        chat('//tlines probe [index|me]  survey the anchors and bones')
        chat('//tlines move <x> <y>      reposition the panel')
        chat('//tlines ui <rows> <chars> fix click alignment, in pixels')
        chat('//tlines ui precise | zones   click target style')
        chat('//tlines bold             bold the whole panel')
    else
        chat(_TargetLines.status())
        chat(('filter %s, auto-attacks %s, target line %s, %d arc(s) tracked')
            :format(set.filter, set.attacks,
                config.display('target_line'), (function()
                    local n = 0
                    for _ in pairs(arcs) do n = n + 1 end
                    return n
                end)()))
    end
end)
