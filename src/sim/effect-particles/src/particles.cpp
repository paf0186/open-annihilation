// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/effect_particles.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <utility>

namespace oa::sim::effect_particles {
namespace {
constexpr int32_t lcg_rand_range = 0x8000;
constexpr int32_t default_smoke_hold = 7; // both smoke emitters, when hold is 0
constexpr int32_t trail_respawn_ticks = 10;
constexpr int32_t trail_step_length = 0x50000; // five units per step
constexpr int32_t nano_step_length = 0x40000;  // four units per step
constexpr int32_t nano_stream_count = 5;
constexpr int32_t nano_palette_base = 0xa1;
constexpr int32_t nano_unread_word = 0x100; // written at spawn, never read
constexpr int32_t wake_ramp_low = 0x61;
constexpr int32_t wake_ramp_high = 0x67;
constexpr int32_t wake_lifetime_periods = 6;
constexpr int32_t wake_jitter_span = 7;    // -3..+3 units
constexpr int32_t spark_jitter_span = 3;   // -1..+1 units
constexpr int32_t smoke_rise = 4;          // gravity multiple
constexpr int32_t feature_smoke_rise = 16; // gravity multiple
constexpr int32_t smoke_wind_drift = 8;
constexpr int32_t start_smoke_frames = 3;
constexpr int32_t start_smoke_hold = 30;
constexpr int32_t feature_smoke_interval = 5;
constexpr int32_t feature_smoke_duration = 150;
constexpr int32_t nano_lifetime = 1;
constexpr int32_t wake_lifetime = 1;
constexpr int32_t fixed_one = 0x10000;

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) * std::bit_cast<uint32_t>(b));
}

FixedVec3 add(const FixedVec3& a, const FixedVec3& b) noexcept {
    return {wrap_add(a.x, b.x), wrap_add(a.y, b.y), wrap_add(a.z, b.z)};
}

FixedVec3 sub(const FixedVec3& a, const FixedVec3& b) noexcept {
    return {wrap_sub(a.x, b.x), wrap_sub(a.y, b.y), wrap_sub(a.z, b.z)};
}

int32_t lcg_rand(const EffectHost& host) {
    return host.lcg_rand(host.context);
}

// rand() scaled by `n` over the generator range; every spawn samples this way.
int32_t rand_scaled(const EffectHost& host, int32_t n) {
    return static_cast<int32_t>(static_cast<int64_t>(lcg_rand(host)) * n / lcg_rand_range);
}

// Add whole units to the integer half of a 16.16 word with 16-bit wrap.
int32_t jitter_units(int32_t fixed, int32_t units) noexcept {
    const auto bits = std::bit_cast<uint32_t>(fixed);
    const auto high = static_cast<uint16_t>((bits >> 16) + static_cast<uint32_t>(units));
    return std::bit_cast<int32_t>((static_cast<uint32_t>(high) << 16) | (bits & 0xffffU));
}

int32_t length_of(const FixedVec3& v) noexcept {
    return base::game_math::truncated_length(v.x, v.y, v.z);
}

// Whole steps of `step` in a 16.16 length: the high word of (length << 16) / step.
int32_t step_count(int32_t length, int32_t step) noexcept {
    const auto quotient = (static_cast<int64_t>(length) << 16) / step;
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint64_t>(quotient) >> 16));
}

/// Scales a 16.16 vector in place by a 16.16 factor.
///
/// @param[in,out] v vector to scale
/// @param factor 16.16 fixed-point factor
void scale_in_place(FixedVec3& v, int32_t factor) noexcept {
    int32_t components[3]{v.x, v.y, v.z};
    base::game_math::scale_vector_fixed(components, components, factor);
    v = {components[0], components[1], components[2]};
}

// 0x100000000 / divisor, low 32 bits: the reciprocal the game scales by.
int32_t reciprocal(int32_t divisor) noexcept {
    return static_cast<int32_t>((int64_t{1} << 32) / divisor);
}

int32_t frame_count(const formats::gaf::Sequence& sequence) noexcept {
    return static_cast<uint16_t>(sequence.frames.size());
}

int32_t now(const oa::Game& game) noexcept {
    return static_cast<int32_t>(game.tick);
}

// ---- particle pools -------------------------------------------------------

template <class Record, uint16_t Capacity>
uint16_t pool_allocate(ParticlePool<Record, Capacity>& pool) noexcept {
    uint16_t index = no_record;
    if (pool.free_head != no_record) {
        index = pool.free_head;
        pool.free_head = pool.next[index];
    } else if (pool.high_water < Capacity) {
        index = pool.high_water++;
    } else {
        return no_record;
    }
    pool.next[index] = no_record;
    ++pool.live;
    return index;
}

template <class Record, uint16_t Capacity>
void pool_release(ParticlePool<Record, Capacity>& pool, uint16_t index) noexcept {
    pool.next[index] = pool.free_head;
    pool.free_head = index;
    --pool.live;
}

template <class Record, uint16_t Capacity>
void push_particle(
    ParticlePool<Record, Capacity>& pool, ParticleList& list, const Record& record
) noexcept {
    const auto index = pool_allocate(pool);
    if (index == no_record)
        return;
    pool.records[index] = record;
    if (list.last == no_record)
        list.first = index;
    else
        pool.next[list.last] = index;
    list.last = index;
    ++list.count;
}

template <class Record, uint16_t Capacity>
void clear_particles(ParticlePool<Record, Capacity>& pool, ParticleList& list) noexcept {
    auto index = list.first;
    while (index != no_record) {
        const auto next = pool.next[index];
        pool_release(pool, index);
        index = next;
    }
    list = {};
}

