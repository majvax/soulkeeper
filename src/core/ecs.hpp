// src/core/ecs.hpp
//
// A lightweight, cache-friendly Entity Component System built around sparse
// sets. Component storage is fully dense (a single flat std::vector<T> per
// component type), so iteration walks contiguous memory — the data-oriented
// layout we want for tens of thousands of active entities.
//
// Design notes:
//   * Entities are versioned 32-bit handles: [ index : 20 ][ version : 12 ].
//     Recycling an index bumps its version, so a stale handle to a destroyed
//     entity is cheaply detectable via valid().
//   * Each component type owns a ComponentPool<T> (a typed SparseSet). The
//     sparse array maps entity-index -> dense slot; the dense arrays
//     (entities + components) stay packed via swap-and-pop on removal.
//   * Registry erases type via a SparseSet base so it can wipe an entity from
//     every pool polymorphically on destroy().

#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace core {

// ---------------------------------------------------------------------------
// Entity handle: packed [ index : 20 ][ version : 12 ]
// ---------------------------------------------------------------------------
using Entity = std::uint32_t;

inline constexpr std::uint32_t entity_index_bits = 20u;
inline constexpr std::uint32_t entity_version_bits = 12u;
static_assert(entity_index_bits + entity_version_bits == 32u);

inline constexpr std::uint32_t entity_index_mask = (1u << entity_index_bits) - 1u;
inline constexpr std::uint32_t entity_version_mask = (1u << entity_version_bits) - 1u;

// Largest representable index doubles as the "no entity" / empty-slot sentinel.
inline constexpr std::uint32_t null_index = entity_index_mask;
inline constexpr Entity null_entity = null_index; // version 0, sentinel index

[[nodiscard]] constexpr std::uint32_t entity_index(Entity e) noexcept {
    return e & entity_index_mask;
}

[[nodiscard]] constexpr std::uint32_t entity_version(Entity e) noexcept {
    return (e >> entity_index_bits) & entity_version_mask;
}

[[nodiscard]] constexpr Entity make_entity(std::uint32_t index, std::uint32_t version) noexcept {
    return (index & entity_index_mask) | ((version & entity_version_mask) << entity_index_bits);
}

// A component is any object type we can move into dense storage.
template <typename T>
concept Component = std::is_object_v<T> && std::movable<T>;

// ---------------------------------------------------------------------------
// SparseSet: type-erased membership container.
// ---------------------------------------------------------------------------
// dense_ holds the live entity handles (packed, iterable order). sparse_ is
// indexed by entity-index and stores the slot inside dense_, or `tombstone`
// when the entity is absent. Membership is O(1); removal is swap-and-pop to
// keep dense_ contiguous.
class SparseSet {
public:
    static constexpr std::uint32_t tombstone = std::numeric_limits<std::uint32_t>::max();

    SparseSet() = default;
    virtual ~SparseSet() = default;

    SparseSet(const SparseSet&) = delete;
    SparseSet& operator=(const SparseSet&) = delete;
    SparseSet(SparseSet&&) = default;
    SparseSet& operator=(SparseSet&&) = default;

    [[nodiscard]] bool contains(Entity e) const noexcept {
        const std::uint32_t idx = entity_index(e);
        return idx < sparse_.size() && sparse_[idx] != tombstone;
    }

    [[nodiscard]] std::size_t size() const noexcept { return dense_.size(); }
    [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }

    // Iterate the packed live entities.
    [[nodiscard]] auto begin() const noexcept { return dense_.begin(); }
    [[nodiscard]] auto end() const noexcept { return dense_.end(); }

