_addon.name = 'TargetLines'
_addon.author = "ported from Jyouya's Ashita addon"
_addon.version = '0.3.0'
_addon.commands = {'tlines', 'targetlines'}

-- FFXII style target lines. The Lua side tracks who is acting on whom and
-- decides how much of each arc should be showing; the module in libs/ does the
-- drawing, inside the game's own 3D scene so the arcs are occluded by the world.

local addon_path = windower.addon_path:gsub('\\', '/')
package.cpath = package.cpath .. ';' .. addon_path .. '/libs/?.dll'

local loaded, load_error = pcall(require, '_TargetLines')

local tracker = require('tracker')
local arcs = tracker.arcs
local TIMEOUTS = tracker.timeouts

-- Ashita's palette, from targetlines.lua.
local COLOURS = {
    player = 0xFF0088FF,           -- you or an ally, onto a monster
    enemy = 0xFFFF1133,            -- a monster, onto you
    player_friendly = 0xFF00FF66,  -- a cure or buff between allies
    enemy_friendly = 0xFFFF8800,   -- a monster buffing another monster
}

local enabled = true

-- 'all', 'alliance' or 'party'. Ashita's filter, same meanings.
local filter = 'all'

-- Draw a line to whatever you currently have targeted, whether or not anything
-- is happening. Not in Ashita, but it makes the addon useful out of combat.
local show_target = true

local function chat(message)
    windower.add_to_chat(207, 'TargetLines: ' .. message)
end

-- The module treats this sentinel as "put this setting back to its default".
-- It sits outside every valid range, so no real value can collide with it.
local RESTORE_DEFAULT = -999

-- Which end of a line a chest value applies to. Must match ChestScope in the
-- module.
local CHEST_BOTH = 0
local CHEST_SELF = 1
local CHEST_OTHER = 2

-- Read a numeric argument, mapping the words "default" and "reset" onto the
-- sentinel. Returns nil when there is no argument, which means "just report".
local function setting_arg(word)
    if not word then
        return nil
    end

    local lowered = word:lower()
    if lowered == 'default' or lowered == 'reset' or lowered == 'defaults' then
        return RESTORE_DEFAULT
    end

    return tonumber(word)
end

-- Call a module setter with a value, or with nothing at all so it only reports.
local function apply(setter, word)
    local value = setting_arg(word)
    if value then
        return setter(value)
    end

    return setter()
end

local function available()
    return loaded and _TargetLines ~= nil
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

-- The index is what lets the module read the live render position; the
-- coordinates are only a fallback for entities it cannot resolve itself.
local function submit(src_index, dst_index, colour, progress, reverse)
    local src = mob_by_index(src_index)
    local dst = mob_by_index(dst_index)
    if not src or not dst then
        return
    end

    _TargetLines.add(
        src_index, src.x or 0, src.y or 0, src.z or 0,
        dst_index, dst.x or 0, dst.y or 0, dst.z or 0,
        COLOURS[colour] or COLOURS.player,
        progress,
        reverse and 1 or 0)
end

-- The set of entity indices the filter cares about: party or alliance members,
-- plus whatever they are currently engaged with, so an incoming line from the
-- mob your tank is holding still shows.
local function relevant_indices()
    local set = {}
    local ok, party = pcall(windower.ffxi.get_party)
    if not ok or not party then
        return nil
    end

    local slots = filter == 'party'
        and {'p0', 'p1', 'p2', 'p3', 'p4', 'p5'}
        or {'p0', 'p1', 'p2', 'p3', 'p4', 'p5',
            'a10', 'a11', 'a12', 'a13', 'a14', 'a15',
            'a20', 'a21', 'a22', 'a23', 'a24', 'a25'}

    for _, slot in ipairs(slots) do
        local member = party[slot]
        local mob = member and member.mob
        if mob and mob.index then
            set[mob.index] = true
            if mob.target_index and mob.target_index ~= 0 then
                set[mob.target_index] = true
            end
        end
    end

    return set
end

-- How much of the arc should be showing, and which end it is growing from.
--
-- Ashita's three phases, unchanged:
--   fresh      grows out from the actor over half a second
--   sustained  a player line held on one target for 2.5s retracts and goes,
--              so a long fight does not leave a permanent beam
--   expiring   retracts back into the target over the last half second
--
-- Retracting arcs are drawn from the target end, which is what `reverse` says.
local function phase(arc, now)
    local timeout = TIMEOUTS[arc.colour]
    local age = now - arc.clock
    local held = arc.first_clock and (now - arc.first_clock)

    -- Ashita retires a player line 2.5s after it first appeared, retracting it
    -- over the following half second. This always applies: it is the fade that
    -- follows the orb in, and losing it makes the line a static beam.
    --
    -- Persistence in repeat mode comes from the tracker replaying the arc on
    -- each attack instead, so the line pulses with the swing rhythm.
    if arc.colour == 'player' and held and held > 2.5 then
        return math.max((3 - held) * 2, 0), true
    end

    if age > timeout - 0.5 then
        return math.min(1 - (0.5 - math.min(timeout - age, 1)) * 2, 1), true
    end

    return math.min(1 - (0.5 - math.min(age, 1)) * 2, 1), false
end

windower.register_event('load', function()
    if not available() then
        chat('the module failed to load: ' .. tostring(load_error))
        return
    end

    chat(_TargetLines.start())
end)