/// Steps every particle of a list in order and erases those `step` reports spent.
///
/// The survivors keep their order.
///
/// @param[in,out] pool the particle class's pool
/// @param[in,out] list the emitter's particles
/// @param step called on each particle; true erases it
template <class Record, uint16_t Capacity, class Step>
void step_particles(ParticlePool<Record, Capacity>& pool, ParticleList& list, Step step) {
    uint16_t previous = no_record;
    auto index = list.first;
    while (index != no_record) {
        const auto next = pool.next[index];
        if (step(pool.records[index])) {
            if (previous == no_record)
                list.first = next;
            else
                pool.next[previous] = next;
            if (list.last == index)
                list.last = previous;
            pool_release(pool, index);
            --list.count;
        } else {
            previous = index;
        }
        index = next;
    }
}

template <class Record, uint16_t Capacity, class Visit>
void each_particle(
    const ParticlePool<Record, Capacity>& pool, const ParticleList& list, Visit visit
) {
    for (auto index = list.first; index != no_record; index = pool.next[index])
        visit(pool.records[index]);
}

/// Takes a slot of the shared emitter pool for a new emitter.
///
/// @param[in,out] world emitter pool count and SFX toggle
/// @return false while the SFX toggle refuses emitters or once every slot is out; the
///         caller then creates nothing and draws no random numbers
bool take_emitter_slot(EffectWorld& world) noexcept {
    if (world.emitters_refused || world.pooled_emitters >= world.pool_capacity)
        return false;
    ++world.pooled_emitters;
    return true;
}

/// Returns a slot to the shared emitter pool.
///
/// @param[in,out] world emitter pool count
void give_back_emitter_slot(EffectWorld& world) noexcept {
    --world.pooled_emitters;
}

/// Creates an emitter with no deadline and an empty particle list.
///
/// The trail, nano, flame and wake emitters are built this way.
///
/// @param kind emitter class
/// @return the new emitter
Emitter make_emitter(EmitterKind kind) noexcept {
    Emitter e{};
    e.kind = kind;
    return e;
}

/// Creates a smoke emitter that is due at once; its first puff sets the next spawn.
///
/// @param kind smoke or feature_smoke
/// @param game current tick
/// @return the new emitter
Emitter make_smoke_emitter(EmitterKind kind, const oa::Game& game) noexcept {
    auto e = make_emitter(kind);
    e.next_spawn = game.tick;
    return e;
}

/// Deletes a finished or evicted emitter.
///
/// Its particles return to their pool and its slot to the emitter pool. The classes
/// differ only in particle size.
///
/// @param[in,out] world particle pools and emitter pool count
/// @param[in,out] emitter emitter whose particles are released
void release_emitter(EffectWorld& world, Emitter& emitter) noexcept {
    switch (emitter.kind) {
    case EmitterKind::teleport_trail:
        clear_particles(world.trail, emitter.particles);
        break;
    case EmitterKind::nano:
        clear_particles(world.nano, emitter.particles);
        break;
    case EmitterKind::flame:
        clear_particles(world.flame, emitter.particles);
        break;
    case EmitterKind::wake:
        clear_particles(world.wake, emitter.particles);
        break;
    case EmitterKind::smoke:
    case EmitterKind::feature_smoke:
        clear_particles(world.smoke, emitter.particles);
        break;
    }
    give_back_emitter_slot(world);
}

/// Returns the ring slot of a layer's emitter.
///
/// @param world layers and their emitter block
/// @param layer_index layer index 0..9
/// @param index position in the layer, oldest first
/// @return index into the world's emitter block
std::size_t ring_slot(const EffectWorld& world, uint16_t layer_index, uint32_t index) noexcept {
    const Layer& layer = world.layers[layer_index];
    const uint32_t slot = static_cast<uint32_t>((uint64_t{layer.head} + index) % world.layer_slots);
    return std::size_t{layer_index} * world.layer_slots + slot;
}

/// Sets an emitter's deadline a number of ticks from now.
///
/// @param[in,out] emitter emitter to update
/// @param game current tick
/// @param duration ticks until the deadline; the sum wraps
void set_deadline(Emitter& emitter, const oa::Game& game, int32_t duration) noexcept {
    emitter.deadline = static_cast<uint32_t>(wrap_add(now(game), duration));
}

/// Tests whether an emitter should spawn this tick.
///
/// @param emitter emitter to test
/// @param game current tick
/// @return true while the next spawn is inside the deadline (signed compare) and has
///         been reached (unsigned compare)
bool spawn_due(const Emitter& emitter, const oa::Game& game) noexcept {
    return static_cast<int32_t>(emitter.next_spawn) <= static_cast<int32_t>(emitter.deadline) &&
           emitter.next_spawn <= game.tick;
}

/// Tests whether a feature smoke emitter should spawn this tick.
///
/// @param emitter emitter to test
/// @param game current tick
/// @return true once the next spawn tick is reached; feature smoke has no deadline
bool feature_spawn_due(const Emitter& emitter, const oa::Game& game) noexcept {
    return game.tick >= emitter.next_spawn;
}

// ---- teleport trail ----

/// Adds one flamestream sprite at a random frame, living one step count.
///
/// The next follows ten ticks later.
///
/// @param[in,out] world particle pools
/// @param[in,out] e trail emitter
/// @param game current tick
/// @param host rand() stream
void spawn_trail_particle(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    const auto* flame = world.fx[static_cast<uint32_t>(Fx::flamestream)];
    TrailParticle p{};
    p.sequence = flame;
    p.position = e.origin;
    p.target = e.target;
    p.delta = e.delta;
    p.expire_tick = wrap_add(now(game), e.interval);
    p.period = frame_count(*flame) - 1;
    p.frame = rand_scaled(host, p.period);
    push_particle(world.trail, e.particles, p);
    e.next_spawn = game.tick + trail_respawn_ticks;
}

