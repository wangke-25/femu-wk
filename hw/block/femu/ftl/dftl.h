#ifndef __DFTL_H
#define __DFTL_H

#define M 32
#define N 64
#define ENTRY_PER_PAGE 1024
#define MAX_MAPPING_CAHCHE_SIZE (512 * ENTRY_PER_PAGE)
#define DFTL_HASH_MAP_SIZE 1024         //512*2
#define DFTL_NODE_ADD_SIZE 1

#define WRITE 0
#define READ 1

#define DFTL_DEBUG 0

struct map_entry_node {
    unsigned long tvpn;
    char bitmap[ENTRY_PER_PAGE];
    char dirty[ENTRY_PER_PAGE];
    int entry_cnt[2];
    int dirty_cnt[2];

    struct map_entry_node *pre;
    struct map_entry_node *next;

    struct map_entry_node *hashpre;
    struct map_entry_node *hashnext;
};

struct mapping_cache_info {
    int max_size;
    int cur_size;

    struct map_entry_node *head;
    struct map_entry_node *tail;

    struct map_entry_node **hashmap;

    unsigned long dftl_rhit;
    unsigned long dftl_rmiss;
    unsigned long dftl_whit;
    unsigned long dftl_wmiss;

    unsigned long r_delay;
};

#endif