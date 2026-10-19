#include "cachesim.hpp"
#include "cachesim_struct.hpp"
#include <stdexcept>
/*-------------DO NOT CHANGE THIS BLOCK OF CODE-------------*/
//Pseudo-Random Number Generator for RANDOM Replacement policy
static unsigned long int evict_random_next = 1;
int evict_random(void)
{
    evict_random_next = evict_random_next * 1103515243 + 12345;
    return (unsigned int)(evict_random_next / 65536) % 32768;
}

void evict_srand(unsigned int seed)
{
    evict_random_next = seed;
}
/*----------------------------------------------------------*/

/**
 * Subroutine for initializing the cache simulator. You many add and initialize any global or heap
 * variables as needed.
 * TODO: You're responsible for completing this routine
 */
Cache* l1_cache = nullptr;
Cache* victim_cache = nullptr;
Cache* l2_cache = nullptr;

// Return the number of sets
static uint64_t cache_num_sets(const cache_config_t& config) {
    if (config.c < config.b || config.c - config.b < config.s) {
        throw std::invalid_argument("cache capacity must hold at least one complete set");
    }
    return 1ULL << (config.c - config.b - config.s);
}

struct DecodedAddress {
    uint64_t block_number;
    uint64_t index;
    uint64_t tag;
};

static DecodedAddress decode_address(const Cache& cache, uint64_t addr) {
    const uint64_t block_number = addr / cache.block_size;
    return {
        block_number,
        block_number % cache.num_sets,
        block_number / cache.num_sets,
    };
}

static int choose_fill_way(Set& set) {
    // the set still has empty way
    for (uint64_t way = 0; way < set.num_ways; ++way) {
        if (!set.blocks[way].valid) {
            return static_cast<int>(way);
        }
    }

    if (set.replace_policy == REPLACEMENT_POLICY_RANDOM) {
        // The last block is the newest insertion and must be protected.
        // A direct-mapped set has no alternative victim.
        return set.num_ways == 1 ? 0 : evict_random() % (set.num_ways - 1);
    }

    if (set.lru_list.empty()) {
        throw std::logic_error("replacement metadata is empty for a full cache set");
    }

    //the set is fully occupied
    const uint64_t victim_tag = set.lru_list.back(); // LRU positon
    for (uint64_t way = 0; way < set.num_ways; ++way) {
        if (set.blocks[way].valid && set.blocks[way].tag == victim_tag) {
            return static_cast<int>(way);
        }
    }

    throw std::logic_error("replacement metadata does not match cache contents");
}

static Block install_block(Set& set, uint64_t tag, bool dirty) {
    int fill_way = choose_fill_way(set);
    const Block evicted_block = set.blocks[fill_way];

    if (set.replace_policy == REPLACEMENT_POLICY_RANDOM) {
        // Remove the selected way while preserving the order of survivors.
        // Always append the new block, including when filling an invalid way.
        std::rotate(set.blocks.begin() + fill_way,
                    set.blocks.begin() + fill_way + 1, set.blocks.end());
        fill_way = static_cast<int>(set.num_ways - 1);
    }

    if (evicted_block.valid && set.replace_policy != REPLACEMENT_POLICY_RANDOM) {
        set.lru_list.remove(evicted_block.tag);
    }

    set.blocks[fill_way] = {tag, true, dirty};

    if (set.replace_policy == REPLACEMENT_POLICY_LIP) {
        set.lru_list.push_back(tag);
    } else if (set.replace_policy != REPLACEMENT_POLICY_RANDOM) {
        // MIP inserts at MRU. FIFO also inserts at the newest position,
        // while FIFO hits leave this ordering unchanged.
        set.lru_list.push_front(tag);
    }

    return evicted_block;
}

static uint64_t compose_block_number(const Cache& cache, uint64_t tag, uint64_t index) {
    return tag * cache.num_sets + index;
}

static void write_back_to_l2(uint64_t l1_block_number, sim_stats_t* stats) {
    stats->write_backs_l1_or_victim_cache++;
    stats->writes_l2++;

    if (!l2_cache->is_enabled()) {
        return;
    }

    const uint64_t addr = l1_block_number * l1_cache->block_size;
    const DecodedAddress l2_addr = decode_address(*l2_cache, addr);
    Set& l2_set = l2_cache->sets[l2_addr.index];
    const int hit_way = l2_set.find_block(l2_addr.tag);
    if (hit_way != -1) {
        // L2 is write-through, so the write continues to memory and the line
        // remains clean. The access still updates replacement recency.
        l2_set.handle_hit(hit_way);
    }
}

