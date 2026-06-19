#include <stdlib.h>
#include "hashtable.h"
#include <assert.h>

const size_t rehashing_constant=128;
const size_t load_factor = 8;


void ht_init(hash_table* htab, size_t n)
{
    assert(n && n&(n-1)==0);

    htab->mask = n-1;
    htab->size=0;
    htab->tab = (hash_node**)calloc(n, sizeof(hash_node*));
    return;
}


void ht_insert(hash_table* htab, hash_node* node)
{
    size_t pos=htab->mask & (node->hash);
    hash_node* next=htab->tab[pos];
    node->next=next;
    htab->tab[pos]=node;
    htab->size++;
    return;
}


hash_node** ht_lookup(hash_table* htab, hash_node* key, bool(*equal_func)(hash_node* a, hash_node* b))
{
    if(!htab->tab) return NULL;
    size_t pos=key->hash & htab->mask;
    hash_node** target = &(htab->tab[pos]);
    for(hash_node* curr=*target;curr!=NULL;target=&((*target)->next), curr=*target)
    {   
        if(key->hash == curr->hash && equal_func(key, curr))
        {
            return target;
        }
    }
    return NULL;
}


hash_node* ht_detach(hash_table* htab, hash_node** target)
{
    hash_node* req=*target;
    *target=req->next;
    htab->size--;
    return req;
}



void hm_reshashing_helper(hash_map* hmap) 
{
    size_t it=0;
    while(it<rehashing_constant && hmap->older.size>0)
    {
        hash_node** curr=&hmap->older.tab[hmap->migration_offset];
        if(!*curr)
        {
            hmap->migration_offset ++;
            continue;
        }
        ht_insert(&hmap->newer, ht_detach(&hmap->older, curr));
        it++;
    }

    if(hmap->older.size == 0 && hmap->older.tab)
    {
        free(hmap->older.tab);
        hmap->older=hash_table();
    }

    return;
}



void trigger_rehashing(hash_map* hmap) 
{
    assert(hmap->older.tab == NULL);
    hmap->older=hmap->newer;
    ht_init(&hmap->newer, 2*(hmap->newer.mask + 1));
    hmap->migration_offset=0;
    return;
}

hash_node* hm_lookup(hash_map* hmap, hash_node* key, bool(*equal_func)(hash_node* a, hash_node* b))
{
    hm_reshashing_helper(hmap);
    hash_node** target=ht_lookup(&hmap->newer, key, equal_func);
    if(!target)
    {
        target=ht_lookup(&hmap->older, key, equal_func);
    }

    return target?*target:NULL;
}


void hm_insert(hash_map* hmap, hash_node* node)
{
    if(!hmap->newer.tab) ht_init(&hmap->newer, 4);

    ht_insert(&hmap->newer, node);

    if(!hmap->older.tab)
    {
        size_t threshold=(hmap->newer.mask + 1)*load_factor;
        if(hmap->newer.size >= threshold)
        {
            trigger_rehashing(hmap);
        }
    }

    hm_reshashing_helper(hmap);
    return;
}

hash_node* hm_delete(hash_map* hmap, hash_node* key, bool(*equal_func)(hash_node* a, hash_node* b))
{
    trigger_rehashing(hmap);

    hash_node** target=ht_lookup(&hmap->newer, key, equal_func);
    if(target!=NULL)  return ht_detach(&hmap->newer, target);

    target=ht_lookup(&hmap->older, key, equal_func);
    if(target!=NULL) return ht_detach(&hmap->older, target);

    return NULL;
}


void hm_clear(hash_map* hmap) //only frees the memory associated with hash_node** tab, need to free the entries before calling this
{
    free(hmap->newer.tab);
    free(hmap->older.tab);
    return;
}

size_t hm_size(hash_map* hmap)
{
    return hmap->newer.size + hmap->older.size;
}


