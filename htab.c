#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "htab.h"

#define _GNU_SOURCE
#define __USE_GNU
#include <search.h>

size_t hash_table_size (){
    return sizeof(struct hsearch_data);
}

int hash_table_init(struct hsearch_data* htab){
    hcreate_r(2000, htab);
    
    return 0;
}


void hash_table_add(char *key, long data, struct hsearch_data *htab, char** all_keys){
    ENTRY item, *temp;

    item.key = strdup(key);
    item.data = (void *) data;
    
    *all_keys = item.key;
    // printf("keys pointer in hash: %p", all_keys);
    hsearch_r(item, ENTER, &temp, htab);
    
    return;
}


int hash_table_get(char *key, struct hsearch_data *htab){
    ENTRY item;
    item.key = key;

    ENTRY *temp;
    // printf("URL in the KEY: %s\n", item.key);
    int ret = hsearch_r(item, FIND, &temp, htab);
    if (ret) {
        return 1;
    }
    return 0;
    
    
}