static void read_from_l2(uint64_t addr, sim_stats_t* stats) {
    stats->reads_l2++;

    if (!l2_cache->is_enabled()) {
        stats->read_misses_l2++;
        return;
    }

    const DecodedAddress l2_addr = decode_address(*l2_cache, addr);
    Set& l2_set = l2_cache->sets[l2_addr.index];
    const int hit_way = l2_set.find_block(l2_addr.tag);
    if (hit_way != -1) {
        stats->read_hits_l2++;
        l2_set.handle_hit(hit_way);
        return;
    }

    stats->read_misses_l2++;
    install_block(l2_set, l2_addr.tag, false);

    if (l2_cache->is_ER_enabled()) {
        const uint64_t word_offset =
            (addr % l2_cache->block_size) / static_cast<uint64_t>(WORD_SIZE);
        stats->cumulative_l2_mp +=
            DRAM_AT + DRAM_AT_PER_WORD * word_offset;
    }
}

// 
static void handle_l1_victim(const Block& l1_victim, uint64_t l1_index,
                             sim_stats_t* stats) {
    if (!l1_victim.valid) {
        return;
    }

    const uint64_t victim_block_number =
        compose_block_number(*l1_cache, l1_victim.tag, l1_index);

    if (victim_cache != nullptr) {
        Set& vc_set = victim_cache->sets[0];
        const Block vc_victim =
            install_block(vc_set, victim_block_number, l1_victim.dirty);
        if (vc_victim.valid && vc_victim.dirty) {
            write_back_to_l2(vc_victim.tag, stats);
        }
        return;
    }

    if (l1_victim.dirty) {
        write_back_to_l2(victim_block_number, stats);
    }
}

void sim_setup(sim_config_t *config) {
    if (config == nullptr) {
        throw std::invalid_argument("sim_setup requires a valid configuration");
    }

    if (config->l1_config.b >= 64 || config->l1_config.s >= 64 ||
        config->l2_config.b >= 64 || config->l2_config.s >= 64) {
        throw std::invalid_argument("cache geometry exceeds 64-bit limits");
    }

    // Validate and derive all geometry before allocating any cache.
    const uint64_t l1_num_sets = cache_num_sets(config->l1_config);
    const uint64_t l1_num_ways = 1ULL << config->l1_config.s;
    const uint64_t l2_num_sets = cache_num_sets(config->l2_config);
    const uint64_t l2_num_ways = 1ULL << config->l2_config.s;
    const uint64_t l1_block_size = 1ULL << config->l1_config.b;
    const uint64_t l2_block_size = 1ULL << config->l2_config.b;

    // Initialize L1 cache
    l1_cache = new Cache(l1_num_sets, l1_num_ways, l1_block_size,
                            config->l1_config.replace_policy, config->l1_config.write_strat,
                            true, false, 0);

    // Initialize victim cache
    victim_cache = nullptr;
    if (config->victim_cache_entries > 0) {
        victim_cache = new Cache(1, config->victim_cache_entries, l1_block_size,
                                    REPLACEMENT_POLICY_MIP, config->l1_config.write_strat,
                                    true, false, config->victim_cache_entries);
    }

    // Initialize L2 cache
    const bool l2_enabled = !config->l2_config.disabled;
    l2_cache = new Cache(l2_num_sets, l2_num_ways, l2_block_size,
                            config->l2_config.replace_policy, config->l2_config.write_strat,
                            l2_enabled, l2_enabled && config->l2_config.enable_ER, 0);
}

/**
 * Subroutine that simulates the cache one trace event at a time.
 * TODO: You're responsible for completing this routine
 */
