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

    -- An action can name several targets; each one gets its own arc, so an AoE
    -- fans out the way it does in FFXII.
    for i = 1, count do
        local target = mob_by_id(packet[('Target %u ID'):format(i)])
        if target and target.index and target.index ~= actor.index then
            local colour = classify(actor, target)
            local clock = now
            local first_clock = now

            local existing = arcs[actor.index]
            if existing then
                if category == 4 and colour == 'player_friendly' then
                    -- Category 4 is a completed spell. Friendly casts expire
                    -- almost at once so a cure flashes rather than lingering.
                    clock = now - TIMEOUTS.player_friendly + 0.5
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
end)

return {
    arcs = arcs,
    timeouts = TIMEOUTS,
}
