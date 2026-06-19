#pragma once
#include <stddef.h>
#include <stdint.h>



struct hash_node{
    hash_node* next; //next node pointer
    uint64_t hash; //hash
};

struct hash_table{ //single hashtable
    hash_node** tab=NULL; // buckets
    size_t mask=0; //2^n -1 , where 2^n is buckets count
    size_t size=0; //keys count
};


struct hash_map{ //OG hashmap interface, 2 hashtables for progressive rehashing
    hash_table newer;
    hash_table older;
    size_t migration_offset=0;
};

hash_node* hm_lookup(hash_map* hmap, hash_node* key, bool(*equal_func)(hash_node* a, hash_node* b));
void hm_insert(hash_map* hmap, hash_node* node);
hash_node* hm_delete(hash_map* hmap, hash_node* key, bool(*equal_func)(hash_node* a, hash_node* b));
void hm_clear(hash_map* hmap);
size_t hm_size(hash_map* hmap);