windower.register_event('unload', function()
    if available() then
        _TargetLines.clear()
        -- Leave the shared scene hook, rather than just stopping. The module
        -- repeats this from DllMain as a backstop, but doing it here is what
        -- allows it to wait for an in-flight frame first.
        _TargetLines.release()
    end
end)

windower.register_event('addon command', function(command, ...)
    if not available() then
        chat('the module failed to load: ' .. tostring(load_error))
        return
    end

    local args = {...}
    command = command and command:lower() or 'status'

    if command == 'on' then
        enabled = true
        _TargetLines.start()
        chat('enabled')
    elseif command == 'off' then
        enabled = false
        _TargetLines.clear()
        _TargetLines.stop()
        chat('disabled')
    elseif command == 'filter' then
        local want = args[1] and args[1]:lower()
        if want == 'all' or want == 'alliance' or want == 'party' then
            filter = want
        end
        chat('filter: ' .. filter)
    elseif command == 'target' then
        show_target = not show_target
        chat('line to current target: ' .. (show_target and 'on' or 'off'))
    elseif command == 'attacks' then
        local want = args[1] and args[1]:lower()
        if want == 'persistent' then
            want = 'repeat'
        end

        local mode = tracker.mode(want)
        local described = {
            first = 'one line per engagement',
            ['repeat'] = 'persistent while the attacks keep coming',
            off = 'no auto-attack lines, abilities and spells only',
        }
        chat(('auto-attacks: %s - %s'):format(mode, described[mode]))
    elseif command == 'arc' then
        chat(_TargetLines.arc())
    elseif command == 'reset' then
        chat(_TargetLines.reset())
    elseif command == 'arch' then
        chat(apply(_TargetLines.arch, args[1]))
    elseif command == 'bow' then
        chat(apply(_TargetLines.bow, args[1]))
    elseif command == 'orb' then
        chat(apply(_TargetLines.orb, args[1]))
    elseif command == 'anchor' then
        chat(_TargetLines.anchor())
    elseif command == 'chest' then
        -- //tlines chest [me|target] [value|default]
        -- With no scope word the value applies to both ends.
        local scope, word = CHEST_BOTH, args[1]
        local first = args[1] and args[1]:lower()
        if first == 'me' or first == 'self' or first == 'player' then
            scope, word = CHEST_SELF, args[2]
        elseif first == 'target' or first == 'tgt' or first == 'other' then
            scope, word = CHEST_OTHER, args[2]
        end

        local value = setting_arg(word)
        if value then
            chat(_TargetLines.chest(scope, value))
        else
            chat(_TargetLines.chest())
        end
    elseif command == 'bones' then
        chat(apply(_TargetLines.bones, args[1]))
    elseif command == 'bone' then
        chat(apply(_TargetLines.bone, args[1]))
    elseif command == 'depth' then
        chat(_TargetLines.depth())
    elseif command == 'lift' then
        -- apply() passes nothing when there is no argument. A nil argument
        -- would still reach the module as 0, silently flattening the lift
        -- instead of reporting it.
        chat(apply(_TargetLines.lift, args[1]))
    elseif command == 'width' then
        chat(apply(_TargetLines.width, args[1]))
    elseif command == 'scan' then
        chat(_TargetLines.scan())
    elseif command == 'probe' then
        -- No argument probes your target; "me" probes your own model.
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
        end
    elseif command == 'help' then
        chat('//tlines on | off          start or stop drawing')
        chat('//tlines filter <mode>     all / alliance / party')
        chat('//tlines attacks <mode>    first / repeat / off, for auto-attacks')
        chat('//tlines target            toggle the line to your current target')
        chat('//tlines arc               toggle curved arc vs straight line')
        chat('//tlines arch <0-2>        arc rise as a fraction of the distance')
        chat('//tlines bow <degrees>     sideways lean of the arc')
        chat('//tlines orb <pixels>      size of the travelling dot')
        chat('//tlines anchor            cycle model / bone / nameplate / entity')
        chat('//tlines chest [me|target] <0-1>   attach height on the model')
        chat('//tlines bones <n>         bones used to measure model height')
        chat('//tlines depth             world occlusion vs always on top')
        chat('//tlines lift <yalms>      extra height, every anchor mode')
        chat('//tlines width <pixels>    line thickness')
        chat('//tlines probe [index|me]  survey the anchors and bones')
        chat('//tlines reset             put every setting back to default')
        chat('any setting also takes "default", e.g. //tlines chest default')
    else
        chat(_TargetLines.status())
        chat(('filter %s, auto-attacks %s, target line %s, %d arc(s) tracked')
            :format(filter, tracker.mode(), show_target and 'on' or 'off', (function()
                local n = 0
                for _ in pairs(arcs) do n = n + 1 end
                return n
            end)()))
    end
end)

windower.register_event('prerender', function()
    if not available() or not enabled then
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
    _TargetLines.player(self_mob and self_mob.index or 0)

    local now = os.clock()
    local wanted = filter ~= 'all' and relevant_indices() or nil

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

    if show_target then
        local ok, me = pcall(windower.ffxi.get_mob_by_target, 'me')
        local ok2, target = pcall(windower.ffxi.get_mob_by_target, 't')
        if ok and ok2 and me and target and me.index ~= target.index
            and not arcs[me.index] then
            submit(me.index, target.index,
                target.is_npc and 'player' or 'player_friendly', 1, false)
        end
    end
end)
