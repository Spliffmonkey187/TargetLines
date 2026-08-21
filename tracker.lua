-- Action tracking, ported from tracker.lua in Jyouya's Ashita addon.
--
-- Watches action packets (0x028) and message packets (0x029) and maintains a
-- table of live arcs keyed on the actor's entity index. Ashita bit-unpacks the
-- action packet by hand; Windower's packets library already parses it, so the
-- field offsets come from there instead.
--
-- Returns the arc table. Each entry:
--   dst         entity index the actor is acting on
--   colour      one of the four line kinds
--   clock       when the action landed, for the timeout
--   first_clock when this actor first started acting on this target

local packets = require('packets')

local arcs = {}

-- Regular attacks: melee and ranged auto-attacks. Everything else -- weapon
-- skills, spells, job abilities, monster TP moves, pet abilities -- counts as a
-- special action and is never suppressed.
local REGULAR_CATEGORIES = {
    [1] = true,  -- melee attack
    [2] = true,  -- ranged attack
}

-- How auto-attacks are drawn:
--   first   one line when an engagement begins, then quiet until it ends.
--           Auto-attacks land every few seconds all fight long, so redrawing
--           on each swing is most of the on-screen clutter.
--   repeat  refresh the line while attacks continue, so it stays up for as
--           long as the fight does.
--   off     no lines for auto-attacks at all; abilities and spells only.
local attack_mode = 'first'

-- When an actor last drew a regular-attack line at a given target, keyed
-- "actor>target". In repeat mode the line is only refreshed once the cooldown
-- has passed, so the grow animation is not restarted on every swing.
local seen_attacks = {}
local REPEAT_COOLDOWN = 3.0

-- Area-of-effect bursts. An action naming several targets gets a ring showing
-- how far it actually reached, plus a small ring on each entity it caught.
--
-- The radius is *inferred* from the distance to the furthest target rather than
-- looked up, so it is correct for every spell and ability in the game with no
-- table to maintain. It measures what landed, which is the useful thing.
local bursts = {}
local burst_sequence = 0

-- Monster TP moves centre on the monster itself.
local MOB_TP_MOVE = 11

-- How long a burst stays on screen, in seconds. Short: it is an event, not a
-- state, and lingering rings would pile up badly in a busy fight.
local BURST_LIFE = 1.4

-- How long each kind of line lingers, in seconds. Combat lines outlast the
-- friendly ones so a fight stays readable between rounds.
local TIMEOUTS = {
    player = 10,
    enemy = 10,
    player_friendly = 5,
    enemy_friendly = 5,
}

-- Death and defeat messages. When one of these lands, the arcs involving that
-- entity are expired early rather than left hanging for the full timeout.
local DEATH_MESSAGES = {
    [6] = true, [20] = true, [97] = true, [113] = true,
    [406] = true, [605] = true, [646] = true,
}

-- Windower's spawn_type for a monster. Ashita tests spawn flag 0x10, which is
-- the same bit.
local SPAWN_MOB = 16

local function mob_by_id(id)
    if not id or id == 0 then
        return nil
    end

    local ok, mob = pcall(windower.ffxi.get_mob_by_id, id)
    if not ok then
        return nil
    end

    return mob
end

-- A monster, as opposed to a player, trust, NPC or a charmed pet fighting for
-- someone. Party and alliance members are never monsters even when charmed.
local function is_mob(mob)
    if not mob or not mob.is_npc then
        return false
    end

    if mob.in_party or mob.in_alliance then
        return false
    end

    return (tonumber(mob.spawn_type) or 0) == SPAWN_MOB
end

-- Pets act on their owner's behalf, so a pet attacking a monster should draw a
-- player-coloured line rather than a monster-coloured one. Windower exposes
-- charm state directly; avatars and wyverns are npcs owned by a party member.
local function is_pet(mob)
    if not mob then
        return false
    end

    return mob.charmed == true or (mob.pet_owner_id ~= nil and mob.pet_owner_id ~= 0)
end

-- Ashita's colour rules, unchanged:
--   monster -> monster   enemy_friendly   (a mob healing or buffing a mob)
--   monster -> anyone    enemy            (a mob attacking you)
--   anyone  -> monster   player           (you attacking a mob)
--   anyone  -> anyone    player_friendly  (a cure, a buff)
local function classify(actor, target)
    if is_mob(actor) and not is_pet(actor) then
        return is_mob(target) and 'enemy_friendly' or 'enemy'
    end

    return is_mob(target) and 'player' or 'player_friendly'
end

-- Whether a regular attack from this actor onto this target should draw.
-- Special actions always draw and never touch the bookkeeping.
local function allow_regular(regular, actor_index, target_index, now)
    if not regular then
        return true
    end

    local key = actor_index .. '>' .. target_index

    if attack_mode == 'first' then
        if seen_attacks[key] then
            return false
        end
    elseif attack_mode == 'repeat' then
        local last = seen_attacks[key]
        if last and now - last < REPEAT_COOLDOWN then
            return false
        end
    end

    seen_attacks[key] = now
    return true
end

