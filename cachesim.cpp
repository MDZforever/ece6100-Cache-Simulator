#include "cachesim.hpp"
#include "cachesim_struct.hpp"
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

void sim_setup(sim_config_t *config) {
    // Initialize L1 cache
    uint64_t l1_num_sets = 1ULL << (config->l1_config.c - config->l1_config.b - config->l1_config.s);
    l1_cache = new Cache(l1_num_sets, 1ULL << config->l1_config.s, 1ULL << config->l1_config.b,
                            config->l1_config.replace_policy, config->l1_config.write_strat,
                            true, false, 0);

    // Initialize victim cache
    if (config->victim_cache_entries > 0) {
        victim_cache = new Cache(1, config->victim_cache_entries, 1ULL << config->l1_config.b,
                                    REPLACEMENT_POLICY_LIP, config->l1_config.write_strat,
                                    true, false, config->victim_cache_entries);
    }

    // Initialize L2 cache
    uint64_t l2_num_sets = 1ULL << (config->l2_config.c - config->l2_config.b);
    l2_cache = new Cache(l2_num_sets, 1ULL << config->l2_config.s, 1ULL << config->l2_config.b,
                            config->l2_config.replace_policy, config->l2_config.write_strat,
                            !config->l2_config.disabled, config->l2_config.enable_ER, 0);
}

/**
 * Subroutine that simulates the cache one trace event at a time.
 * TODO: You're responsible for completing this routine
 */
void sim_access(char rw, uint64_t addr, sim_stats_t* stats) {
    stats->accesses_l1++;
    if (rw == READ) stats->reads++;
    else stats->writes++;

    // Extract block tag and index
    // uint64_t block_offset = log2(l1_cache->block_size);
    uint64_t l1_index = (addr >> __builtin_ctzll(l1_cache->block_size)) & ((1ULL << __builtin_ctzll(l1_cache->num_sets)) - 1);
    uint64_t l1_tag = addr >> (__builtin_ctzll(l1_cache->block_size) + __builtin_ctzll(l1_cache->num_sets));

    // Check L1 cache
    int l1_hit_index = l1_cache->sets[l1_index].find_block(l1_tag);
    if (l1_hit_index != -1) {
        stats->hits_l1++;
        l1_cache->sets[l1_index].handle_hit(l1_hit_index);
        if (rw == WRITE) l1_cache->sets[l1_index].blocks[l1_hit_index].dirty = true;
    } else {
        stats->misses_l1++;
        // Check victim cache
        if (victim_cache) {
            int vc_hit_index = victim_cache->sets[0].find_block(l1_tag);
            if (vc_hit_index != -1) {
                stats->hits_victim_cache++;
                // Move block from victim cache to L1
                Block evicted_block = l1_cache->sets[l1_index].handle_miss(l1_tag);
                if (evicted_block.valid && evicted_block.dirty) {
                    stats->write_backs_l1_or_victim_cache++;
                }
            } else {
                stats->misses_victim_cache++;
                // Check L2 cache
                uint64_t l2_index = (addr >> __builtin_ctzll(l2_cache->block_size)) & ((1ULL << __builtin_ctzll(l2_cache->num_sets)) - 1);
                uint64_t l2_tag = addr >> (__builtin_ctzll(l2_cache->block_size) + __builtin_ctzll(l2_cache->num_sets));
                int l2_hit_index = l2_cache->sets[l2_index].find_block(l2_tag);
                stats->reads_l2++;
                if (l2_hit_index != -1) {
                    stats->read_hits_l2++;
                    l2_cache->sets[l2_index].handle_hit(l2_hit_index);
                } else {
                    stats->read_misses_l2++;
                    // Handle L2 miss (fetch from DRAM)
                    if (l2_cache->is_ER_enabled()) {
                        uint64_t word_offset = (addr >> 3) & ((1ULL << 6) - 1); // 64-bit words
                        double dram_time = DRAM_AT + (DRAM_AT_PER_WORD * word_offset);
                        stats->cumulative_l2_mp += dram_time;
                    }
                }
                // Move block from L2 to L1
                Block evicted_block = l1_cache->sets[l1_index].handle_miss(l1_tag);
                if (evicted_block.valid && evicted_block.dirty) {
                    stats->write_backs_l1_or_victim_cache++;
                }
            }
        }
    }
}
/**
 * Subroutine for cleaning up any outstanding memory operations and calculating overall statistics
 * such as miss rate or average access time.
 * TODO: You're responsible for completing this routine
 */
void sim_finish(sim_stats_t *stats) {
    // Calculate hit and miss ratios
    stats->hit_ratio_l1 = static_cast<double>(stats->hits_l1) / stats->accesses_l1;
    stats->miss_ratio_l1 = static_cast<double>(stats->misses_l1) / stats->accesses_l1;

    stats->hit_ratio_victim_cache = static_cast<double>(stats->hits_victim_cache) / stats->misses_l1;
    stats->miss_ratio_victim_cache = static_cast<double>(stats->misses_victim_cache) / stats->misses_l1;

    stats->read_hit_ratio_l2 = static_cast<double>(stats->read_hits_l2) / stats->reads_l2;
    stats->read_miss_ratio_l2 = static_cast<double>(stats->read_misses_l2) / stats->reads_l2;

    // Calculate AAT for L1 and L2
    double ht_l1 = L1_HIT_TIME_CONST + (l1_cache->num_sets * L1_HIT_TIME_PER_S);
    double ht_l2 = L2_HIT_TIME_CONST + (l2_cache->num_sets * L2_HIT_TIME_PER_S);
    double access_time_DRAM = DRAM_AT + (DRAM_AT_PER_WORD * (l2_cache->block_size / WORD_SIZE));
    double mp_l1_vc = access_time_DRAM;    
    if(l2_cache->enabled){
            double mp_l1_vc = ht_l2 + (stats->read_miss_ratio_l2 * access_time_DRAM);
    }
    stats->avg_access_time_l1 = ht_l1 + (stats->miss_ratio_victim_cache * stats->miss_ratio_l1) * mp_l1_vc;

    stats->avg_access_time_l2 = ht_l2 + (stats->read_miss_ratio_l2 * (DRAM_AT + (DRAM_AT_PER_WORD * (l2_cache->block_size / WORD_SIZE))));

    // Cleanup
    delete l1_cache;
    delete victim_cache;
    delete l2_cache;
}