    // Erase `e` from this set if present. Pure base hook so Registry can wipe
    // an entity from every pool without knowing the component type.
    virtual void remove(Entity e) = 0;

protected:
    // Reserve a dense slot for `e`, returning its position. Caller is expected
    // to have checked !contains(e). Grows sparse_ as needed.
    std::uint32_t emplace_slot(Entity e) {
        const std::uint32_t idx = entity_index(e);
        if (idx >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(idx) + 1u, tombstone);
        }
        const auto slot = static_cast<std::uint32_t>(dense_.size());
        sparse_[idx] = slot;
        dense_.push_back(e);
        return slot;
    }

    // Remove the entity at the given dense slot via swap-and-pop, fixing up the
    // sparse entry of whatever entity gets swapped into its place. Returns the
    // index of the element that was moved into `slot` (or tombstone if the
    // removed element was already last), so derived pools can mirror the move.
    std::uint32_t remove_slot(Entity e, std::uint32_t slot) {
        const std::uint32_t last = static_cast<std::uint32_t>(dense_.size()) - 1u;
        std::uint32_t moved = tombstone;
        if (slot != last) {
            const Entity moved_entity = dense_[last];
            dense_[slot] = moved_entity;
            sparse_[entity_index(moved_entity)] = slot;
            moved = last;
        }
        dense_.pop_back();
        sparse_[entity_index(e)] = tombstone;
        return moved;
    }

    [[nodiscard]] std::uint32_t slot_of(Entity e) const noexcept {
        return sparse_[entity_index(e)];
    }

private:
    std::vector<Entity> dense_;
    std::vector<std::uint32_t> sparse_;
};

// ---------------------------------------------------------------------------
// ComponentPool<T>: a SparseSet plus a parallel dense array of components.
// ---------------------------------------------------------------------------
// components_[i] belongs to the entity at dense slot i, so iterating
// components_ is a contiguous walk. Removal mirrors the base's swap-and-pop.
template <Component T>
class ComponentPool final : public SparseSet {
public:
    template <typename... Args>
    T& emplace(Entity e, Args&&... args) pre(!contains(e)) {
        emplace_slot(e);
        return components_.emplace_back(std::forward<Args>(args)...);
    }

    void remove(Entity e) override {
        if (!contains(e)) {
            return;
        }
        const std::uint32_t slot = slot_of(e);
        const std::uint32_t moved = remove_slot(e, slot);
        if (moved != tombstone) {
            components_[slot] = std::move(components_.back());
        }
        components_.pop_back();
    }

    [[nodiscard]] T& get(Entity e) noexcept pre(contains(e)) {
        return components_[slot_of(e)];
    }

    [[nodiscard]] const T& get(Entity e) const noexcept pre(contains(e)) {
        return components_[slot_of(e)];
    }

    [[nodiscard]] T* try_get(Entity e) noexcept {
        return contains(e) ? &components_[slot_of(e)] : nullptr;
    }

    [[nodiscard]] const T* try_get(Entity e) const noexcept {
        return contains(e) ? &components_[slot_of(e)] : nullptr;
    }

private:
    std::vector<T> components_;
};

// ---------------------------------------------------------------------------
// DynamicPool: a SparseSet whose per-entity record is a flat run of `stride`
// doubles. Used for Lua-defined (script) components — the field layout isn't a
// C++ type, so storage is schema-driven at runtime. Cache-friendly (contiguous
// rows) and trivial to serialize.
// ---------------------------------------------------------------------------
class DynamicPool final : public SparseSet {
public:
    explicit DynamicPool(std::uint32_t stride) : stride_{stride} {}

    // Insert a zero-initialised row for `e` (or return the existing one).
    double* emplace_default(Entity e) {
        if (contains(e)) {
            return row(e);
        }
        emplace_slot(e);
        const std::size_t base = fields_.size();
        fields_.resize(base + stride_, 0.0);
        return &fields_[base];
    }

    void remove(Entity e) override {
        if (!contains(e)) {
            return;
        }
        const std::uint32_t slot = slot_of(e);
        const std::uint32_t moved = remove_slot(e, slot);
        if (moved != tombstone) {
            // Mirror the base's swap-and-pop: copy the last row into `slot`.
            const std::size_t last = fields_.size() - stride_;
            std::copy(fields_.begin() + static_cast<std::ptrdiff_t>(last), fields_.end(),
                      fields_.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(slot) * stride_));
        }
        fields_.resize(fields_.size() - stride_);
    }

    // Pointer to the entity's `stride` doubles, or nullptr if absent.
    [[nodiscard]] double* row(Entity e) noexcept {
        return contains(e) ? &fields_[static_cast<std::size_t>(slot_of(e)) * stride_] : nullptr;
    }

    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

