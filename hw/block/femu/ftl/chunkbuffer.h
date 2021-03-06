#ifndef __CHUNK_BUFFER_H
#define __CHUNK_BUFFER_H

#define PAGES_PER_CHUNK 32
#define MAX_CHUNK_BUFFER_SIZE 4096
#define HASH_MAP_SIZE (MAX_CHUNK_BUFFER_SIZE * 2)
#define NODE_ADD_SIZE 1

#define BUFFER_HIT_LAT 1000

struct chunk_node {
    unsigned long lcn;
    char bitmap[PAGES_PER_CHUNK];
    int size;

    struct chunk_node *pre;
    struct chunk_node *next;

    struct chunk_node *hashpre;
    struct chunk_node *hashnext;
};

struct chunk_buffer_info {
    int max_size;
    int cur_size;

    struct chunk_node *head;
    struct chunk_node *tail;
    
    struct chunk_node **hashmap;

    unsigned long rbuffer_hit;
    unsigned long wbuffer_hit;
    unsigned long rbuffer_miss;
    unsigned long wbuffer_miss;

    unsigned long wb_page;

    unsigned long rdelay;
    unsigned long wdelay;
    unsigned long rcnt;
    unsigned long wcnt;
};

#endif