/// Starts a teleport trail emitter and its first sprite.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new trail emitter
/// @param game current tick
/// @param host rand() stream
/// @param from old position, 16.16 world coordinates
/// @param to new position, 16.16 world coordinates
/// @param lifetime ticks until the deadline
/// @return false when the move is shorter than one five-unit step
bool init_trail(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t lifetime
) {
    set_deadline(e, game, lifetime);
    e.origin = from;
    e.target = to;
    e.delta = sub(to, from);
    const auto steps = step_count(length_of(e.delta), trail_step_length);
    if (steps == 0)
        return false;
    e.interval = steps;
    e.delta = {e.delta.x / steps, e.delta.y / steps, e.delta.z / steps};
    spawn_trail_particle(world, e, game, host);
    return true;
}

/// Tests whether a trail particle has outlived its expiry tick.
///
/// @param p trail particle
/// @param game current tick
/// @return true once the expiry tick is behind the current tick
bool trail_expired(const TrailParticle& p, const oa::Game& game) noexcept {
    return p.expire_tick < now(game);
}

/// Steps a trail emitter's particles and spawns the next when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e trail emitter
/// @param game current tick
/// @param host rand() stream
void tick_trail(EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host) {
    step_particles(world.trail, e.particles, [&](TrailParticle& p) {
        (void)step_trail_particle(p);
        return trail_expired(p, game);
    });
    if (spawn_due(e, game))
        spawn_trail_particle(world, e, game, host);
}

// ---- nano stream ----

/// Adds five nano particles flying from the source box to the target box.
///
/// Each flies from a random point of the source box to a random point of the target
/// box, one step per four units; a pair closer than four units is skipped. The next
/// five follow a tick later.
///
/// @param[in,out] world particle pools
/// @param[in,out] e nano emitter
/// @param game current tick
/// @param host rand() stream
void spawn_nano_particles(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    for (int32_t i = 0; i < nano_stream_count; ++i) {
        NanoParticle p{};
        p.position.x = wrap_add(rand_scaled(host, e.origin_extent.x), e.origin.x);
        p.position.y = wrap_add(rand_scaled(host, e.origin_extent.y), e.origin.y);
        p.position.z = wrap_add(rand_scaled(host, e.origin_extent.z), e.origin.z);
        p.target.x = wrap_add(rand_scaled(host, e.target_extent.x), e.target.x);
        p.target.y = wrap_add(rand_scaled(host, e.target_extent.y), e.target.y);
        p.target.z = wrap_add(rand_scaled(host, e.target_extent.z), e.target.z);
        p.delta = sub(p.target, p.position);
        const auto steps = step_count(length_of(p.delta), nano_step_length);
        if (steps == 0)
            continue;
        p.delta = {p.delta.x / steps, p.delta.y / steps, p.delta.z / steps};
        p.unread_word = nano_unread_word;
        p.expire_tick = wrap_add(now(game), steps);
        p.color = nano_palette_base + i % 7;
        push_particle(world.nano, e.particles, p);
    }
    e.next_spawn = game.tick + 1;
}

// Keep the middle 3/11 of one box axis as an origin and an extent.
void middle_of(int32_t low, int32_t high, int32_t& origin, int32_t& extent) noexcept {
    const auto span = wrap_sub(high, low);
    const auto start = wrap_add(wrap_mul(span, 4) / 11, low);
    const auto end = wrap_add(wrap_mul(span, 7) / 11, low);
    origin = start;
    extent = wrap_sub(end, start);
}

/// Starts a nano emitter between the middles of two boxes, and its first particles.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new nano emitter
/// @param game current tick
/// @param host rand() stream
/// @param from source box; the middle 3/11 of each axis is used
/// @param to target box; the middle 3/11 of each axis is used
/// @param lifetime ticks until the deadline
void init_nano(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const EffectHost& host,
    const Box& from,
    const Box& to,
    int32_t lifetime
) {
    set_deadline(e, game, lifetime);
    middle_of(to.low.x, to.high.x, e.target.x, e.target_extent.x);
    middle_of(to.low.y, to.high.y, e.target.y, e.target_extent.y);
    middle_of(to.low.z, to.high.z, e.target.z, e.target_extent.z);
    middle_of(from.low.x, from.high.x, e.origin.x, e.origin_extent.x);
    middle_of(from.low.y, from.high.y, e.origin.y, e.origin_extent.y);
    middle_of(from.low.z, from.high.z, e.origin.z, e.origin_extent.z);
    spawn_nano_particles(world, e, game, host);
}

/// Advances a nano particle and cycles the low nibble of its colour through 1..7.
///
/// @param[in,out] p nano particle
void step_nano(NanoParticle& p) noexcept {
    p.position = add(p.position, p.delta);
    auto low = static_cast<uint16_t>((static_cast<uint32_t>(p.color) & 0xfU) + 1U);
    if (low > 7U)
        low = 1U;
    p.color = static_cast<int32_t>((static_cast<uint32_t>(p.color) & 0xfff0U) + low);
}

/// Tests whether a nano particle has outlived its expiry tick.
///
/// @param p nano particle
/// @param game current tick
/// @return true once the expiry tick is behind the current tick
bool nano_expired(const NanoParticle& p, const oa::Game& game) noexcept {
    return p.expire_tick < now(game);
}

/// Steps a nano emitter's particles and spawns the next five when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e nano emitter
/// @param game current tick
/// @param host rand() stream
void tick_nano(EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host) {
    step_particles(world.nano, e.particles, [&](NanoParticle& p) {
        step_nano(p);
        return nano_expired(p, game);
    });
    if (spawn_due(e, game))
        spawn_nano_particles(world, e, game, host);
}

