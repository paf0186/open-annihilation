// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/effect_particles.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace oa::sim::effect_particles;
using oa::FixedVec3;

namespace {
int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Scripted rand() stream: returns the queued values in order, then 0x4000.
struct Script {
    int32_t values[64]{};
    int32_t count{};
    int32_t next{};
    int32_t calls{};
    int32_t grid_height{};
};

int32_t scripted_rand(void* context) {
    auto& s = *static_cast<Script*>(context);
    ++s.calls;
    return s.next < s.count ? s.values[s.next++] : 0x4000;
}

int32_t scripted_grid(void* context, const FixedVec3&) {
    return static_cast<Script*>(context)->grid_height;
}

int32_t scripted_ground(void*, const FixedVec3&) {
    return 0;
}

void queue(Script& s, std::initializer_list<int32_t> values) {
    s.count = 0;
    s.next = 0;
    for (auto v : values)
        s.values[s.count++] = v;
}

EffectHost host_for(Script& s) {
    return {&s, scripted_rand, scripted_grid, scripted_ground};
}

oa::formats::gaf::Sequence sequence_of(size_t frames, uint16_t hold) {
    oa::formats::gaf::Sequence sequence{};
    sequence.frames.resize(frames);
    for (auto& frame : sequence.frames)
        frame.duration = hold;
    return sequence;
}

struct Fixture {
    std::unique_ptr<EffectWorld> world = std::make_unique<EffectWorld>();
    oa::formats::gaf::Sequence smoke_1 = sequence_of(12, 5);
    oa::formats::gaf::Sequence smoke_2 = sequence_of(16, 3);
    oa::formats::gaf::Sequence flamestream = sequence_of(20, 5);
    oa::Game game{};
    Script script{};

    Fixture() {
        world->fx[static_cast<uint32_t>(Fx::smoke_1)] = &smoke_1;
        world->fx[static_cast<uint32_t>(Fx::smoke_2)] = &smoke_2;
        world->fx[static_cast<uint32_t>(Fx::flamestream)] = &flamestream;
        game.tick = 100;
        game.sea_level = 20;
    }

    const Layer& layer(uint16_t index) const { return world->layers[index]; }

    const Emitter& emitter(uint16_t layer_index, uint32_t index = 0) const {
        return layer_emitter(*world, layer_index, index);
    }