void sim_access(char rw, uint64_t addr, sim_stats_t* stats) {
    if (stats == nullptr || l1_cache == nullptr) {
        throw std::logic_error("sim_access requires an initialized simulator and statistics");
    }
    if (rw != READ && rw != WRITE) {
        throw std::invalid_argument("cache access must be a read or write");
    }

    stats->accesses_l1++;
    if (rw == READ) {
        stats->reads++;
    } else {
        stats->writes++;
    }

    const DecodedAddress l1_addr = decode_address(*l1_cache, addr);
    Set& l1_set = l1_cache->sets[l1_addr.index];

    // Check L1 cache
    const int l1_hit_way = l1_set.find_block(l1_addr.tag);
    if (l1_hit_way != -1) {
        stats->hits_l1++;
        l1_set.handle_hit(l1_hit_way);
        if (rw == WRITE) {
            l1_set.blocks[l1_hit_way].dirty = true;
        }
        return;
    }

    stats->misses_l1++;
    // Check VC
    if (victim_cache != nullptr) {
        Set& vc_set = victim_cache->sets[0];
        // VC is fully associative, no index, tag = block_number;
        const int vc_hit_index = vc_set.find_block(l1_addr.block_number);
        if (vc_hit_index != -1) {
            stats->hits_victim_cache++;

            const Block requested_block = vc_set.blocks[vc_hit_index];
            const Block l1_victim = install_block(
                l1_set, l1_addr.tag, requested_block.dirty || rw == WRITE);

            vc_set.lru_list.remove(requested_block.tag);
            if (l1_victim.valid) {
                const uint64_t victim_block_number =
                    compose_block_number(*l1_cache, l1_victim.tag, l1_addr.index);
                vc_set.blocks[vc_hit_index] =
                    {victim_block_number, true, l1_victim.dirty};
                vc_set.lru_list.push_front(victim_block_number);
            } else {
                vc_set.blocks[vc_hit_index] = {0, false, false};
            }
            return;
        }
    }

    // A disabled victim cache behaves as an always-missing victim cache for
    // the conditional VC statistics used by the driver.
    stats->misses_victim_cache++;

    read_from_l2(addr, stats);

    const Block l1_victim = install_block(l1_set, l1_addr.tag, rw == WRITE);
    handle_l1_victim(l1_victim, l1_addr.index, stats);
}
/**
 * Subroutine for cleaning up any outstanding memory operations and calculating overall statistics
 * such as miss rate or average access time.
 * TODO: You're responsible for completing this routine
 */
void sim_finish(sim_stats_t *stats) {
    if (stats == nullptr || l1_cache == nullptr || l2_cache == nullptr) {
        throw std::logic_error("sim_finish requires initialized simulator state");
    }

    const auto ratio = [](uint64_t numerator, uint64_t denominator) {
        return denominator == 0
            ? 0.0
            : static_cast<double>(numerator) / denominator;
    };

    stats->hit_ratio_l1 = ratio(stats->hits_l1, stats->accesses_l1);
    stats->miss_ratio_l1 = ratio(stats->misses_l1, stats->accesses_l1);
    stats->hit_ratio_victim_cache =
        ratio(stats->hits_victim_cache, stats->misses_l1);
    stats->miss_ratio_victim_cache =
        ratio(stats->misses_victim_cache, stats->misses_l1);
    stats->read_hit_ratio_l2 = ratio(stats->read_hits_l2, stats->reads_l2);
    stats->read_miss_ratio_l2 = ratio(stats->read_misses_l2, stats->reads_l2);

    const uint64_t l1_s = __builtin_ctzll(l1_cache->num_ways);
    const uint64_t l2_s = __builtin_ctzll(l2_cache->num_ways);
    const double l1_hit_time = L1_HIT_TIME_CONST + L1_HIT_TIME_PER_S * l1_s;
    const double l2_hit_time = L2_HIT_TIME_CONST + L2_HIT_TIME_PER_S * l2_s;
    const double full_dram_penalty =
        DRAM_AT + DRAM_AT_PER_WORD * (l2_cache->block_size / WORD_SIZE);

    if (l2_cache->is_ER_enabled() && stats->read_misses_l2 > 0) {
        stats->averaged_miss_penalty_l2 =
            static_cast<double>(stats->cumulative_l2_mp) / stats->read_misses_l2;
    } else {
        stats->averaged_miss_penalty_l2 = full_dram_penalty;
    }

    if (l2_cache->is_enabled()) {
        stats->avg_access_time_l2 =
            l2_hit_time
            + stats->read_miss_ratio_l2 * stats->averaged_miss_penalty_l2;
    } else {
        // With L2 bypassed, every downstream demand is serviced by DRAM.
        stats->avg_access_time_l2 = full_dram_penalty;
    }

    stats->avg_access_time_l1 =
        l1_hit_time
        + stats->miss_ratio_l1
            * stats->miss_ratio_victim_cache
            * stats->avg_access_time_l2;

    delete l1_cache;
    delete victim_cache;
    delete l2_cache;
    l1_cache = nullptr;
    victim_cache = nullptr;
    l2_cache = nullptr;
}