// ---- flame ----

/// Adds one flamestream sprite living until the emitter's deadline.
///
/// The next follows a tick later.
///
/// @param[in,out] world particle pools
/// @param[in,out] e flame emitter
/// @param game current tick
void spawn_flame_particle(EffectWorld& world, Emitter& e, const oa::Game& game) {
    const auto* flame = world.fx[static_cast<uint32_t>(Fx::flamestream)];
    FlameParticle p{};
    p.step_period = e.interval;
    p.step = 0;
    p.expire_tick = static_cast<int32_t>(e.deadline);
    p.position = e.origin;
    p.target = e.target;
    p.delta = e.delta;
    p.sequence = flame;
    p.frame = 0;
    p.frame_period = frame_count(*flame) - 1;
    push_particle(world.flame, e.particles, p);
    e.next_spawn = game.tick + 1;
}

/// Starts a flame emitter carrying sprites between two points, and its first sprite.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new flame emitter
/// @param game current tick
/// @param from start point, 16.16 world coordinates
/// @param to end point, 16.16 world coordinates
/// @param period ticks per animation frame
/// @param travel ticks to cover the distance, and the emitter's lifetime
/// @return false when `travel` scaled to 16.16 wraps to zero
bool init_flame(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    int32_t travel
) {
    set_deadline(e, game, travel);
    e.interval = period;
    e.origin = from;
    e.target = to;
    e.delta = sub(to, from);
    const auto divisor = wrap_mul(travel, fixed_one);
    if (divisor == 0)
        return false;
    scale_in_place(e.delta, reciprocal(divisor));
    spawn_flame_particle(world, e, game);
    return true;
}

/// Moves a flame particle one step and, every step_period steps, advances its frame.
///
/// @param[in,out] p flame particle; the frame wraps at the frame period
void step_flame(FlameParticle& p) noexcept {
    p.position = add(p.position, p.delta);
    const auto step = static_cast<int32_t>(static_cast<uint32_t>(p.step) + 1U);
    p.step = step % p.step_period;
    if (p.step == 0) {
        const auto frame = static_cast<int32_t>(static_cast<uint32_t>(p.frame) + 1U);
        p.frame = frame % p.frame_period;
    }
}

/// Tests whether a flame particle has outlived its expiry tick.
///
/// @param p flame particle
/// @param game current tick
/// @return true once the expiry tick is behind the current tick
bool flame_expired(const FlameParticle& p, const oa::Game& game) noexcept {
    return p.expire_tick < now(game);
}

/// Steps a flame emitter's particles and spawns the next when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e flame emitter
/// @param game current tick
void tick_flame(EffectWorld& world, Emitter& e, const oa::Game& game) {
    step_particles(world.flame, e.particles, [&](FlameParticle& p) {
        step_flame(p);
        return flame_expired(p, game);
    });
    if (spawn_due(e, game))
        spawn_flame_particle(world, e, game);
}

// ---- wake ----

/// Adds one wake pixel jittered by up to three units, living six periods.
///
/// It cycles through the colour ramp upwards when lit, downwards otherwise.
///
/// @param[in,out] world particle pools
/// @param[in,out] e wake emitter
/// @param game current tick
/// @param host rand() stream
void spawn_wake_particle(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    WakeParticle p{};
    p.period = e.interval;
    p.phase = 0;
    p.expire_tick = wrap_add(now(game), wrap_mul(e.interval, wake_lifetime_periods));
    p.position = e.origin;
    p.position.x = jitter_units(p.position.x, rand_scaled(host, wake_jitter_span) - 3);
    p.position.y = jitter_units(p.position.y, rand_scaled(host, wake_jitter_span) - 3);
    p.position.z = jitter_units(p.position.z, rand_scaled(host, wake_jitter_span) - 3);
    p.target = e.target;
    p.delta = e.delta;
    p.sequence = world.fx[static_cast<uint32_t>(Fx::smoke_1)];
    p.low = wake_ramp_low;
    p.high = wake_ramp_high;
    if (e.variant == 0) {
        p.value_delta = -1;
        p.value = wake_ramp_high;
    } else {
        p.value = wake_ramp_low;
        p.value_delta = 1;
    }
    push_particle(world.wake, e.particles, p);
    e.next_spawn = game.tick + 1;
}

/// Starts a wake emitter drifting half a unit a tick between two points, and its first pixel.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new wake emitter
/// @param game current tick
/// @param host rand() stream
/// @param from start point, 16.16 world coordinates
/// @param to point the pixels drift towards
/// @param period ticks per colour step
/// @param lifetime ticks until the deadline
/// @param lit true to brighten through the ramp, false to darken
/// @return false when the two points are too close to give a direction
bool init_wake(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    int32_t lifetime,
    bool lit
) {
    set_deadline(e, game, lifetime);
    e.interval = period;
    e.origin = from;
    e.target = to;
    e.delta = sub(to, from);
    const auto doubled = static_cast<int32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(length_of(e.delta)) * 0x20000) >> 16
    );
    if (doubled == 0)
        return false;
    scale_in_place(e.delta, reciprocal(doubled));
    e.variant = lit ? 1 : 0;
    spawn_wake_particle(world, e, game, host);
    return true;
}

/// Tests whether a wake pixel is spent.
///
/// @param p wake particle
/// @param game current tick and sea level
/// @param host terrain grid height query
/// @return false while its lifetime holds and the ground under it is below sea level
bool wake_expired(const WakeParticle& p, const oa::Game& game, const EffectHost& host) {
    if (now(game) <= p.expire_tick && host.grid_height(host.context, p.position) < game.sea_level)
        return false;
    return true;
}

