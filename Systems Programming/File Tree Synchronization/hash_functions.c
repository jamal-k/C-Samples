#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hash.h"

#define BLOCK_SIZE 8

char *hash(FILE *f) {
    // initialize hash_val
    char *hash_val = malloc(sizeof(char)*BLOCK_SIZE);
    int i;
    for (i = 0; i < BLOCK_SIZE; i++) {
    	hash_val[i] = '\0';
    }
    
    char input; //current char being xor'd
    int input_size = 0;
    
    // set hash_val to hash representation of user input
    while (fread(&input, sizeof(char), 1, f) != 0) {
        hash_val[input_size % BLOCK_SIZE]
            = input ^ hash_val[input_size % BLOCK_SIZE];
        input_size++;
    }
    
    return hash_val;
}

/* Compare hash1 and hash2, and return first index with unmatched hashes,
 * or block_size if all indices match */
int check_hash(const char *hash1, const char *hash2, long block_size) {
    int i;
    
    for (i = 0; i < block_size; i++) {
        if (hash1[i] != hash2[i]) {
        	   return i;
        }
     }
     
     return (int)block_size;
}

