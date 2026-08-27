#ifndef CACHESIM_STRUCT_HPP
#define CACHESIM_STRUCT_HPP

#include <iostream>
#include <cmath>
#include <memory>
#include <cstdint>
#include "cachesim.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <list>
//using namespace std;
struct Block {
    uint64_t tag;
    bool valid;
    bool dirty;
};

struct Set {
    uint64_t num_ways;
    replacement_policy_t replace_policy;
    std::vector<Block> blocks;
    std::list<uint64_t> lru_list; // For LRU replacement

    Set(uint64_t num_ways, replacement_policy_t replace_policy)
        : num_ways(num_ways), replace_policy(replace_policy) {
        blocks.resize(num_ways, {0, false, false});
    }

    // Find a block in the set, return the index of the block in the set.
    int find_block(uint64_t tag) {
        for (int i = 0; i < num_ways; i++) {
            if (blocks[i].valid && blocks[i].tag == tag) {
                return i;
            }
        }
        return -1; // Block not found
    }

    // Handle a cache hit, put the block with a given indwx to MRU position of the lru_list.
    void handle_hit(int index) {
        if (replace_policy == REPLACEMENT_POLICY_MIP || replace_policy == REPLACEMENT_POLICY_LIP) {
            // Move block to MRU position
            lru_list.erase(std::find(lru_list.begin(), lru_list.end(), blocks[index].tag));
            lru_list.push_front(blocks[index].tag);
        }
    }

    // Handle a cache miss
    // Return the inserted block
    Block handle_miss(uint64_t tag) {
        int evict_index = -1;
        if (lru_list.size() == num_ways) {
            // Evict LRU block
            uint64_t evicted_tag = lru_list.back();
            lru_list.pop_back();
            for (int i = 0; i < num_ways; i++) {
                if (blocks[i].valid && blocks[i].tag == evicted_tag) {
                    evict_index = i;
                    break;
                }
            }
        } 
        else {
            // Find an invalid block
            for (int i = 0; i < num_ways; i++) {
                if (!blocks[i].valid) {
                    evict_index = i;
                    break;
                }
            }
        }
        // Insert new block
        if (evict_index != -1) {
            blocks[evict_index].tag = tag;
            blocks[evict_index].valid = true;
            lru_list.push_front(tag);
        }
        return blocks[evict_index];
    }

    // Handle random replacement
    int handle_random_miss(uint64_t tag) {
        int evict_index = evict_random() % num_ways;
        blocks[evict_index].tag = tag;
        blocks[evict_index].valid = true;
        return evict_index;
    }
};

struct Cache {
    uint64_t num_sets;   // 2^(C-B-S)
    uint64_t num_ways;   // 2^S, block per set
    uint64_t block_size; // 2^B
    replacement_policy_t replace_policy;
    write_strat_t write_strat;
    bool enabled; // For L2 cache
    bool ER_enabled; // L2 early restart
    uint64_t victim_cache_entries; // For Victim Cache
    std::vector<Set> sets;

    Cache(uint64_t num_sets, uint64_t num_ways, uint64_t block_size,
          replacement_policy_t replace_policy, write_strat_t write_strat, bool enabled, bool ER_enabled, uint64_t victim_cache_entries)
        : num_sets(num_sets), num_ways(num_ways), block_size(block_size),
          replace_policy(replace_policy), write_strat(write_strat), enabled(enabled), ER_enabled(ER_enabled), victim_cache_entries(victim_cache_entries) {
        if (victim_cache_entries > 0) {
            // VC is fully associative (1 set)
            sets.emplace_back(victim_cache_entries, replace_policy);
        } else {
            // Regular cache
            sets.reserve(num_sets);
            if (enabled){
                for (uint64_t i = 0; i < num_sets; i++) {
                    sets.emplace_back(num_ways, replace_policy);
                }
            } 
        }
    }

    // Check if the cache is enabled
    bool is_enabled() const {
        return enabled;
    }

    // Check if the early restart is enabled
    bool is_ER_enabled() const {
        return ER_enabled;
    }

    // Check if the cache is a victim cache
    bool is_victim_cache() const {
        return victim_cache_entries > 0;
    }
};
#endif 