/// Steps a wake emitter's pixels and spawns the next when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e wake emitter
/// @param game current tick and sea level
/// @param host random stream and terrain grid height
void tick_wake(EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host) {
    step_particles(world.wake, e.particles, [&](WakeParticle& p) {
        (void)step_wake_particle(p);
        return wake_expired(p, game, host);
    });
    if (spawn_due(e, game))
        spawn_wake_particle(world, e, game, host);
}

// ---- smoke and feature smoke ----

SmokeParticle
make_puff(const Emitter& e, const formats::gaf::Sequence* sequence, const EffectHost& host) {
    SmokeParticle p{};
    p.sequence = sequence;
    p.position = e.origin;
    p.hold = e.hold;
    p.remaining = e.hold;
    p.frame_limit = rand_scaled(host, e.frame_cap - 2) + 2;
    p.frame = 0;
    return p;
}

const formats::gaf::Sequence* smoke_sequence(const EffectWorld& world, int32_t variant) noexcept {
    return world.fx[static_cast<uint32_t>(variant == 0 ? Fx::smoke_1 : Fx::smoke_2)];
}

/// Adds one puff showing two to frame_cap - 1 frames, and schedules the next.
///
/// @param[in,out] world particle pools
/// @param[in,out] e smoke emitter
/// @param game current tick
/// @param host rand() stream
void spawn_smoke_puff(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    push_particle(world.smoke, e.particles, make_puff(e, smoke_sequence(world, e.variant), host));
    e.next_spawn = game.tick + static_cast<uint32_t>(e.interval);
}

/// Adds one light feature-smoke puff and schedules the next.
///
/// @param[in,out] world particle pools
/// @param[in,out] e feature smoke emitter
/// @param game current tick
/// @param host rand() stream
void spawn_feature_puff(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    push_particle(
        world.smoke, e.particles, make_puff(e, world.fx[static_cast<uint32_t>(Fx::smoke_1)], host)
    );
    e.next_spawn = game.tick + static_cast<uint32_t>(e.interval);
}

/// Starts a smoke emitter and its first puff.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new smoke emitter
/// @param game current tick
/// @param host rand() stream
/// @param position where the puffs start, 16.16 world coordinates
/// @param frame_cap frames a puff may show; 0 or more than the sequence has means all
///        but the last
/// @param interval ticks between puffs
/// @param hold ticks per frame; 0 means 7
/// @param duration ticks until the deadline
/// @param dark true for smoke 2, false for smoke 1
void init_smoke(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    int32_t frame_cap,
    int32_t interval,
    int32_t hold,
    int32_t duration,
    bool dark
) {
    set_deadline(e, game, duration);
    e.origin = position;
    e.interval = interval;
    e.variant = dark ? 1 : 0;
    auto cap = frame_count(*smoke_sequence(world, e.variant)) - 1;
    if (frame_cap != 0 && frame_cap < cap)
        cap = frame_cap;
    e.frame_cap = cap;
    e.hold = hold == 0 ? default_smoke_hold : hold;
    spawn_smoke_puff(world, e, game, host);
}

/// Starts a feature smoke emitter and its first puff.
///
/// @param[in,out] world particle pools
/// @param[in,out] e new feature smoke emitter
/// @param game current tick
/// @param host rand() stream
/// @param position where the puffs start, 16.16 world coordinates
/// @param interval ticks between puffs
/// @param hold ticks per frame; 0 means 7
/// @param duration ticks until the deadline
void init_feature_smoke(
    EffectWorld& world,
    Emitter& e,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    int32_t interval,
    int32_t hold,
    int32_t duration
) {
    set_deadline(e, game, duration);
    e.origin = position;
    e.interval = interval;
    e.frame_cap = frame_count(*world.fx[static_cast<uint32_t>(Fx::smoke_1)]) - 1;
    e.hold = hold == 0 ? default_smoke_hold : hold;
    spawn_feature_puff(world, e, game, host);
}

// Drift with the wind and rise; when the hold runs out show the next frame
// for half the base hold plus a random part of that half.
void drift_puff(SmokeParticle& p, const oa::Game& game, const EffectHost& host, int32_t rise) {
    p.position.x = wrap_add(p.position.x, wrap_mul(game.wind_vector.x, smoke_wind_drift));
    p.position.y = wrap_add(p.position.y, wrap_mul(game.gravity, rise));
    p.position.z = wrap_add(p.position.z, wrap_mul(game.wind_vector.z, smoke_wind_drift));
    if (--p.remaining == 0) {
        ++p.frame;
        const auto half = p.hold / 2;
        p.remaining = rand_scaled(host, half) + half;
    }
}

/// Drifts a smoke puff with the wind, raises it by four times gravity and ages its frame.
///
/// @param[in,out] p smoke puff
/// @param game wind and gravity
/// @param host rand() stream for the next frame's hold
void step_smoke_puff(SmokeParticle& p, const oa::Game& game, const EffectHost& host) {
    drift_puff(p, game, host, smoke_rise);
}

/// Drifts a feature-smoke puff with the wind, raises it by sixteen times gravity and ages its frame.
///
/// @param[in,out] p smoke puff
/// @param game wind and gravity
/// @param host rand() stream for the next frame's hold
void step_feature_puff(SmokeParticle& p, const oa::Game& game, const EffectHost& host) {
    drift_puff(p, game, host, feature_smoke_rise);
}

/// Steps a smoke emitter's puffs and spawns the next when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e smoke emitter
/// @param game current tick, wind and gravity
/// @param host rand() stream
void tick_smoke(EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host) {
    step_particles(world.smoke, e.particles, [&](SmokeParticle& p) {
        step_smoke_puff(p, game, host);
        return p.frame >= p.frame_limit;
    });
    if (spawn_due(e, game))
        spawn_smoke_puff(world, e, game, host);
}