    void advance() {
        ++game.tick;
        tick_particles(*world, game, host_for(script));
    }
};

template <class Pool>
const auto& first_record(const Pool& pool, const Emitter& e) {
    return pool.records[e.particles.first];
}

FixedVec3 units(int32_t x, int32_t y, int32_t z) {
    return {x << 16, y << 16, z << 16};
}

void white_smoke_puff_lifecycle() {
    Fixture f;
    f.game.wind_vector = {3, 0, -2};
    f.game.gravity = 0x1000;
    // Cap = 12 frames - 1 = 11; puff limit = 0x4000 * (11 - 2) / 0x8000 + 2 = 6.
    queue(f.script, {0x4000});
    spawn_white_smoke(*f.world, f.game, host_for(f.script), units(10, 30, 40), layer_smoke);
    const auto& e = f.emitter(layer_smoke);
    check(f.layer(layer_smoke).count == 1, "white smoke queues one emitter");
    check(
        e.kind == EmitterKind::smoke && e.frame_cap == 11 && e.hold == 7 && e.interval == 1,
        "white smoke takes the whole entry, the default hold and a one-tick interval"
    );
    check(
        e.deadline == 100 && e.next_spawn == 101,
        "white smoke ends at once and would spawn next tick"
    );
    const auto& puff = first_record(f.world->smoke, e);
    check(
        puff.frame_limit == 6 && puff.frame == 0 && puff.hold == 7 && puff.remaining == 7,
        "the first puff holds frame 0 for seven ticks and shows six frames"
    );
    check(puff.sequence == &f.smoke_1, "white smoke draws smoke 1");

    // Per tick: x += wind.x * 8, y += gravity * 4, z += wind.z * 8.
    // The emitter does not spawn again: next 101 > deadline 100.
    f.advance();
    check(
        puff.position.x == (10 << 16) + 24 && puff.position.y == (30 << 16) + 0x4000 &&
            puff.position.z == (40 << 16) - 16,
        "a puff drifts with the wind and rises by four gravities"
    );
    check(
        puff.remaining == 6 && e.particles.count == 1,
        "one tick of hold is spent and no new puff spawned"
    );
    // Seven ticks in, frame 1 shows for 7 / 2 + rand * 3 / 0x8000 = 3 + 1 ticks.
    queue(f.script, {0x3000});
    for (int i = 0; i < 6; ++i)
        f.advance();
    check(
        puff.frame == 1 && puff.remaining == 4,
        "after the base hold the next frame gets half plus a random part"
    );
    check(f.script.calls == 2, "only the spawn and the frame change drew random numbers");
    // Frames 1..5 take four ticks each with the default 0x4000 draws (3 + 1).
    for (int i = 0; i < 4 * 5; ++i)
        f.advance();
    check(
        f.layer(layer_smoke).count == 1 && e.particles.count == 0,
        "the puff is erased when it reaches its limit"
    );
    check(f.world->smoke.live == 0, "the erased puff returns to the pool");
    f.advance();
    check(f.layer(layer_smoke).count == 0, "an empty smoke emitter past its deadline is deleted");
}

void dark_and_start_smoke() {
    Fixture f;
    // Dark smoke: smoke 2 has 16 frames, cap 15, limit 0x7fff * 13 / 0x8000 + 2 = 14.
    queue(f.script, {0x7fff});
    spawn_black_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_smoke);
    const auto& dark = f.emitter(layer_smoke);
    check(dark.variant == 1 && dark.frame_cap == 15, "black smoke caps at smoke 2's last frame");
    check(
        first_record(f.world->smoke, dark).frame_limit == 14 &&
            first_record(f.world->smoke, dark).sequence == &f.smoke_2,
        "black smoke puffs draw smoke 2"
    );
    // Start smoke: cap min(3, 11) = 3, limit 0 * 1 / 0x8000 + 2 = 2, hold 30.
    queue(f.script, {0});
    spawn_start_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_smoke);
    const auto& start = f.emitter(layer_smoke, 1);
    check(
        start.frame_cap == 3 && start.hold == 30,
        "start smoke shows at most three frames of thirty ticks"
    );
    check(
        first_record(f.world->smoke, start).frame_limit == 2,
        "start smoke limits its puff to two frames"
    );
}

void smoke_column_spawns_on_interval() {
    Fixture f;
    // Smoke column (position, 7, 15): puffs at ticks 100, 107 and 114, none at 121 (> deadline 115).
    spawn_smoke_column(*f.world, f.game, host_for(f.script), units(0, 30, 0), 7, 15, layer_smoke);
    const auto& e = f.emitter(layer_smoke);
    check(e.deadline == 115 && e.next_spawn == 107, "the column's deadline and first respawn");
    int32_t spawned = 1;
    uint16_t last = e.particles.count;
    for (int i = 0; i < 25; ++i) {
        f.advance();
        if (e.particles.count > last)
            ++spawned;
        last = e.particles.count;
    }
    check(spawned == 3, "a fifteen-tick column at interval seven spawns three puffs");
}

void layer_evicts_oldest_past_400() {
    Fixture f;
    for (int32_t i = 0; i < 402; ++i)
        spawn_white_smoke(*f.world, f.game, host_for(f.script), units(i, 0, 0), layer_smoke);
    check(f.layer(layer_smoke).count == 401, "a layer grows to 401 before evicting");
    check(f.emitter(layer_smoke).origin.x == (1 << 16), "the oldest emitter was dropped");
    check(f.world->smoke.live == 401, "the dropped emitter's puff went back to the pool");
    check(
        f.world->pooled_emitters == 401, "the dropped emitter's slot went back to the emitter pool"
    );
}

