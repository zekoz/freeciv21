// SPDX-FileCopyrightText: 2022 Louis Moureaux
// SPDX-License-Identifier: GPL-3.0-or-later

#include "path_finder.h"

#include "actions.h"
#include "game.h"
#include "map.h"
#include "movement.h"
#include "path.h"
#include "tile.h"
#include "unit.h"
#include "unit_utils.h"
#include "world_object.h"

#include <map>
#include <queue>
#include <set>

namespace freeciv {

namespace detail {

/**
 * Returns `true` if the comparison with `other` would be unambiguous.
 */
bool cost::comparable(const cost &other) const
{
  // When the results of the expressions below are positive, this cost does
  // better than the other for this criteria. When it's negative, it's the
  // opposite.
  auto a = other.turns - turns;
  auto b = moves_left - other.moves_left;
  auto c = health - other.health;
  auto d = fuel_left - other.fuel_left;
  // For the comparison to be meaningful, all criteria must go in the same
  // direction.
  return (a <= 0 && b <= 0 && c <= 0 && d <= 0)
         || (a >= 0 && b >= 0 && c >= 0 && d >= 0);
}

/**
 * Compares for equality.
 */
bool cost::operator==(const cost &other) const
{
  return std::tie(turns, other.moves_left, other.health, other.fuel_left)
         == std::tie(other.turns, moves_left, health, fuel_left);
}

/**
 * Defines a strict ordering among costs.
 */
bool cost::operator<(const cost &other) const
{
  // To break ties, we prefer the unit with the most moves, then the
  // healthiest unit, then the unit with the most fuel. This is an arbitrary
  // choice.
  return std::tie(turns, other.moves_left, other.health, other.fuel_left)
         < std::tie(other.turns, moves_left, health, fuel_left);
}

/**
 * Creates a vertex starting from this one and executing an action. The
 * vertex state is initialized as a copy of this one except that the action
 * move cost is subtracted.
 */
vertex vertex::child_for_action(action_id action, const unit &probe,
                                const tile *target)
{
  auto ret = *this;
  ret.parent = this;
  ret.order.order = ORDER_PERFORM_ACTION;
  ret.order.action = action;
  ret.order.target = target->index;
  ret.order.dir = DIR8_ORIGIN;
  ret.cost.moves_left =
      unit_pays_mp_for_action(action_by_number(action), &probe);
  return ret;
}

/**
 * Checks whether two vertices are comparable, which is the case when one of
 * them is unambiguously "better" than the other. Vertices that are not
 * comparable should be considered distinct: this is the case, for instance,
 * of vertices at different locations. Comparability is not a transitive
 * property.
 */
bool vertex::comparable(const vertex &other) const
{
  return std::tie(location, loaded, moved)
             == std::tie(other.location, other.loaded, other.moved)
         && cost.comparable(other.cost);
}

/**
 * Ensures that `probe` reflects the properties of this vertex. Any property
 * of `probe` not used in path finding is left unchanged.
 */
void vertex::fill_probe(unit &probe) const
{
  probe.tile = location;
  probe.transporter = loaded;
  probe.client.transported_by = loaded ? loaded->id : -1;
  probe.moved = moved;
  probe.fuel = cost.fuel_left;
  probe.hp = cost.health;
  probe.moves_left = cost.moves_left;
}

/**
 * Equality comparator.
 */
bool vertex::operator==(const vertex &other) const
{
  return std::tie(location, loaded, moved, cost)
         == std::tie(other.location, loaded, moved, other.cost);
}

/**
 * Defines an ordering for the priority queue.
 */
bool vertex::operator>(const vertex &other) const
{
  return other.cost < cost;
}

} // namespace detail

/**
 * Constructor.
 */
path_finder::path_finder_private::path_finder_private(const ::unit *unit)
    : unit(*unit)
{
  insert_initial_vertex();
}

/**
 * Inserts the initial vertex, from which the search will be started.
 */
void path_finder::path_finder_private::insert_initial_vertex()
{
  // Insert the starting vertex
  auto v = detail::vertex{unit.tile,
                          unit.transporter,
                          unit.moved,
                          {0, 0, unit.moves_left, unit.hp, unit.fuel},
                          nullptr};
  queue.push(v);
  best_vertices.emplace(v.location, std::make_unique<detail::vertex>(v));
}

/**
 * Saves a new vertex for further processing if it is better than the current
 * vertex at the same location.
 */
void path_finder::path_finder_private::maybe_insert_vertex(
    const detail::vertex &v)
{
  // Make a copy because it may change before insertion
  auto insert = v;

  // Handle turn change
  if (v.cost.moves_left <= 0) {
    // Make a probe
    auto probe = unit;
    v.fill_probe(probe);

    // FIXME The order could be important here: fuel before HP or HP before
    // fuel?
    insert.cost.turns++;
    insert.cost.moves_left = unit_move_rate(&probe);

    // Fuel
    if (utype_fuel(probe.utype)) {
      if (is_unit_being_refueled(&probe)) {
        // Refuel
        probe.fuel = utype_fuel(probe.utype);
        insert.cost.fuel_left = probe.fuel;
      } else if (probe.fuel <= 1) {
        // The unit dies, don't generate a new vertex
        return;
      } else {
        // Consume fuel
        probe.fuel--;
        insert.cost.fuel_left--;
      }
    }

    // HP loss and recovery
    // Userful for helis, killunhomed, slow damaged units with fuel. A path
    // can require that the unit heals first. Of course, it will choose
    // barracks if you have them, because healing is faster there.
    unit_restore_hitpoints(&probe);
    if (probe.hp <= 0) {
      // Unit dies, don't let the user send it there.
      return;
    }
    insert.cost.health = probe.hp;

    // "Start of turn" part
    insert.moved = false; // Didn't move yet
  }

  // The remaining logic checks whether we should insert the new vertex. This
  // is complicated because the new candidate may be better than one or
  // several of the previous paths to the same tile. Also do some bookkeeping
  // so we only insert the new cost if it isn't already there.
  const auto [begin, end] = best_vertices.equal_range(v.location);
  bool do_insert = true;
  for (auto it = begin; it != end; /* in loop body */) {
    const bool comparable = it->second->comparable(insert);
    if (comparable && insert.cost < it->second->cost) {
      // The new candidate is strictly better. Remove the old one
      it = best_vertices.erase(it);
      continue; // ++it is done inside erase()
    } else if (comparable) {
      // We already have it (or something equivalent, or even something
      // better), no need to add it.
      do_insert = false;
      break;
    }
    ++it;
  }

  // Insert the new cost if needed
  if (do_insert) {
    queue.push(insert);
    best_vertices.emplace(v.location,
                          std::make_unique<detail::vertex>(insert));
  }
}

/**
 * Opens vertices corresponding to attempts to do ORDER_MOVE from the source
 * vertex.
 */
void path_finder::path_finder_private::attempt_move(detail::vertex &source)
{
  // Don't attempt to move loaded units
  if (source.loaded) {
    return;
  }

  // Make a probe
  auto probe = unit;
  source.fill_probe(probe);

  // Try moving to adjacent tiles
  adjc_dir_iterate(&(wld.map), source.location, target, dir)
  {
    if (target->terrain == nullptr) {
      // Can't see this tile
      continue;
    }
    if (unit_can_move_to_tile(&wld.map, &probe, target, false, false)) {
      auto move_cost = std::min(map_move_cost_unit(&wld.map, &probe, target),
                                probe.moves_left);

      // Construct the next vertex
      auto next = source;
      next.location = target;
      next.moved = true;
      next.cost.moves_left -= move_cost;
      next.parent = &source;
      next.order.order = ORDER_MOVE;
      next.order.dir = dir;
      maybe_insert_vertex(next);
    }
  }
  adjc_dir_iterate_end;
}

/**
 * Opens vertices corresponding to attempts to do ORDER_FULL_MP from the
 * source vertex. This is a last resort vertex that may give the unit more HP
 * or fuel that will be useful to continue its journey.
 */
void path_finder::path_finder_private::attempt_full_mp(
    detail::vertex &source)
{
  auto next = source;
  next.cost.moves_left = 0; // Trigger end-of-turn logic
  next.parent = &source;
  next.order.order = ORDER_FULL_MP;
  maybe_insert_vertex(next);
}

/**
 * Opens vertices corresponding to attempts to load into a transport from the
 * source vertex.
 */
void path_finder::path_finder_private::attempt_load(detail::vertex &source)
{
  // Make a probe
  auto probe = unit;
  source.fill_probe(probe);

  // Try to load into a transport -- even if we're already in a transport
  // Same tile (maybe we can recover HP)
  if (auto transport = transporter_for_unit(&probe);
      transport != nullptr
      && is_action_enabled_unit_on_unit(ACTION_TRANSPORT_BOARD, &probe,
                                        transport)) {
    auto next =
        source.child_for_action(ACTION_TRANSPORT_BOARD, probe, probe.tile);
    next.loaded = transport;
    maybe_insert_vertex(next);
  }

  // Nearby tiles
  adjc_iterate(&(wld.map), probe.tile, target)
  {
    probe.tile = target;
    auto transport = transporter_for_unit(&probe);
    // Reset the probe -- needed now because is_action_enabled_unit_on_unit
    // checks the range
    probe.tile = source.location;
    if (transport != nullptr
        && is_action_enabled_unit_on_unit(ACTION_TRANSPORT_EMBARK, &probe,
                                          transport)) {
      auto next =
          source.child_for_action(ACTION_TRANSPORT_EMBARK, probe, target);
      // See unithand.cpp:do_unit_embark
      next.cost.moves_left -= map_move_cost_unit(&(wld.map), &probe, target);
      next.moved = true;
      next.loaded = transport;
      maybe_insert_vertex(next);
    }
  }
  adjc_iterate_end;
}

/**
 * Opens vertices corresponding to attempts to unload from a transport at the
 * source vertex.
 */
void path_finder::path_finder_private::attempt_unload(detail::vertex &source)
{
  // Make a probe
  auto probe = unit;
  source.fill_probe(probe);

  // Try to unload from a transport -- but only if we're already loaded
  if (probe.transporter != nullptr) {
    // Same tile
    if (is_action_enabled_unit_on_unit(ACTION_TRANSPORT_ALIGHT, &probe,
                                       probe.transporter)) {
      auto next = source.child_for_action(ACTION_TRANSPORT_ALIGHT, probe,
                                          probe.tile);
      next.loaded = nullptr;
      maybe_insert_vertex(next);
    }

    // Nearby tiles
    adjc_iterate(&(wld.map), probe.tile, target)
    {
      if (is_action_enabled_unit_on_tile(ACTION_TRANSPORT_DISEMBARK1, &probe,
                                         target, nullptr)) {
        auto next = source.child_for_action(ACTION_TRANSPORT_DISEMBARK1,
                                            probe, target);
        next.moved = true;
        next.loaded = nullptr;
        // See unithand.cpp:do_disembark
        next.cost.moves_left -=
            map_move_cost_unit(&(wld.map), &probe, target);
        maybe_insert_vertex(next);
      }
      // Thanks sveinung
      if (is_action_enabled_unit_on_tile(ACTION_TRANSPORT_DISEMBARK2, &probe,
                                         target, nullptr)) {
        auto next = source.child_for_action(ACTION_TRANSPORT_DISEMBARK2,
                                            probe, target);
        next.moved = true;
        next.loaded = nullptr;
        // See unithand.cpp:do_disembark
        next.cost.moves_left -=
            map_move_cost_unit(&(wld.map), &probe, target);
        maybe_insert_vertex(next);
      }
    }
    adjc_iterate_end;
  }
}

/**
 * Runs the path finding seach until the stopping condition is met (the
 * destination tile is reached). Checks if the tile has already been reached
 * before proceeding.
 *
 * \returns true if a path was found.
 */
bool path_finder::path_finder_private::run_search(
    const tile *stopping_condition)
{
  // Check if we've already found a path (but keep searching if the tip of
  // the queue is cheaper: we haven't checked every possibility).
  if (auto it = best_vertices.find(stopping_condition);
      it != best_vertices.end() && !(queue.top().cost < it->second->cost)) {
    return true;
  }

  // What follows is an implementation of Dijkstra's path finding algorithm.
  while (!queue.empty()) {
    // Get the "best" vertex
    const auto v = queue.top();

    // Check if we just arrived
    // Keep the node in the queue so adjacent nodes are generated if the
    // search needs to be expanded later.
    if (v.location == stopping_condition) {
      return true;
    }

    queue.pop(); // Remove it from the queue

    // An equivalent (or better) vertex may already have been processed.
    // Check that we have one of the "current best" vertices for that tile.
    const auto [begin, end] = best_vertices.equal_range(v.location);
    const auto it = std::find_if(
        begin, end, [&v](const auto &pair) { return *pair.second == v; });
    if (it == best_vertices.end()) {
      // Not found, we processed something else in the meantime. Since we
      // processed it earlier, that path was at least as short.
      continue;
    }

    // Fetch the pointer version of v for use as a parent
    auto parent = it->second.get();

    // Generate vertices starting from this one
    attempt_move(*parent);
    attempt_full_mp(*parent);
    attempt_load(*parent);
    attempt_unload(*parent);
  }

  return false;
}

/**
 * Constructs a @c path_finder for the given unit and destination. Doesn't
 * start the path finding yet.
 *
 * The path finder becomes useless if the unit moves.
 */
path_finder::path_finder(const ::unit *unit)
    : m_d(std::make_unique<path_finder_private>(unit))
{
}

/**
 * Destructor.
 */
path_finder::~path_finder()
{
  // Make sure that the destructor of path_finder_private is known.
}

/**
 * Notifies the path finder that some unit died or changed state. In many
 * cases, this will trigger a recalculation of the path.
 */
void path_finder::unit_changed(const ::unit &unit)
{
  Q_UNUSED(unit);

  // We can try to be smarter later. For now, just invalidate everything.
  m_d->best_vertices.clear();
  while (!m_d->queue.empty()) {
    m_d->queue.pop();
  }
  m_d->insert_initial_vertex();
}

/**
 * Runs the path finding algorithm.
 */
path path_finder::find_path(const tile *destination)
{
  fc_assert_ret_val(destination != nullptr, path());

  // Unit frozen by scenario
  if (m_d->unit.stay) {
    return path();
  }

  // Already at the destination
  if (m_d->unit.tile == destination) {
    return path();
  }

  if (m_d->run_search(destination)) {
    // Find the best path. We may have several vertices, so select the one
    // with the lowest cost.
    const auto [begin, end] = m_d->best_vertices.equal_range(destination);
    const detail::vertex *best = nullptr;
    for (auto it = begin; it != end; ++it) {
      if (best == nullptr || it->second->cost < best->cost) {
        best = it->second.get();
      }
    }

    // Build a path
    auto steps = std::vector<path::step>();
    for (auto vertex = best; vertex->parent != nullptr;
         vertex = vertex->parent) {
      steps.push_back({vertex->location, vertex->cost.turns,
                       vertex->cost.turns, vertex->order});
    }

    return path(std::vector<path::step>(steps.rbegin(), steps.rend()));
  } else {
    return path();
  }
}

} // namespace freeciv