/// Steps a feature smoke emitter's puffs and spawns the next when due.
///
/// @param[in,out] world particle pools
/// @param[in,out] e feature smoke emitter
/// @param game current tick, wind and gravity
/// @param host rand() stream
void tick_feature_smoke(
    EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host
) {
    step_particles(world.smoke, e.particles, [&](SmokeParticle& p) {
        step_feature_puff(p, game, host);
        return p.frame >= p.frame_limit;
    });
    if (feature_spawn_due(e, game))
        spawn_feature_puff(world, e, game, host);
}

/// Tests whether an emitter's particle list is empty.
///
/// @param e emitter
/// @return true when no particle is left
bool particles_gone(const Emitter& e) noexcept {
    return e.particles.count == 0;
}

/// Tests whether a smoke emitter is done.
///
/// @param e smoke emitter
/// @param game current tick
/// @return true once its puffs are gone and its deadline has passed
bool smoke_finished(const Emitter& e, const oa::Game& game) noexcept {
    return particles_gone(e) && e.deadline < game.tick;
}

// Whether an emitter is done; feature smoke never finishes.
bool finished(const Emitter& e, const oa::Game& game) noexcept {
    switch (e.kind) {
    case EmitterKind::teleport_trail:
    case EmitterKind::nano:
    case EmitterKind::flame:
    case EmitterKind::wake:
        return particles_gone(e);
    case EmitterKind::smoke:
        return smoke_finished(e, game);
    case EmitterKind::feature_smoke:
        return false;
    }
    return true;
}

void tick_emitter(EffectWorld& world, Emitter& e, const oa::Game& game, const EffectHost& host) {
    switch (e.kind) {
    case EmitterKind::teleport_trail:
        tick_trail(world, e, game, host);
        break;
    case EmitterKind::nano:
        tick_nano(world, e, game, host);
        break;
    case EmitterKind::flame:
        tick_flame(world, e, game);
        break;
    case EmitterKind::wake:
        tick_wake(world, e, game, host);
        break;
    case EmitterKind::smoke:
        tick_smoke(world, e, game, host);
        break;
    case EmitterKind::feature_smoke:
        tick_feature_smoke(world, e, game, host);
        break;
    }
}

/// Builds the draw item of a particle's sprite.
///
/// The host tests the sight grid, projects the point and blits the frame.
///
/// @param sequence GAF sequence to draw from
/// @param frame frame index in the sequence
/// @param position 16.16 world coordinates
/// @param gated true to draw only where the viewer's sight grid shows the point
///        (everything but feature smoke)
/// @return the draw item
ParticleDraw sprite_item(
    const formats::gaf::Sequence* sequence, int32_t frame, const FixedVec3& position, bool gated
) {
    ParticleDraw item{};
    item.kind = DrawKind::sprite;
    item.sight_gated = gated;
    item.position = position;
    item.sequence = sequence;
    item.frame = frame;
    return item;
}

/// Builds the draw item of a nano or wake pixel.
///
/// It is drawn where the viewer's sight grid shows its position, as a square
/// pixel_item_side battlefield pixels on a side; the host tests and fills it.
///
/// @param color palette index
/// @param position 16.16 world coordinates
/// @return the draw item
ParticleDraw pixel_item(int32_t color, const FixedVec3& position) {
    ParticleDraw item{};
    item.kind = DrawKind::pixel;
    item.position = position;
    item.color = static_cast<uint8_t>(color);
    return item;
}

/// Visits one draw item per particle of an emitter, oldest first.
///
/// @param world particle pools
/// @param e emitter to draw
/// @param context passed through to `visit`
/// @param visit called once per particle
void draw_emitter(
    const EffectWorld& world, const Emitter& e, void* context, ParticleVisitor visit
) {
    // Each particle that steps by its delta tells the step as its motion.
    const auto moving = [](ParticleDraw item, const FixedVec3& delta) {
        item.motion = delta;
        return item;
    };
    switch (e.kind) {
    case EmitterKind::teleport_trail:
        each_particle(world.trail, e.particles, [&](const TrailParticle& p) {
            visit(context, moving(sprite_item(p.sequence, p.frame, p.position, true), p.delta));
        });
        break;
    case EmitterKind::nano:
        each_particle(world.nano, e.particles, [&](const NanoParticle& p) {
            visit(context, moving(pixel_item(p.color, p.position), p.delta));
        });
        break;
    case EmitterKind::flame:
        each_particle(world.flame, e.particles, [&](const FlameParticle& p) {
            visit(context, moving(sprite_item(p.sequence, p.frame, p.position, true), p.delta));
        });
        break;
    case EmitterKind::wake:
        each_particle(world.wake, e.particles, [&](const WakeParticle& p) {
            visit(context, moving(pixel_item(p.value, p.position), p.delta));
        });
        break;
    case EmitterKind::smoke:
    case EmitterKind::feature_smoke:
        each_particle(world.smoke, e.particles, [&](const SmokeParticle& p) {
            visit(
                context, sprite_item(p.sequence, p.frame, p.position, e.kind == EmitterKind::smoke)
            );
        });
        break;
    }
}

bool fx_loaded(const EffectWorld& world, Fx entry) noexcept {
    return world.fx[static_cast<uint32_t>(entry)] != nullptr;
}