void wrapped_layer_keeps_survivors_and_stale_slots() {
    Fixture f;
    check(size_effect_world(*f.world, {4, 16}), "the small emitter rings were allocated");
    auto& layer = f.world->layers[layer_smoke];
    layer.head = 3;
    layer.count = 5;
    f.world->pooled_emitters = layer.count;
    for (uint32_t index = 0; index < layer.count; ++index) {
        Emitter emitter{};
        emitter.kind = index % 2 == 0 ? EmitterKind::smoke : EmitterKind::feature_smoke;
        emitter.deadline = f.game.tick - 1;
        emitter.next_spawn = f.game.tick + 10;
        emitter.origin.x = static_cast<int32_t>(index);
        layer_emitter(*f.world, layer_smoke, index) = emitter;
    }
    f.advance();
    check(
        layer.head == 3 && layer.count == 2 && f.world->pooled_emitters == 2,
        "wrapped compaction keeps the head and releases only spent emitters"
    );
    check(
        f.emitter(layer_smoke, 0).origin.x == 1 && f.emitter(layer_smoke, 1).origin.x == 3,
        "survivors crossing the end of a ring keep their order"
    );
    const int32_t stale_origins[]{2, 3, 4, 1, 3};
    const auto base = std::size_t{layer_smoke} * f.world->layer_slots;
    for (uint32_t slot = 0; slot < f.world->layer_slots; ++slot)
        check(
            f.world->emitter_block.slots[base + slot].origin.x == stale_origins[slot],
            "wrapped compaction leaves the same stale slots"
        );

    for (int32_t origin : {5, 6}) {
        Emitter emitter{};
        emitter.kind = origin == 5 ? EmitterKind::smoke : EmitterKind::feature_smoke;
        emitter.deadline = f.game.tick - 1;
        emitter.next_spawn = f.game.tick + 10;
        emitter.origin.x = origin;
        ++f.world->pooled_emitters;
        queue_emitter(*f.world, layer_smoke, emitter);
    }
    f.advance();
    check(
        layer.head == 3 && layer.count == 3 && f.world->pooled_emitters == 3 &&
            f.emitter(layer_smoke, 0).origin.x == 1 && f.emitter(layer_smoke, 1).origin.x == 3 &&
            f.emitter(layer_smoke, 2).origin.x == 6 && f.script.calls == 0,
        "new emitters reuse the wrapped tail without moving the head or drawing random numbers"
    );
}

void empty_unallocated_layers_stay_empty() {
    Fixture f;
    f.world->emitter_block = EmitterBlock{};
    f.world->layer_slots = 0;
    f.advance();
    check(
        f.world->pooled_emitters == 0 && f.script.calls == 0,
        "an unallocated empty effect world ticks without visiting its emitter block"
    );
    for (const auto& layer : f.world->layers)
        check(layer.count == 0, "an unallocated layer remains empty");
}

void layer_and_pool_follow_raised_limits() {
    // A mod's limits of 20,480 emitters a layer and 204,800 in the pool: a
    // layer grows to 20,481 before evicting, and the pool runs out across the
    // ten layers before they fill.
    Fixture f;
    const oa::data::limits::Effects raised{20480, 204800};
    check(size_effect_world(*f.world, raised), "the raised rings are allocated");
    check(f.world->layer_slots == 20481, "each ring holds the queue plus one");
    for (int32_t i = 0; i < 20482; ++i)
        spawn_white_smoke(*f.world, f.game, host_for(f.script), units(i, 0, 0), layer_smoke);
    check(f.layer(layer_smoke).count == 20481, "a layer grows to 20,481 before evicting");
    check(f.emitter(layer_smoke).origin.x == (1 << 16), "the oldest emitter was dropped");
    check(f.world->pooled_emitters == 20481, "the evicted emitter's slot went back");
    for (uint16_t layer = 0; layer < layer_count; ++layer)
        for (int32_t i = 0; i < 20481 && layer != layer_smoke; ++i)
            spawn_white_smoke(*f.world, f.game, host_for(f.script), units(i, 0, 0), layer);
    check(f.world->pooled_emitters == 204800, "every pool slot is out");
    check(f.layer(layer_smoke - 1).count == 20481 - 10, "the last layer filled stops short by ten");
    // Back to 3.1c's sizes: an empty world with 401-slot rings and 1000 in the pool.
    check(size_effect_world(*f.world, {}), "the base rings are allocated");
    f.world->pooled_emitters = 0;
    check(f.world->layer_slots == 401 && f.layer(layer_smoke).count == 0, "base rings");
    check(f.world->evict_above == 400 && f.world->pool_capacity == 1000, "base limits");
}

