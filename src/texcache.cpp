// Decoded-texture cache (see texcache.h).
//
// Textures rewritten about as often as they're drawn (movie frames, render targets) aren't worth
// decoding: an entry is only decoded once it has been used kPromote times with its pages
// unchanged; until then the caller samples GS memory directly.
//
// A texture is looked up by everything its texels depend on. Each entry remembers the GS memory
// pages it was decoded from (texture and CLUT) with their write counts at that time; it is reused
// while those counts are unchanged, so textures the game uploads once (fonts, menus, room
// textures) are decoded once instead of per drawn pixel, and render targets used as textures stay
// correct. Least-recently-used entries go first when the cache passes its memory budget.
#include "texcache.h"

#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

uint32_t g_tc_page_gen[512];

namespace {

constexpr size_t kBudgetTexels = 16u << 20;    // 64 MiB of RGBA
constexpr uint32_t kMaxSide = 1024;
constexpr uint32_t kPromote = 4;

struct KeyHash {
    size_t operator()(const TexKey &k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(k.tbp); mix(k.tbw); mix(k.psm); mix(k.tw); mix(k.th);
        mix(k.cbp); mix(k.cpsm); mix(k.csa); mix(k.texa);
        return static_cast<size_t>(h ^ (h >> 32));
    }
};
struct KeyEq {
    bool operator()(const TexKey &a, const TexKey &b) const {
        return a.tbp == b.tbp && a.tbw == b.tbw && a.psm == b.psm && a.tw == b.tw && a.th == b.th &&
               a.cbp == b.cbp && a.cpsm == b.cpsm && a.csa == b.csa && a.texa == b.texa;
    }
};

struct Entry {
    bool decoded = false;                   // texels hold the texture as of gens
    uint32_t uses = 0;                      // lookups since the pages last changed
    std::vector<uint32_t> texels;
    std::vector<uint16_t> pages;
    std::vector<uint32_t> gens;             // g_tc_page_gen[pages[i]] when decoded
    std::list<TexKey>::iterator lru;
};

std::unordered_map<TexKey, Entry, KeyHash, KeyEq> g_cache;
std::list<TexKey> g_lru;                    // front: most recently used
size_t g_texels;

bool is_indexed(uint32_t psm) {
    switch (psm) {
    case 0x13: case 0x14: case 0x1B: case 0x24: case 0x2C: return true;
    }
    return false;
}

// The pages a texture's texels can come from (a superset when it starts mid-page).
void texture_pages(const TexKey &k, std::vector<uint16_t> &out) {
    uint32_t pw = 64, ph = 32, row = k.tbw ? k.tbw : 1;
    switch (k.psm) {
    case 0x02: case 0x0A: case 0x32: case 0x3A: ph = 64; break;                 // 16-bit
    case 0x13: pw = 128; ph = 64; row = k.tbw / 2 ? k.tbw / 2 : 1; break;        // 8-bit
    case 0x14: pw = 128; ph = 128; row = k.tbw / 2 ? k.tbw / 2 : 1; break;       // 4-bit
    }
    uint32_t first = k.tbp / 32, nx = (k.tw + pw - 1) / pw + (k.tbp % 32 ? 1 : 0);
    uint32_t ny = (k.th + ph - 1) / ph;
    for (uint32_t y = 0; y < ny; y++)
        for (uint32_t x = 0; x < nx; x++) out.push_back(static_cast<uint16_t>((first + y * row + x) & 511));
    if (is_indexed(k.psm)) {
        out.push_back(static_cast<uint16_t>((k.cbp / 32) & 511));
        out.push_back(static_cast<uint16_t>((k.cbp / 32 + 1) & 511));
    }
}

bool still_valid(const Entry &e) {
    for (size_t i = 0; i < e.pages.size(); i++)
        if (g_tc_page_gen[e.pages[i]] != e.gens[i]) return false;
    return true;
}

void evict_until(size_t room) {
    while ((g_texels + room > kBudgetTexels || g_cache.size() > 4096) && !g_lru.empty()) {
        auto it = g_cache.find(g_lru.back());
        if (it->second.texels.empty() && g_lru.size() == 1) break;
        g_texels -= it->second.texels.size();
        g_cache.erase(it);
        g_lru.pop_back();
    }
}

}  // namespace

static void snapshot(Entry &e) {
    for (size_t i = 0; i < e.pages.size(); i++) e.gens[i] = g_tc_page_gen[e.pages[i]];
}

extern "C" const uint32_t *tc_get(const TexKey *key, TexDecodeFn decode, void *ctx) {
    if (key->tw > kMaxSide || key->th > kMaxSide) return nullptr;
    auto it = g_cache.find(*key);
    if (it == g_cache.end()) {
        g_lru.push_front(*key);
        Entry &e = g_cache[*key];
        e.lru = g_lru.begin();
        texture_pages(*key, e.pages);
        e.gens.resize(e.pages.size());
        snapshot(e);
        e.uses = 1;
        return nullptr;
    }
    Entry &e = it->second;
    g_lru.splice(g_lru.begin(), g_lru, e.lru);
    if (!still_valid(e)) {                  // rewritten: start counting again
        snapshot(e);
        e.uses = 1;
        if (e.decoded) {
            g_texels -= e.texels.size();
            std::vector<uint32_t>().swap(e.texels);
            e.decoded = false;
        }
        return nullptr;
    }
    if (e.decoded) return e.texels.data();
    if (++e.uses < kPromote) return nullptr;
    size_t n = static_cast<size_t>(key->tw) * key->th;
    evict_until(n);                         // (never this entry: it was just moved to the front)
    e.texels.resize(n);
    decode(ctx, e.texels.data());
    e.decoded = true;
    g_texels += n;
    return e.texels.data();
}

extern "C" void tc_reset(void) {
    g_cache.clear();
    g_lru.clear();
    g_texels = 0;
    std::memset(g_tc_page_gen, 0, sizeof g_tc_page_gen);
}