/// Creates a smoke emitter, starts it and appends it to a layer.
///
/// Nothing happens when the smoke sequence is not loaded or the emitter pool refuses.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param host rand() stream
/// @param position where the puffs start, 16.16 world coordinates
/// @param frame_cap frames a puff may show; 0 means all but the last
/// @param interval ticks between puffs
/// @param hold ticks per frame; 0 means 7
/// @param duration ticks until the deadline
/// @param layer layer index 0..9
/// @param dark true for smoke 2, false for smoke 1
void queue_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    int32_t frame_cap,
    int32_t interval,
    int32_t hold,
    int32_t duration,
    uint16_t layer,
    bool dark
) {
    if (!fx_loaded(world, dark ? Fx::smoke_2 : Fx::smoke_1) || !take_emitter_slot(world))
        return;
    auto e = make_smoke_emitter(EmitterKind::smoke, game);
    init_smoke(world, e, game, host, position, frame_cap, interval, hold, duration, dark);
    queue_emitter(world, layer, e);
}

/// Creates a wake emitter, starts it and appends it to a layer.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param host random stream and terrain queries
/// @param from start point, 16.16 world coordinates
/// @param to point the pixels drift towards
/// @param period ticks per colour step
/// @param layer layer index 0..9
/// @param lit true to brighten through the ramp
void queue_wake(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    uint16_t layer,
    bool lit
) {
    if (!take_emitter_slot(world))
        return;
    auto e = make_emitter(EmitterKind::wake);
    if (init_wake(world, e, game, host, from, to, period, wake_lifetime, lit))
        queue_emitter(world, layer, e);
    else
        give_back_emitter_slot(world);
}

/// Creates a nano emitter between two boxes, starts it and appends it to a layer.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param host rand() stream
/// @param from source box
/// @param to target box
/// @param layer layer index 0..9
void queue_nano(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const Box& from,
    const Box& to,
    uint16_t layer
) {
    if (!take_emitter_slot(world))
        return;
    auto e = make_emitter(EmitterKind::nano);
    init_nano(world, e, game, host, from, to, nano_lifetime);
    queue_emitter(world, layer, e);
}

Box point_box(const FixedVec3& point) noexcept {
    return {point, point};
}

/// Creates a flame emitter, starts it and appends it to a layer.
///
/// Nothing happens when flamestream is not loaded or the emitter pool refuses.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param from start point, 16.16 world coordinates
/// @param to end point, 16.16 world coordinates
/// @param period ticks per animation frame
/// @param travel ticks to cover the distance
/// @param layer layer index 0..9
void queue_flame(
    EffectWorld& world,
    const oa::Game& game,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    int32_t travel,
    uint16_t layer
) {
    if (!fx_loaded(world, Fx::flamestream) || !take_emitter_slot(world))
        return;
    auto e = make_emitter(EmitterKind::flame);
    if (init_flame(world, e, game, from, to, period, travel))
        queue_emitter(world, layer, e);
    else
        give_back_emitter_slot(world);
}

/// Creates a teleport trail emitter, starts it and appends it to a layer.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param host rand() stream
/// @param from old position, 16.16 world coordinates
/// @param to new position, 16.16 world coordinates
/// @param lifetime ticks the trail keeps spawning
/// @param layer layer index 0..9
void queue_trail(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t lifetime,
    uint16_t layer
) {
    if (!fx_loaded(world, Fx::flamestream) || !take_emitter_slot(world))
        return;
    auto e = make_emitter(EmitterKind::teleport_trail);
    if (init_trail(world, e, game, host, from, to, lifetime))
        queue_emitter(world, layer, e);
    else
        give_back_emitter_slot(world);
}

/// Creates a feature smoke emitter, starts it and appends it to a layer.
///
/// @param[in,out] world layers and pools
/// @param game current tick
/// @param host rand() stream
/// @param position where the puffs start, 16.16 world coordinates
/// @param interval ticks between puffs
/// @param duration ticks until the deadline
/// @param layer layer index 0..9
void queue_feature_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    int32_t interval,
    int32_t duration,
    uint16_t layer
) {
    if (!fx_loaded(world, Fx::smoke_1) || !take_emitter_slot(world))
        return;
    auto e = make_smoke_emitter(EmitterKind::feature_smoke, game);
    init_feature_smoke(world, e, game, host, position, interval, 0, duration);
    queue_emitter(world, layer, e);
}
} // namespace

const char* fx_name(Fx entry) noexcept {
    static constexpr const char* names[fx_count]{
        "smoke 1",
        "smoke 2",
        "fire1",
        "alfboom1",
        "radlogo",
        "radlogohigh",
        "nuclogo",
        "h2oboom2",
        "lavasplash",
        "flamestream",
        "explosion",
        "explode2",
        "explode3",
        "explode4",
        "explode5",
        "nuke1",
        "shadow"
    };
    const auto index = static_cast<uint32_t>(entry);
    return index < fx_count ? names[index] : "";
}

int32_t step_trail_particle(TrailParticle& p) noexcept {
    p.position = add(p.position, p.delta);
    const auto next = static_cast<int32_t>(static_cast<uint32_t>(p.frame) + 1U);
    p.frame = next % p.period;
    return next / p.period;
}

int32_t step_wake_particle(WakeParticle& p) noexcept {
    p.position = add(p.position, p.delta);
    const auto next = static_cast<int32_t>(static_cast<uint32_t>(p.phase) + 1U);
    const auto quotient = next / p.period;
    p.phase = next % p.period;
    if (p.phase != 0)
        return quotient;
    p.value = wrap_add(p.value, p.value_delta);
    if (p.value > p.high)
        p.value = p.low;
    if (p.value < p.low)
        p.value = p.high;
    return p.high;
}

EmitterBlock::EmitterBlock(std::size_t slot_count) noexcept
    : slots(slot_count != 0 ? new (std::nothrow) Emitter[slot_count] : nullptr),
      count(slots != nullptr ? slot_count : 0) {
}

EmitterBlock::EmitterBlock(const EmitterBlock& other) noexcept : EmitterBlock(other.count) {
    if (count != 0)
        std::copy(other.slots.get(), other.slots.get() + count, slots.get());
}

