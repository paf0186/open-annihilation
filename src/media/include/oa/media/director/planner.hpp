// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Plans a director script from a replayed game's timeline. The planner
// knows the whole game before it places the first shot, so the camera can
// arrive where something is about to happen.
//
// The camera follows the game's moments: the whole map settling onto the
// first commander; in the quiet parts of the game, turns of each player's
// side in order, turn_side_shots turns a side, each a commander and what it
// builds, a factory finishing its first units, a construction unit
// expanding the base or the base itself, framed at varied heights and
// drifting or pushing in slowly for 6 to 14 seconds, longer when the
// subject starts something, and now and then a wide drift from one side to
// the other; armed groups that are advancing on an enemy base, weighed by
// their value; where the most damage lands in the next seconds, weighted
// by what it destroys and held while the battle goes on; now and then
// long-range fire, framed wide from launch to impact, or a gun firing and
// its shell landing, cut to one after the other; a commander dying, a
// nuclear blast or another large one interrupting the shot; and the
// commander death under the commander-death victory rule, the first after
// which at most one side has a commander, held, then a pull back. Other
// rules and a game no commander death decides are followed to the end; a
// commander replaced in its slot by another of its player's on the same
// tick is no loss.
//
// Shots last min_shot_ticks to max_shot_ticks with at least min_cut_ticks
// between cuts; a new subject must score switch_ratio times the current
// one; a subject is never taken twice in a row, and a subject whose view
// overlaps a view left in the last memory_ticks scores less the more it
// overlaps and the more recently that view was left; units the viewer's
// view does not draw, and commanders under the sea, are never framed;
// subjects stay in the middle of the frame (safe_area_percent), placed
// within it so that the view shows as little water as it can, and a fight
// at sea scores less; the camera follows its subject only once it nears
// the edge of the view, a bounded step at a time; and zoom changes at most
// max_zoom_change_percent a second between follow shots.
//
// Near subjects are panned to (a quiet turn's only when the camera shows
// its middle already), and a view whose middle the one before shows, or
// that shows the middle of the one before, is cut to; so are a gun firing
// and its shell landing; other cuts dissolve, a cut to the other side of
// the map after wipe_spacing_ticks without a wipe wipes, a big moment
// comes in with a checkerboard, and the first action of the game and the
// ending fade through black.
//
// The planner uses integer arithmetic alone, so the same timeline gives the
// same script, byte for byte, on every platform.
#pragma once

#include "oa/formats/oascript.hpp"
#include "oa/media/director/timeline.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::media::director {

/// The shortest shot, in ticks: 5 seconds.
inline constexpr uint32_t min_shot_ticks = 150;
/// The longest shot, in ticks: 20 seconds.
inline constexpr uint32_t max_shot_ticks = 600;
/// The fewest ticks between two cuts: 4 seconds.
inline constexpr uint32_t min_cut_ticks = 120;
/// The fewest ticks between a cut and an interrupting moment's cut: 2 seconds.
inline constexpr uint32_t min_interrupt_ticks = 60;
/// How much a new subject must outscore the current one before the camera
/// switches: numerator over denominator, 1.5.
inline constexpr uint32_t switch_ratio_numerator = 3;
inline constexpr uint32_t switch_ratio_denominator = 2;
/// The ticks of one turn in the quiet parts of the game: 10 seconds.
inline constexpr uint32_t turn_ticks = 300;
/// The turns a side takes in a row before the camera crosses to the next.
inline constexpr uint32_t turn_side_shots = 2;
/// How long a view shown counts against a new subject whose view overlaps
/// it, in ticks after the view was left: 60 seconds.
inline constexpr uint32_t memory_ticks = 1800;
/// The look-ahead window a hot spot is scored over, in ticks from the
/// decision: 3 to 8 seconds.
inline constexpr uint32_t hot_window_first_ticks = 90;
inline constexpr uint32_t hot_window_last_ticks = 240;
/// How early the camera arrives before a hot spot's first event: 3 seconds.
inline constexpr uint32_t arrive_early_ticks = 90;
/// The ticks between two decisions: every sample.
inline constexpr uint32_t decision_period_ticks = sample_period_ticks;
/// The side of the grid squares events are gathered in, in map pixels.
inline constexpr int32_t hot_cell_pixels = 512;
/// The distance within which two advancing units belong to one group, map
/// pixels.
inline constexpr int32_t group_link_pixels = 400;
/// How far ahead an army's advance is measured, in ticks: 5 seconds.
inline constexpr uint32_t army_look_ahead_ticks = 150;
/// How far toward the enemy base a unit must move over army_look_ahead_ticks
/// to advance, in map pixels.
inline constexpr int32_t army_min_advance_pixels = 60;
/// The part of the frame, in percent of each side, subjects stay within.
inline constexpr uint32_t safe_area_percent = 60;
/// The most the view height changes in a second, in percent of itself.
inline constexpr uint32_t max_zoom_change_percent = 50;
/// The view height of a close shot, in map pixels.
inline constexpr int32_t close_view_height = 540;
/// The widest view a shot that is not a wide one takes, in map pixels.
inline constexpr int32_t max_subject_view_height = 1400;
/// The farthest a new subject may be for a pan rather than a cut, in
/// percent of the current view's width: a pan never sweeps far across the
/// map.
inline constexpr uint32_t pan_reach_percent = 75;
/// The dissolve a far cut takes, in milliseconds.
inline constexpr uint32_t far_cut_dissolve_ms = 500;
/// The wipe a cut to the other side of the map takes, in milliseconds.
inline constexpr uint32_t side_wipe_ms = 600;
/// The fewest ticks between two wipes: 2 minutes.
inline constexpr uint32_t wipe_spacing_ticks = 3600;
/// The checkerboard a big moment comes in with, in milliseconds.
inline constexpr uint32_t moment_checkerboard_ms = 500;
/// The fade through black of the first action and of the ending, in
/// milliseconds.
inline constexpr uint32_t phase_fade_ms = 1000;
/// A blast at least this wide is a big moment, in map pixels.
inline constexpr int32_t big_blast_area = 300;
/// A weapon range at least this long makes long-range fire, in map pixels.
inline constexpr int32_t long_range_pixels = 1200;
/// The ticks the deciding commander death is held after it happens.
inline constexpr uint32_t ending_hold_ticks = 150;
/// The ticks of the ending's pull back.
inline constexpr uint32_t ending_pull_back_ticks = 150;

/// What the planned script's output and input say.
struct PlannerSettings {
    std::string recording{}; ///< input.demo: the recording's name or path
    uint32_t width{oa::formats::oascript::default_width};
    uint32_t height{oa::formats::oascript::default_height};
    oa::formats::oascript::Decimal framerate{oa::formats::oascript::default_framerate};
    oa::formats::oascript::Decimal tickrate{oa::formats::oascript::default_tickrate};
};

/// A planned script and what the planner noted while planning it.
struct Plan {
    oa::formats::oascript::Script script{};
    std::vector<std::string> notes{}; ///< one line per moment found, in tick order
};

/// Plans a script from a timeline.
///
/// The script's shots cover the timeline from its first tick to its usable
/// end, which becomes director.endTick. Its output keeps the settings' size
/// and rates, one-minute chunks and no interface.
///
/// @param timeline the replay's timeline; its verdict must be finished
/// @param settings the script's input and output
/// @return the plan; a timeline with no unit gives one whole-map shot
[[nodiscard]] Plan plan_script(const Timeline& timeline, const PlannerSettings& settings);

} // namespace oa::media::director