void layer_counts_pass_sixteen_bits_at_the_largest_queue() {
    // The largest queue a profile may name, a million emitters a layer: the ring
    // takes the pool's size instead, and a layer and the pool count past 65,535.
    using oa::data::limits::highest_effect_queue;
    using oa::data::limits::highest_effect_reserve;
    check(
        layer_ring_slots({highest_effect_queue, highest_effect_reserve}) ==
            highest_effect_queue + 1,
        "the largest ring holds a million and one emitters"
    );
    check(
        uint64_t{layer_count} * layer_ring_slots({highest_effect_queue, highest_effect_reserve}) <
            uint64_t{1} << 32,
        "the largest rings' slot count fits 32 bits"
    );
    Fixture f;
    const oa::data::limits::Effects largest{highest_effect_queue, 65600};
    check(size_effect_world(*f.world, largest), "the rings are allocated");
    check(f.world->layer_slots == 65601, "a ring never holds more than the pool");
    for (int32_t i = 0; i < 65601; ++i)
        spawn_white_smoke(*f.world, f.game, host_for(f.script), units(i, 0, 0), layer_smoke);
    check(f.layer(layer_smoke).count == 65600, "the layer holds the whole pool");
    check(f.world->pooled_emitters == 65600, "the pool count passes 16 bits");
    check(
        f.emitter(layer_smoke, 65599).origin.x == units(65599, 0, 0).x,
        "the newest emitter keeps its place"
    );
    for (int32_t i = 0; i < 300; ++i)
        f.advance();
    check(f.layer(layer_smoke).count == 0 && f.world->pooled_emitters == 0, "every emitter ends");
}

void nano_stream_from_nozzle_to_box() {
    Fixture f;
    // Box x 0..22 units: 22 * 4 / 11 = 8 and 22 * 7 / 11 = 14 units, so the
    // target origin is 8 units with a 6-unit extent. y and z are flat at 0.
    const Box target{units(0, 0, 0), units(22, 0, 0)};
    const FixedVec3 nozzle = units(0, 0, 0);
    // Six draws a particle: source x/y/z (extent 0), then target x/y/z.
    // Particle 0: target x = 8 + 0x4000 * 6 / 0x8000 = 11 units -> 11 / 4 = 2 steps.
    // Particle 1: target x = 8 + 0 = 8 units -> 2 steps.
    // Particles 2..4 default to 11 units as well.
    queue(f.script, {0, 0, 0, 0x4000, 0, 0, 0, 0, 0, 0, 0, 0});
    spawn_nano_outward(*f.world, f.game, host_for(f.script), nozzle, target, layer_nano);
    const auto& e = f.emitter(layer_nano);
    check(
        e.target.x == (8 << 16) && e.target_extent.x == (6 << 16),
        "the nano stream keeps the middle 3/11 of the box"
    );
    check(e.origin.x == 0 && e.origin_extent.x == 0, "a nozzle box is a point");
    check(
        e.deadline == 101 && e.next_spawn == 101,
        "a nano stream lives one tick and respawns next tick"
    );
    check(e.particles.count == 5 && f.script.calls == 30, "five particles, six draws each");
    const auto& first = first_record(f.world->nano, e);
    check(
        first.target.x == (11 << 16) && first.delta.x == (11 << 16) / 2 && first.expire_tick == 102,
        "the first particle steps half its way per tick and expires two ticks on"
    );
    check(
        first.color == 0xa1 && first.unread_word == 0x100,
        "the first particle starts at palette 0xa1"
    );
    const auto& second = f.world->nano.records[f.world->nano.next[e.particles.first]];
    check(
        second.target.x == (8 << 16) && second.delta.x == (4 << 16) && second.color == 0xa2,
        "the second particle aims at the box origin"
    );
    f.advance();
    check(
        first.position.x == (11 << 16) / 2 && first.color == 0xa2,
        "a nano step moves the particle and cycles the colour nibble"
    );
    check(e.particles.count == 10, "the nano stream fires a second volley on the next tick");
    f.advance();
    f.advance();
    check(e.particles.count == 5, "the first volley expires once its step count has passed");
}

void nano_colour_wraps_after_seven() {
    Fixture f;
    const Box box{units(0, 0, 0), units(44, 0, 0)};
    spawn_nano_outward(*f.world, f.game, host_for(f.script), units(0, 0, 0), box, layer_nano);
    auto& p = f.world->nano.records[f.emitter(layer_nano).particles.first];
    p.color = 0xa7;
    p.expire_tick = 1000;
    f.advance();
    check(p.color == 0xa1, "the colour nibble wraps from 7 to 1");
}

