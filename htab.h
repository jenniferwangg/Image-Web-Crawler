#pragma once

struct hsearch_data *htab;

size_t hash_table_size ();
int hash_table_init(struct hsearch_data* htab);
void hash_table_add(char *key, long data, struct hsearch_data *htab,  char** all_keys);
int hash_table_get(char *key, struct hsearch_data *htab);