private:
    std::vector<double> fields_;
    std::uint32_t stride_;
};

// ---------------------------------------------------------------------------
// Stable, monotonic component-type ids (used to index the pool table).
// ---------------------------------------------------------------------------
namespace detail {
[[nodiscard]] inline std::uint32_t next_type_id() noexcept {
    static std::uint32_t counter = 0u;
    return counter++;
}
} // namespace detail

template <Component T>
[[nodiscard]] inline std::uint32_t type_id() noexcept {
    static const std::uint32_t id = detail::next_type_id();
    return id;
}

// ---------------------------------------------------------------------------
// View: iterate entities owning all of Ts..., driven by the smallest pool.
// ---------------------------------------------------------------------------
template <Component... Ts>
class View {
public:
    explicit View(ComponentPool<Ts>*... pools) noexcept : pools_{pools...} {}

    // Invoke fn(Entity, Ts&...) for every entity present in all pools. We walk
    // the smallest pool and probe the rest, so the hot loop is proportional to
    // the rarest component.
    template <typename Fn>
    void each(Fn&& fn) const {
        const SparseSet* lead = smallest();
        if (lead == nullptr) {
            return;
        }
        for (const Entity e : *lead) {
            if ((std::get<ComponentPool<Ts>*>(pools_)->contains(e) && ...)) {
                fn(e, std::get<ComponentPool<Ts>*>(pools_)->get(e)...);
            }
        }
    }

private:
    [[nodiscard]] const SparseSet* smallest() const noexcept {
        const SparseSet* result = nullptr;
        // Fold over the pools, keeping the one with the fewest elements. A null
        // pool means a component type was never used -> the view is empty.
        bool any_null = false;
        auto consider = [&](const SparseSet* s) {
            if (s == nullptr) {
                any_null = true;
            } else if (result == nullptr || s->size() < result->size()) {
                result = s;
            }
        };
        (consider(std::get<ComponentPool<Ts>*>(pools_)), ...);
        return any_null ? nullptr : result;
    }

    std::tuple<ComponentPool<Ts>*...> pools_;
};

// ---------------------------------------------------------------------------
// Registry: owns entities and the per-type component pools.
// ---------------------------------------------------------------------------
class Registry {
public:
    // Allocate an entity, recycling a freed index (with a bumped version) when
    // one is available.
    [[nodiscard]] Entity create() {
        if (!free_list_.empty()) {
            const std::uint32_t idx = free_list_.back();
            free_list_.pop_back();
            return make_entity(idx, versions_[idx]);
        }
        const auto idx = static_cast<std::uint32_t>(versions_.size());
        contract_assert(idx < null_index); // entity index space exhausted
        versions_.push_back(0u);
        return make_entity(idx, 0u);
    }

    // True while `e` refers to the currently-live generation of its index.
    [[nodiscard]] bool valid(Entity e) const noexcept {
        const std::uint32_t idx = entity_index(e);
        return idx < versions_.size() && versions_[idx] == entity_version(e);
    }

    // Destroy `e`: strip it from every pool, then recycle its index with a
    // bumped version so old handles stop validating.
    void destroy(Entity e) pre(valid(e)) {
        for (auto& pool : pools_) {
            if (pool) {
                pool->remove(e);
            }
        }
        for (auto& pool : dynamic_pools_) { // script components, wiped too
            if (pool) {
                pool->remove(e);
            }
        }
        const std::uint32_t idx = entity_index(e);
        versions_[idx] = (versions_[idx] + 1u) & entity_version_mask;
        free_list_.push_back(idx);
    }

    template <Component T, typename... Args>
    T& emplace(Entity e, Args&&... args) pre(valid(e)) {
        return pool<T>().emplace(e, std::forward<Args>(args)...);
    }

    // Convenience overload matching the Task 2 `assign<T>(Entity, T&&)` signature.
    template <Component T>
    T& assign(Entity e, T&& component) {
        return emplace<std::remove_cvref_t<T>>(e, std::forward<T>(component));
    }

