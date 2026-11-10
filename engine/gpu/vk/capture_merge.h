// (TICK-043) A submit's capture made of the pages copied ahead of it (the pre-copy thread, vk_memory.cpp) and the pages
// written since (collected at the submit).
//
// The capture copies the pages the game wrote since the last submit, as they are at the submit. The pre-copy thread takes
// written pages before the submit comes (it clears their write marks, then copies them into the next capture's buffer): a page
// the game writes again after that is marked again (the lifted store's mark comes after the store, with release order, and the
// thread clears a mark with an acquire exchange before it reads the page), so the submit collects it and copies it again, into
// the slot it already has. A page a GPU write-back may have written since its copy (its write-back value past what the GPU had
// done when its copy began) is copied again too. Everything else holds what it held at its copy, which is what it holds at the
// submit. The capture's pages are then the copied ones (slots 0..k-1, in the order they were copied) and the new ones after
// them, each page once.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace capture_merge {

struct Merged {
    std::vector<uint32_t> pages;    // (the capture's pages: page i in slot i)
    std::vector<uint32_t> recopy;   // (slots of copied-ahead pages to copy again now)
    size_t first_new = 0;           // (pages[first_new..] are the new ones, to copy now)
};

/// pre[0..k): the pages copied ahead, in slot order; pre_done[i]: what the GPU had done when page i's copy began.
/// fresh[0..n): the pages collected at the submit. wb(page): the timeline value of the page's last write-back (0: none).
/// A page in both is copied again into its slot; a copied-ahead page whose write-back value is past its pre_done is too.
template <class Wb>
void merge(const uint32_t* pre, const uint64_t* pre_done, size_t k, const uint32_t* fresh, size_t n, Wb wb, Merged& out) {
    out.pages.assign(pre, pre + k);
    out.recopy.clear();
    out.first_new = k;
    std::vector<std::pair<uint32_t, uint32_t>> by_page(k);   // (page, slot), sorted by page
    for (size_t i = 0; i < k; i++) by_page[i] = {pre[i], (uint32_t)i};
    std::sort(by_page.begin(), by_page.end());
    std::vector<uint8_t> again(k, 0);
    for (size_t i = 0; i < n; i++) {
        const auto it = std::lower_bound(by_page.begin(), by_page.end(), std::make_pair(fresh[i], uint32_t{0}));
        if (it != by_page.end() && it->first == fresh[i]) again[it->second] = 1;
        else out.pages.push_back(fresh[i]);
    }
    for (size_t i = 0; i < k; i++)
        if (again[i] || wb(pre[i]) > pre_done[i]) out.recopy.push_back((uint32_t)i);
}

}  // namespace capture_merge