void flame_travels_over_its_ticks() {
    Fixture f;
    // Flame delta = 6 units * (0x100000000 / (6 << 16)) >> 16 = 0x60000 * 10922 >> 16 = 65532.
    spawn_flame(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(6, 0, 0), 1, 6, layer_thrust
    );
    const auto& e = f.emitter(layer_thrust);
    check(
        e.deadline == 106 && e.delta.x == 65532, "a six-tick flame steps just under a unit a tick"
    );
    const auto& p = first_record(f.world->flame, e);
    check(
        p.frame_period == 19 && p.step_period == 1 && p.expire_tick == 106 && p.frame == 0,
        "the flame particle lives to the deadline and cycles flamestream's 19 frames"
    );
    check(f.script.calls == 0, "flames draw no random numbers");
    for (int i = 0; i < 6; ++i)
        f.advance();
    check(e.particles.count == 7, "one flame spawns each tick through the deadline");
    check(
        p.position.x == 6 * 65532 && p.frame == 6, "the first flame moved six steps and six frames"
    );
    f.advance();
    check(e.particles.count == 0, "every flame expires together after the deadline");
    f.advance();
    check(f.layer(layer_thrust).count == 0, "the empty flame emitter is deleted");
}

void wake_drifts_and_dies_ashore() {
    Fixture f;
    f.script.grid_height = 5; // below sea level 20
    // Wake: 10 units long, doubled 20 units; 0x100000000 / 0x140000 = 3276;
    // delta z = 0xa0000 * 3276 >> 16 = 32760.
    // Jitter draws: 0x7fff * 7 / 0x8000 - 3 = +3, 0 - 3 = -3, 0x4000 -> 0.
    queue(f.script, {0x7fff, 0, 0x4000});
    spawn_wake_lit(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(0, 0, 10), 8, layer_wake
    );
    const auto& e = f.emitter(layer_wake);
    check(
        e.delta.z == 32760 && e.variant == 1 && e.deadline == 101,
        "a lit wake heads half a unit a tick"
    );
    const auto& p = first_record(f.world->wake, e);
    check(
        p.position.x == (3 << 16) && p.position.y == (-3 << 16) && p.position.z == 0,
        "a wake pixel starts jittered by up to three units"
    );
    check(
        p.value == 0x61 && p.value_delta == 1 && p.period == 8 && p.expire_tick == 148,
        "a lit wake brightens from 0x61 and lives six periods"
    );
    for (int i = 0; i < 8; ++i)
        f.advance();
    check(p.value == 0x62 && p.phase == 0, "every period the colour steps once");
    check(e.particles.count == 2, "a one-tick wake spawns twice");
    f.script.grid_height = 25;
    f.advance();
    check(e.particles.count == 0, "wake pixels over ground above sea level are dropped");
}

void unlit_wake_darkens() {
    Fixture f;
    f.script.grid_height = 0;
    spawn_wake_unlit(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(10, 0, 0), 8, layer_thrust
    );
    const auto& p = first_record(f.world->wake, f.emitter(layer_thrust));
    check(p.value == 0x67 && p.value_delta == -1, "an unlit wake darkens from 0x67");
    spawn_wake_unlit(
        *f.world, f.game, host_for(f.script), units(1, 1, 1), units(1, 1, 1), 8, layer_thrust
    );
    check(
        f.layer(layer_thrust).count == 1,
        "coincident wake points are dropped instead of dividing by zero"
    );
}

void teleport_trail_steps_five_units() {
    Fixture f;
    // Trail: 50 units / 5 = 10 steps of 5 units; first frame = 0x4000 * 19 / 0x8000 = 9.
    spawn_teleport_trail(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(50, 0, 0), 30, layer_teleport
    );
    const auto& e = f.emitter(layer_teleport);
    check(
        e.interval == 10 && e.delta.x == (5 << 16) && e.deadline == 130 && e.next_spawn == 110,
        "the trail steps five units and respawns every ten ticks"
    );
    const auto& p = first_record(f.world->trail, e);
    check(
        p.period == 19 && p.frame == 9 && p.expire_tick == 110,
        "a trail sprite lives one step count"
    );
    f.advance();
    check(
        p.position.x == (5 << 16) && p.frame == 10, "a trail step moves the sprite and its frame"
    );
    for (int i = 0; i < 9; ++i)
        f.advance();
    check(e.particles.count == 2 && e.next_spawn == 120, "the tenth tick spawns the next sprite");
    // Sprites spawned at 120 and 130 expire at 131 and 141; the emitter goes at 142.
    for (int i = 0; i < 31; ++i)
        f.advance();
    check(f.layer(layer_teleport).count == 1, "the last sprite outlives the trail's deadline");
    f.advance();
    check(f.layer(layer_teleport).count == 0, "the trail is deleted once its last sprite expires");
    spawn_teleport_trail(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(4, 0, 0), 30, layer_teleport
    );
    check(f.layer(layer_teleport).count == 0, "a move under five units makes no trail");
    check(f.world->pooled_emitters == 0, "the dropped trail holds no emitter pool slot");
}