    template <Component T>
    void remove(Entity e) pre(valid(e)) {
        if (SparseSet* p = find_pool<T>()) {
            p->remove(e);
        }
    }

    template <Component T>
    [[nodiscard]] bool has(Entity e) const noexcept {
        const ComponentPool<T>* p = find_pool<T>();
        return p != nullptr && p->contains(e);
    }

    template <Component T>
    [[nodiscard]] T& get(Entity e) noexcept pre(has<T>(e)) {
        return pool<T>().get(e);
    }

    template <Component T>
    [[nodiscard]] const T& get(Entity e) const noexcept pre(has<T>(e)) {
        return find_pool<T>()->get(e);
    }

    template <Component T>
    [[nodiscard]] T* try_get(Entity e) noexcept {
        ComponentPool<T>* p = find_pool<T>();
        return p ? p->try_get(e) : nullptr;
    }

    template <Component T>
    [[nodiscard]] const T* try_get(Entity e) const noexcept {
        const ComponentPool<T>* p = find_pool<T>();
        return p ? p->try_get(e) : nullptr;
    }

    // Build a view over all entities owning every one of Ts....
    template <Component... Ts>
    [[nodiscard]] View<Ts...> view() {
        return View<Ts...>{&pool<Ts>()...};
    }

    [[nodiscard]] std::size_t alive_count() const noexcept {
        return versions_.size() - free_list_.size();
    }

    // --- runtime / script-component access (for the Lua scripting-ECS) --------
    // Erased access to a typed component pool by its type_id<T>() (nullptr if the
    // pool was never created). Lets runtime queries test membership generically.
    [[nodiscard]] SparseSet* raw_pool(std::uint32_t type) noexcept {
        return (type < pools_.size()) ? pools_[type].get() : nullptr;
    }

    // Create a dynamic (script) component pool with `stride` double fields,
    // returning its id. The pool is wiped on destroy() like any other.
    std::uint32_t create_dynamic_pool(std::uint32_t stride) {
        const auto id = static_cast<std::uint32_t>(dynamic_pools_.size());
        dynamic_pools_.push_back(std::make_unique<DynamicPool>(stride));
        return id;
    }

    [[nodiscard]] DynamicPool* dynamic_pool(std::uint32_t id) noexcept {
        return (id < dynamic_pools_.size()) ? static_cast<DynamicPool*>(dynamic_pools_[id].get()) : nullptr;
    }

private:
    // Fetch (lazily creating) the pool for T.
    template <Component T>
    ComponentPool<T>& pool() {
        const std::uint32_t id = type_id<T>();
        if (id >= pools_.size()) {
            pools_.resize(static_cast<std::size_t>(id) + 1u);
        }
        if (!pools_[id]) {
            pools_[id] = std::make_unique<ComponentPool<T>>();
        }
        return *static_cast<ComponentPool<T>*>(pools_[id].get());
    }

    // Fetch the pool for T without creating it (nullptr if it doesn't exist).
    template <Component T>
    [[nodiscard]] ComponentPool<T>* find_pool() noexcept {
        const std::uint32_t id = type_id<T>();
        if (id >= pools_.size() || !pools_[id]) {
            return nullptr;
        }
        return static_cast<ComponentPool<T>*>(pools_[id].get());
    }

    template <Component T>
    [[nodiscard]] const ComponentPool<T>* find_pool() const noexcept {
        const std::uint32_t id = type_id<T>();
        if (id >= pools_.size() || !pools_[id]) {
            return nullptr;
        }
        return static_cast<const ComponentPool<T>*>(pools_[id].get());
    }

    std::vector<std::uint32_t> versions_;                    // version per entity index
    std::vector<std::uint32_t> free_list_;                   // recyclable indices
    std::vector<std::unique_ptr<SparseSet>> pools_;          // indexed by type_id<T>()
    std::vector<std::unique_ptr<SparseSet>> dynamic_pools_;  // script components, by dynamic id
};

} // namespace core
