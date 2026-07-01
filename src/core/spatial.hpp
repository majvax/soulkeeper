#pragma once
#include "core/ecs.hpp"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace core {

// Uniform-grid spatial hash for broad-phase queries. Bucket entities into fixed
// cells, then query an AABB to get the handful of entities that might overlap a
// region instead of scanning everything. Rebuilt each tick by the caller.
class SpatialGrid
{
public:
    explicit SpatialGrid(float cell_size) : cell_size_{ cell_size } {}

    void insert(Entity entity, float x, float y)
    {
        cells_[key(cell(x), cell(y))].push_back(entity);
    }

    // Entities in every cell overlapping the [left,top]..[right,bottom] box.
    [[nodiscard]] std::vector<Entity> query(float left, float top, float right, float bottom) const
    {
        std::vector<Entity> result;
        for (std::int32_t cy = cell(top); cy <= cell(bottom); ++cy) {
            for (std::int32_t cx = cell(left); cx <= cell(right); ++cx) {
                if (const auto it = cells_.find(key(cx, cy)); it != cells_.end()) {
                    result.insert(result.end(), it->second.begin(), it->second.end());
                }
            }
        }
        return result;
    }

private:
    [[nodiscard]] std::int32_t cell(float v) const
    {
        return static_cast<std::int32_t>(std::floor(v / cell_size_));
    }

    static std::uint64_t key(std::int32_t cx, std::int32_t cy)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
             | static_cast<std::uint32_t>(cy);
    }

    float cell_size_;
    std::unordered_map<std::uint64_t, std::vector<Entity>> cells_;
};

} // namespace core
