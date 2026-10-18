// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/search_heap.hpp"

#include <bit>
#include <limits>

namespace oa::sim::ground_orders {

int32_t SearchHeap::score(const Node& value) noexcept {
    return std::bit_cast<int32_t>(value.value[2]);
}

SearchHeap::Node& SearchHeap::node(Handle handle) noexcept {
    if (handle < nodes_.size())
        return nodes_[handle];
    spare_ = {};
    return spare_;
}

const SearchHeap::Node& SearchHeap::node(Handle handle) const noexcept {
    static const Node no_node{};
    return handle < nodes_.size() ? nodes_[handle] : no_node;
}

SearchHeap::Payload& SearchHeap::payload(Handle handle) noexcept {
    return node(handle).value;
}

const SearchHeap::Payload& SearchHeap::payload(Handle handle) const noexcept {
    return node(handle).value;
}

std::size_t SearchHeap::position(Handle handle) const noexcept {
    const auto value = node(handle).heap_position_or_next_free;
    if (value < 0 || static_cast<std::size_t>(value) >= heap_.size() ||
        heap_[static_cast<std::size_t>(value)] != handle)
        return no_position;
    return static_cast<std::size_t>(value);
}

void SearchHeap::reset_open_set() {
    nodes_.clear();
    heap_.clear();
    free_head_ = -1;
    deferred_pop_ = false;
}

bool SearchHeap::reserve(int32_t requested_capacity) {
    std::size_t next{};
    if (requested_capacity < 0 || static_cast<std::size_t>(requested_capacity) < capacity_) {
        if (capacity_ > (std::numeric_limits<std::size_t>::max() - 16) / 3 * 2)
            return false;
        next = capacity_ + capacity_ / 2 + 16;
    } else {
        next = static_cast<std::size_t>(requested_capacity);
    }
    if (next < nodes_.size())
        return false;
    nodes_.reserve(next);
    heap_.reserve(next);
    capacity_ = next;
    return true;
}

SearchHeap::Handle SearchHeap::allocate_slot() {
    if (free_head_ == -1) {
        if (nodes_.size() >= no_handle)
            return no_handle;
        const auto result = static_cast<Handle>(nodes_.size());
        nodes_.emplace_back();
        return result;
    }
    const auto result = static_cast<Handle>(free_head_);
    free_head_ = node(result).heap_position_or_next_free;
    return result;
}

void SearchHeap::sift_up(std::size_t position_value) {
    if (position_value == 0)
        return;
    const auto moving = heap_[position_value];
    const auto moving_score = score(nodes_[moving]);
    auto parent = (position_value - 1) / 2;
    if (moving_score >= score(nodes_[heap_[parent]]))
        return;
    heap_[position_value] = heap_[parent];
    nodes_[heap_[position_value]].heap_position_or_next_free = static_cast<int32_t>(position_value);
    while (parent != 0) {
        const auto next_parent = (parent - 1) / 2;
        if (moving_score >= score(nodes_[heap_[next_parent]]))
            break;
        heap_[parent] = heap_[next_parent];
        nodes_[heap_[parent]].heap_position_or_next_free = static_cast<int32_t>(parent);
        parent = next_parent;
    }
    heap_[parent] = moving;
    nodes_[moving].heap_position_or_next_free = static_cast<int32_t>(parent);
}

void SearchHeap::sift_down(std::size_t position_value) {
    const auto moving = heap_[position_value];
    const auto moving_score = score(nodes_[moving]);
    while (true) {
        const auto left = position_value * 2 + 1;
        if (left >= heap_.size())
            break;
        const auto right = left + 1;
        auto selected = left;
        // The left child is picked when the scores compare equal.
        if (right < heap_.size() && score(nodes_[heap_[right]]) < score(nodes_[heap_[left]]))
            selected = right;
        if (moving_score <= score(nodes_[heap_[selected]]))
            break;
        heap_[position_value] = heap_[selected];
        nodes_[heap_[position_value]].heap_position_or_next_free =
            static_cast<int32_t>(position_value);
        position_value = selected;
    }
    heap_[position_value] = moving;
    nodes_[moving].heap_position_or_next_free = static_cast<int32_t>(position_value);
}

void SearchHeap::remove_slot(Handle handle) {
    const auto removed_position = position(handle);
    if (removed_position == no_position)
        return;
    node(handle).heap_position_or_next_free = free_head_;
    free_head_ = static_cast<int32_t>(handle);
    const auto last = heap_.back();
    heap_.pop_back();
    if (removed_position < heap_.size()) {
        heap_[removed_position] = last;
        node(last).heap_position_or_next_free = static_cast<int32_t>(removed_position);
        sift_down(removed_position);
    }
}

SearchHeap::Handle SearchHeap::insert(const Payload& value) {
    // A second pop can empty the heap with a pop still pending; the insert
    // then starts a new root.
    if (deferred_pop_ && heap_.empty())
        deferred_pop_ = false;
    if (deferred_pop_) {
        const auto result = heap_.front();
        node(result).value = value;
        sift_down(0);
        deferred_pop_ = false;
        return result;
    }
    if (heap_.size() == capacity_)
        (void)reserve(-1);
    const auto result = allocate_slot();
    if (result == no_handle)
        return no_handle;
    const auto position_value = heap_.size();
    node(result).value = value;
    node(result).heap_position_or_next_free = static_cast<int32_t>(position_value);
    heap_.push_back(result);
    sift_up(position_value);
    return result;
}

void SearchHeap::decrease_key(Handle handle) {
    const auto handle_position = position(handle);
    if (handle_position == no_position)
        return;
    if (deferred_pop_) {
        const auto deferred_root = heap_.front();
        sift_up(handle_position);
        if (node(deferred_root).heap_position_or_next_free != 0) {
            deferred_pop_ = false;
            remove_slot(deferred_root);
        }
        return;
    }
    sift_up(handle_position);
}

const SearchHeap::Payload& SearchHeap::peek() {
    if (deferred_pop_) {
        deferred_pop_ = false;
        if (!heap_.empty())
            remove_slot(heap_.front());
    }
    static const Payload no_payload{};
    if (heap_.empty())
        return no_payload;
    return node(heap_.front()).value;
}

void SearchHeap::pop() {
    if (heap_.empty())
        return;
    if (!deferred_pop_) {
        deferred_pop_ = true;
        return;
    }
    remove_slot(heap_.front());
}

} // namespace oa::sim::ground_orders
