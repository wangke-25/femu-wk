#ifndef __DFTL_H
#define __DFTL_H

#define M 64
#define N 32
#define ENTRY_PER_PAGE 2048             // M*N
#define MAX_MAPPING_CAHCHE_SIZE (60 * ENTRY_PER_PAGE)   //28
#define DFTL_HASH_MAP_SIZE (128 * ENTRY_PER_PAGE)         //1024*2
#define DFTL_NODE_ADD_SIZE 1

#define WRITE 0
#define READ 1

#define DFTL_DEBUG 0

#define GTD_INDEX(x) (x/(M*N/2))

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

struct chunk_map_info {
    char *bitmap;
    int *chunk_page_cnt;

    unsigned long update_cnt;
    unsigned long page_map_cnt;
    unsigned long chunk_map_cnt;
};

struct mapping_cache_info {
    int max_size;
    int cur_size;

    struct map_entry_node *head;
    struct map_entry_node *tail;

    struct map_entry_node **hashmap;

    unsigned long dftl_rhit;
    unsigned long dftl_rhit1;
    unsigned long dftl_rmiss;
    unsigned long dftl_whit;
    unsigned long dftl_whit1;
    unsigned long dftl_wmiss;

    unsigned long r_delay;

    struct chunk_map_info *ckm_info;
};

#endif