-- Called when an arc expires, so the next engagement with the same target
-- draws again rather than staying suppressed for the rest of the session.
local function forget(actor_index)
    local prefix = actor_index .. '>'
    for key in pairs(seen_attacks) do
        if key:sub(1, #prefix) == prefix then
            seen_attacks[key] = nil
        end
    end
end

-- Which entity an area effect radiates from.
--
-- A heuristic, because the packet does not say. Self-buffs and party heals list
-- the caster among their own targets, and monster TP moves go off where the
-- monster stands; anything else is a spell or ability aimed at something, so it
-- centres on what was aimed at.
local function burst_centre(actor, targets, category)
    for _, target in ipairs(targets) do
        if target.index == actor.index then
            return actor
        end
    end

    if category == MOB_TP_MOVE then
        return actor
    end

    return targets[1] and targets[1].mob or actor
end

local function record_burst(actor, targets, category, colour, now)
    if #targets < 2 then
        -- One target is a normal action, not an area effect. Drawing a ring
        -- there would just be a circle the size of the caster's reach.
        return
    end

    local centre = burst_centre(actor, targets, category)
    local cx = tonumber(centre.x) or 0
    local cy = tonumber(centre.y) or 0

    local radius = 0
    for _, target in ipairs(targets) do
        local mob = target.mob
        if mob then
            local dx = (tonumber(mob.x) or 0) - cx
            local dy = (tonumber(mob.y) or 0) - cy
            local distance = math.sqrt(dx * dx + dy * dy)
            if distance > radius then
                radius = distance
            end
        end
    end

    -- Everyone stood on top of the caster: there is no ring worth drawing.
    if radius < 0.5 then
        return
    end

    burst_sequence = burst_sequence + 1
    bursts[burst_sequence] = {
        centre_index = centre.index,
        centre_x = cx,
        centre_y = cy,
        centre_z = tonumber(centre.z) or 0,
        radius = radius,
        colour = colour,
        clock = now,
        targets = targets,
    }
end

local function handle_action(data)
    local packet = packets.parse('incoming', data)
    if not packet then
        return
    end

    local count = tonumber(packet['Target Count']) or 0
    if count < 1 then
        return
    end

    local actor = mob_by_id(packet['Actor'])
    if not actor or not actor.index then
        return
    end

    local category = tonumber(packet['Category']) or 0
    local now = os.clock()
    local regular = REGULAR_CATEGORIES[category] or false

    if regular and attack_mode == 'off' then
        return
    end

    -- Collected for the area-of-effect ring, which needs to see every target at
    -- once to work out how far the action reached.
    local hit = {}
    local burst_colour = nil

    -- An action can name several targets; each one gets its own arc, so an AoE
    -- fans out the way it does in FFXII.
    for i = 1, count do
        local target = mob_by_id(packet[('Target %u ID'):format(i)])
        if target and target.index then
            hit[#hit + 1] = {index = target.index, mob = target}
            burst_colour = burst_colour or classify(actor, target)
        end

        if target and target.index and target.index ~= actor.index
            and allow_regular(regular, actor.index, target.index, now) then
            local colour = classify(actor, target)
            local clock = now
            local first_clock = now

            local existing = arcs[actor.index]
            if existing then
                if category == 4 and colour == 'player_friendly' then
                    -- Category 4 is a completed spell. Friendly casts expire
                    -- almost at once so a cure flashes rather than lingering.
                    clock = now - TIMEOUTS.player_friendly + 0.5
                elseif regular and attack_mode == 'repeat' then
                    -- Deliberately keep both clocks at `now`, so the arc runs
                    -- its whole grow-hold-fade cycle again. That is what makes
                    -- repeat mode persistent: the line keeps coming back with
                    -- the swing rhythm, rather than sitting there as a static
                    -- beam with no animation.
                    clock = now
                    first_clock = now
                elseif existing.dst == target.index
                    and now - existing.clock < TIMEOUTS[colour] then
                    -- Still hitting the same target: keep the original start so
                    -- the line does not restart its grow animation mid-fight.
                    if existing.colour == 'player' then
                        first_clock = existing.first_clock or now
                    end
                    clock = now - 1
                end
            end

            arcs[actor.index] = {
                dst = target.index,
                colour = colour,
                clock = clock,
                first_clock = first_clock,
            }
        end
    end

    -- Regular attacks never produce a ring: a melee swing hits one target, and
    -- an auto-attack burst would be constant noise even if it did not.
    if not regular then
        record_burst(actor, hit, category, burst_colour or 'player', now)
    end
end

local function handle_message(data)
    local packet = packets.parse('incoming', data)
    if not packet then
        return
    end

    if not DEATH_MESSAGES[tonumber(packet['Message']) or 0] then
        return
    end

    local target = tonumber(packet['Target Index'])
    local actor = tonumber(packet['Actor Index'])

    -- Something died. Retract its line, and the line of whoever killed it if
    -- they were still pointing at it.
    local dying = target and arcs[target]
    if not dying then
        return
    end

    local expiry = os.clock() - TIMEOUTS[dying.colour] + 0.5
    dying.clock = expiry

    local killer = actor and arcs[actor]
    if killer and killer.dst == target then
        killer.clock = expiry
    end
end

windower.register_event('incoming chunk', function(id, data)
    if id == 0x028 then
        handle_action(data)
    elseif id == 0x029 then
        handle_message(data)
    end
end)

-- Drop everything on a zone: entity indices are reassigned, so a stale arc
-- would point at whatever now occupies that slot.
windower.register_event('zone change', function()
    for index in pairs(arcs) do
        arcs[index] = nil
    end

    for key in pairs(seen_attacks) do
        seen_attacks[key] = nil
    end

    for key in pairs(bursts) do
        bursts[key] = nil
    end
end)

return {
    arcs = arcs,
    bursts = bursts,
    burst_life = BURST_LIFE,
    timeouts = TIMEOUTS,
    forget = forget,

    mode = function(want)
        if want == 'first' or want == 'repeat' or want == 'off' then
            attack_mode = want
            -- Switching mode starts clean, so a line suppressed under the old
            -- rule is not still suppressed under the new one.
            for key in pairs(seen_attacks) do
                seen_attacks[key] = nil
            end
        end

        return attack_mode
    end,
}
