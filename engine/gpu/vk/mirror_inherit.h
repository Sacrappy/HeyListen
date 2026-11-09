/* (TICK-036) A pool's new mirror takes over a retired pool's mirror (vk_memory.cpp mirror_make): which parts of it. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace mkwvk::mirror_inherit {

/// The runs [lo, hi) of the guest range [a, b) to copy from a retired pool's mirror into the new mirror: the pages the CPU
/// has not written since that mirror's last sync. The others stay as the new mirror has them (guest memory and the
/// captures, the CPU's newer writes): copied whole, the retired mirror put back what it held before the CPU wrote them
/// (the vertices and indices a scene load writes after the mirror's last sync: zeros, or another object's).
/// written(page): the CPU wrote the page since that sync. region_clean(region): no page of that region of region_pages
/// pages was written since (a whole region at once; only asked for a region that lies inside [a, b))
template <class Written, class RegionClean>
void runs(uint64_t a, uint64_t b, unsigned page_shift, uint64_t region_pages, Written written, RegionClean region_clean,
          std::vector<std::pair<uint64_t, uint64_t>>& out) {
    if (b <= a) return;
    const auto add = [&out](uint64_t lo, uint64_t hi) {
        if (hi <= lo) return;
        if (!out.empty() && out.back().second == lo) out.back().second = hi;
        else out.push_back({lo, hi});
    };
    const uint64_t last = (b - 1) >> page_shift;
    for (uint64_t page = a >> page_shift; page <= last;) {
        if (region_pages && page % region_pages == 0 && page + region_pages - 1 <= last && region_clean(page / region_pages)) {
            add(std::max(page << page_shift, a), std::min((page + region_pages) << page_shift, b));
            page += region_pages;
            continue;
        }
        if (!written(page)) add(std::max(page << page_shift, a), std::min((page + 1) << page_shift, b));
        page++;
    }
}

} // namespace mkwvk::mirror_inherit