void emitter_pool_holds_a_thousand() {
    Fixture f;
    // The emitter pool has 1000 slots and hands none out past them.
    for (uint16_t layer : {uint16_t{0}, uint16_t{1}, layer_smoke})
        for (int32_t i = 0; i < 334; ++i)
            spawn_white_smoke(*f.world, f.game, host_for(f.script), units(i, 0, 0), layer);
    check(
        f.layer(0).count == 334 && f.layer(1).count == 334 && f.layer(layer_smoke).count == 332,
        "the thousandth emitter is the last one queued"
    );
    check(f.world->pooled_emitters == emitter_pool_capacity, "every pool slot is out");
    const auto calls = f.script.calls;
    const auto puffs = f.world->smoke.live;
    spawn_white_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_wake);
    spawn_teleport_trail(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(50, 0, 0), 30, layer_teleport
    );
    check(
        f.layer(layer_wake).count == 0 && f.layer(layer_teleport).count == 0 &&
            f.script.calls == calls && f.world->smoke.live == puffs,
        "with no emitter slot nothing is built and no random number is drawn"
    );
    // A spark draws its jitter and lifetime before the flame asks for a slot.
    spawn_spark(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_thrust);
    check(
        f.layer(layer_thrust).count == 0 && f.script.calls == calls + 4,
        "a spark still draws its four numbers when the pool is empty"
    );
    for (int i = 0; i < 100; ++i)
        f.advance();
    check(
        f.world->pooled_emitters == 0 && f.layer(layer_smoke).count == 0,
        "finished emitters give their slots back"
    );
    spawn_white_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_wake);
    check(f.layer(layer_wake).count == 1, "a freed slot is handed out again");
}

void sfx_toggle_refuses_emitters() {
    Fixture f;
    // The emitter pool refuses every request while the SFX toggle is set.
    f.world->emitters_refused = true;
    spawn_white_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_smoke);
    spawn_feature_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_feature_smoke);
    spawn_nano_outward(
        *f.world,
        f.game,
        host_for(f.script),
        units(0, 0, 0),
        {units(0, 0, 0), units(44, 0, 0)},
        layer_nano
    );
    spawn_wake_lit(
        *f.world, f.game, host_for(f.script), units(0, 0, 0), units(10, 0, 0), 2, layer_wake
    );
    check(
        f.layer(layer_smoke).count == 0 && f.layer(layer_feature_smoke).count == 0 &&
            f.layer(layer_nano).count == 0 && f.layer(layer_wake).count == 0 &&
            f.script.calls == 0 && f.world->pooled_emitters == 0,
        "with SFX off no emitter is built and no random number drawn"
    );
    f.world->emitters_refused = false;
    spawn_white_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_smoke);
    check(
        f.layer(layer_smoke).count == 1 && f.world->pooled_emitters == 1,
        "toggling SFX back on lets emitters through"
    );
}

void evicted_trail_frees_its_sprites() {
    Fixture f;
    // Eviction deletes the oldest trail while its sprite is still alive, and
    // the sprite goes back to the pool with it.
    for (int32_t i = 0; i < 402; ++i)
        spawn_teleport_trail(
            *f.world,
            f.game,
            host_for(f.script),
            units(i, 0, 0),
            units(i + 50, 0, 0),
            30,
            layer_teleport
        );
    check(f.layer(layer_teleport).count == 401, "a trail layer also stops at 401");
    check(f.emitter(layer_teleport).origin.x == (1 << 16), "the oldest trail was dropped");
    check(f.world->trail.live == 401, "the dropped trail's live sprite went back to the pool");
}