EmitterBlock& EmitterBlock::operator=(const EmitterBlock& other) noexcept {
    if (this != &other) {
        EmitterBlock copy(other);
        *this = std::move(copy);
    }
    return *this;
}

bool size_effect_world(EffectWorld& world, const data::limits::Effects& limits) noexcept {
    const uint32_t ring = layer_ring_slots(limits);
    for (auto& layer : world.layers)
        layer = {};
    world.evict_above = limits.queue;
    world.pool_capacity = limits.reserve;
    if (ring != world.layer_slots || world.emitter_block.count != std::size_t{layer_count} * ring)
        world.emitter_block = EmitterBlock(std::size_t{layer_count} * ring);
    world.layer_slots = world.emitter_block.count != 0 ? ring : 0;
    return world.layer_slots != 0;
}

const Emitter& layer_emitter(const EffectWorld& world, uint16_t layer, uint32_t index) noexcept {
    return world.emitter_block.slots[ring_slot(world, layer, index)];
}

Emitter& layer_emitter(EffectWorld& world, uint16_t layer, uint32_t index) noexcept {
    return world.emitter_block.slots[ring_slot(world, layer, index)];
}

void tick_particles(EffectWorld& world, const oa::Game& game, const EffectHost& host) {
    for (uint16_t layer_index = 0; layer_index < layer_count; ++layer_index) {
        auto& layer = world.layers[layer_index];
        if (layer.count == 0)
            continue;
        const auto slots = world.layer_slots;
        auto* ring = world.emitter_block.slots.get() + std::size_t{layer_index} * slots;
        auto read = layer.head;
        auto write = layer.head;
        uint32_t kept = 0;
        for (uint32_t index = 0; index < layer.count; ++index) {
            auto& emitter = ring[read];
            if (++read == slots)
                read = 0;
            if (finished(emitter, game)) {
                release_emitter(world, emitter);
                continue;
            }
            tick_emitter(world, emitter, game, host);
            if (kept != index)
                ring[write] = emitter;
            if (++write == slots)
                write = 0;
            ++kept;
        }
        layer.count = kept;
    }
}

void queue_emitter(EffectWorld& world, uint16_t layer_index, const Emitter& emitter) {
    if (layer_index >= layer_count || world.layer_slots == 0) {
        auto dropped = emitter;
        release_emitter(world, dropped);
        return;
    }
    auto& layer = world.layers[layer_index];
    if (layer.count > world.evict_above || layer.count == world.layer_slots) {
        release_emitter(world, layer_emitter(world, layer_index, 0));
        layer.head = static_cast<uint32_t>((uint64_t{layer.head} + 1) % world.layer_slots);
        --layer.count;
    }
    layer_emitter(world, layer_index, layer.count) = emitter;
    ++layer.count;
}

void spawn_flame(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost&,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    int32_t travel,
    uint16_t layer
) {
    queue_flame(world, game, from, to, period, travel, layer);
}

void spawn_wake_lit(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    uint16_t layer
) {
    queue_wake(world, game, host, from, to, period, layer, true);
}

void spawn_wake_unlit(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t period,
    uint16_t layer
) {
    queue_wake(world, game, host, from, to, period, layer, false);
}

void spawn_nano_outward(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& nozzle,
    const Box& target,
    uint16_t layer
) {
    queue_nano(world, game, host, point_box(nozzle), target, layer);
}

void spawn_nano_inward(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const Box& target,
    const FixedVec3& nozzle,
    uint16_t layer
) {
    queue_nano(world, game, host, target, point_box(nozzle), layer);
}

void spawn_teleport_trail(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& from,
    const FixedVec3& to,
    int32_t lifetime,
    uint16_t layer
) {
    queue_trail(world, game, host, from, to, lifetime, layer);
}

void spawn_smoke_column(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    int32_t interval,
    int32_t duration,
    uint16_t layer
) {
    queue_smoke(world, game, host, position, 0, interval, 0, duration, layer, false);
}

void spawn_white_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    uint16_t layer
) {
    queue_smoke(world, game, host, position, 0, 1, 0, 0, layer, false);
}

void spawn_black_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    uint16_t layer
) {
    queue_smoke(world, game, host, position, 0, 1, 0, 0, layer, true);
}

void spawn_start_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    uint16_t layer
) {
    queue_smoke(
        world, game, host, position, start_smoke_frames, 1, start_smoke_hold, 0, layer, false
    );
}

void spawn_spark(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    uint16_t layer
) {
    auto jittered = position;
    jittered.x = jitter_units(jittered.x, rand_scaled(host, spark_jitter_span) - 1);
    jittered.y = jitter_units(jittered.y, rand_scaled(host, spark_jitter_span) - 1);
    jittered.z = jitter_units(jittered.z, rand_scaled(host, spark_jitter_span) - 1);
    const auto travel = rand_scaled(host, spark_jitter_span) + 1;
    queue_flame(world, game, jittered, jittered, 1, travel, layer);
}

void spawn_feature_smoke(
    EffectWorld& world,
    const oa::Game& game,
    const EffectHost& host,
    const FixedVec3& position,
    uint16_t layer
) {
    queue_feature_smoke(
        world, game, host, position, feature_smoke_interval, feature_smoke_duration, layer
    );
}

void draw_layer(
    const EffectWorld& world, uint16_t layer_index, void* context, ParticleVisitor visit
) {
    if (layer_index >= layer_count || visit == nullptr)
        return;
    const auto& layer = world.layers[layer_index];
    for (uint32_t index = 0; index < layer.count; ++index)
        draw_emitter(world, layer_emitter(world, layer_index, index), context, visit);
}
} // namespace oa::sim::effect_particles