void feature_smoke_never_finishes() {
    Fixture f;
    f.game.gravity = 0x1000;
    spawn_feature_smoke(*f.world, f.game, host_for(f.script), units(0, 0, 0), layer_feature_smoke);
    const auto& e = f.emitter(layer_feature_smoke);
    check(
        e.kind == EmitterKind::feature_smoke && e.interval == 5 && e.deadline == 250,
        "feature smoke every five ticks"
    );
    const auto& p = first_record(f.world->smoke, e);
    f.advance();
    check(p.position.y == 16 * 0x1000, "feature smoke rises four times faster than smoke");
    for (int i = 0; i < 400; ++i)
        f.advance();
    check(
        f.layer(layer_feature_smoke).count == 1 && e.particles.count > 0,
        "feature smoke keeps puffing past its deadline"
    );
    bool ungated = false;
    draw_layer(
        *f.world, layer_feature_smoke, &ungated, [](void* context, const ParticleDraw& item) {
            *static_cast<bool*>(context) = !item.sight_gated && item.kind == DrawKind::sprite;
        }
    );
    check(ungated, "feature smoke is drawn without the sight test");
}

bool same(const FixedVec3& a, const FixedVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

void draw_visits_in_order() {
    Fixture f;
    spawn_nano_outward(
        *f.world,
        f.game,
        host_for(f.script),
        units(0, 0, 0),
        {units(0, 0, 0), units(44, 0, 0)},
        layer_nano
    );

    struct Seen {
        int32_t pixels{};
        uint8_t first_color{};
        int32_t moving{}; ///< pixels whose motion is their particle's delta
        const EffectWorld* world{};
    } seen{};

    seen.world = f.world.get();

    draw_layer(*f.world, layer_nano, &seen, [](void* context, const ParticleDraw& item) {
        auto& s = *static_cast<Seen*>(context);
        if (s.pixels++ == 0)
            s.first_color = item.color;
        // A nano particle steps by its delta each tick, which the item tells.
        for (const auto& particle : s.world->nano.records)
            if (same(particle.position, item.position) && same(particle.delta, item.motion) &&
                (particle.delta.x != 0 || particle.delta.y != 0 || particle.delta.z != 0)) {
                ++s.moving;
                break;
            }
    });
    check(
        seen.pixels == 5 && seen.first_color == 0xa1, "nano particles draw as pixels in spawn order"
    );
    check(seen.moving == 5, "each nano pixel's motion is its particle's step");
}

TrailParticle trail(FixedVec3 position, FixedVec3 delta, int32_t period, int32_t frame) {
    TrailParticle p{};
    p.position = position;
    p.delta = delta;
    p.period = period;
    p.frame = frame;
    return p;
}

void trail_step_signed_arithmetic() {
    auto p = trail({10, 20, 30}, {1, -2, 100}, 4, 1);
    check(
        step_trail_particle(p) == 0 && same(p.position, {11, 18, 130}) && p.frame == 2,
        "a trail step adds the delta and advances the frame"
    );
    p.frame = 3;
    check(
        step_trail_particle(p) == 1 && p.frame == 0,
        "a trail step stores the remainder and returns the quotient"
    );
    p = trail({0x7fffffff, 0, -1}, {2, -1, 1}, 2, 0x7fffffff);
    const auto wrapped = static_cast<int32_t>(0x80000000U);
    check(
        step_trail_particle(p) == wrapped / 2 && p.frame == 0 &&
            same(p.position, {static_cast<int32_t>(0x80000001U), -1, 0}),
        "a trail step wraps the add and the frame before the signed divide"
    );
    p = trail({}, {}, 3, -5);
    check(step_trail_particle(p) == -1 && p.frame == -1, "a trail step keeps a negative remainder");
    p = trail({}, {}, -3, 5);
    check(step_trail_particle(p) == -2 && p.frame == 0, "a trail step divides by a signed period");
}

WakeParticle wake(
    FixedVec3 position,
    FixedVec3 delta,
    int32_t low,
    int32_t high,
    int32_t value,
    int32_t value_delta,
    int32_t phase,
    int32_t period
) {
    WakeParticle p{};
    p.position = position;
    p.delta = delta;
    p.low = low;
    p.high = high;
    p.value = value;
    p.value_delta = value_delta;
    p.phase = phase;
    p.period = period;
    return p;
}

void wake_step_cycles_signed() {
    auto paced = wake({10, 20, 30}, {1, -2, 100}, 0, 5, 3, 1, 1, 4);
    check(
        step_wake_particle(paced) == 0 && same(paced.position, {11, 18, 130}) && paced.phase == 2 &&
            paced.value == 3,
        "a wake step adds the delta and advances the phase"
    );
    auto wrapped_phase = wake({}, {}, 0, 5, 3, 1, 3, 4);
    check(
        step_wake_particle(wrapped_phase) == 5 && wrapped_phase.phase == 0 &&
            wrapped_phase.value == 4,
        "a wake step returns high, not the quotient, on a zero remainder"
    );
    auto wrapped_position = wake({0x7fffffff, 0, -1}, {2, -1, 1}, 0, 9, 1, 1, 0x7fffffff, 2);
    check(
        step_wake_particle(wrapped_position) == 9 && wrapped_position.phase == 0 &&
            wrapped_position.value == 2 &&
            same(wrapped_position.position, {static_cast<int32_t>(0x80000001U), -1, 0}),
        "a wake step wraps the position add and the phase"
    );
    auto above = wake({}, {}, 0, 10, 8, 5, 1, 2);
    check(
        step_wake_particle(above) == 10 && above.phase == 0 && above.value == 0,
        "above high snaps to low"
    );
    auto below = wake({}, {}, 2, 10, 3, -5, 0, 1);
    check(
        step_wake_particle(below) == 10 && below.phase == 0 && below.value == 10,
        "below low snaps to high"
    );
    auto equal_high = wake({}, {}, -5, 5, 5, 0, 2, 3);
    check(
        step_wake_particle(equal_high) == 5 && equal_high.value == 5, "a value equal to high stays"
    );
    auto negative = wake({-5, 0, 0}, {-1, 0, 0}, 0, 100, 7, 9, -5, 3);
    check(
        step_wake_particle(negative) == -1 && negative.phase == -1 && negative.value == 7 &&
            same(negative.position, {-6, 0, 0}),
        "a negative phase remainder skips the value step"
    );
    auto overflow = wake({}, {}, 0, 10, 0x7fffffff, 1, 0, 1);
    check(
        step_wake_particle(overflow) == 10 && overflow.value == 10,
        "a wrapped value falls below low and snaps"
    );
    auto signed_period = wake({}, {}, 0, -3, 0, -1, 5, -3);
    check(
        step_wake_particle(signed_period) == -3 && signed_period.phase == 0 &&
            signed_period.value == 0,
        "a signed period still snaps on the high compare"
    );
}

void missing_fx_spawns_nothing() {
    auto world = std::make_unique<EffectWorld>();
    Script script{};
    oa::Game game{};
    spawn_white_smoke(*world, game, host_for(script), units(0, 0, 0), layer_smoke);
    spawn_flame(*world, game, host_for(script), units(0, 0, 0), units(1, 0, 0), 1, 6, layer_thrust);
    check(
        world->layers[layer_smoke].count == 0 && world->layers[layer_thrust].count == 0 &&
            script.calls == 0,
        "without FX.GAF entries nothing is spawned"
    );
}
} // namespace

int main() {
    white_smoke_puff_lifecycle();
    dark_and_start_smoke();
    smoke_column_spawns_on_interval();
    layer_evicts_oldest_past_400();
    wrapped_layer_keeps_survivors_and_stale_slots();
    empty_unallocated_layers_stay_empty();
    layer_and_pool_follow_raised_limits();
    layer_counts_pass_sixteen_bits_at_the_largest_queue();
    nano_stream_from_nozzle_to_box();
    nano_colour_wraps_after_seven();
    flame_travels_over_its_ticks();
    wake_drifts_and_dies_ashore();
    unlit_wake_darkens();
    teleport_trail_steps_five_units();
    emitter_pool_holds_a_thousand();
    sfx_toggle_refuses_emitters();
    evicted_trail_frees_its_sprites();
    feature_smoke_never_finishes();
    draw_visits_in_order();
    trail_step_signed_arithmetic();
    wake_step_cycles_signed();
    missing_fx_spawns_nothing();
    if (failures != 0) {
        std::fprintf(stderr, "%d effect particle checks failed\n", failures);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
