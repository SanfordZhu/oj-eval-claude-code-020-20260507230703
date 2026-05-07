#include "buddy.h"
#include <stddef.h>
#include <string.h>

#define NULL ((void *)0)
#define PAGE_SIZE 4096
#define MAX_RANK 16

// Structure to represent a free block
typedef struct free_block {
    struct free_block *next;
} free_block_t;

// Global variables
static void *memory_pool = NULL;
static int total_pages = 0;
static free_block_t *free_lists[MAX_RANK + 1]; // 1-indexed
static int page_status[1 << 20]; // Assuming max 1M pages, stores rank of each page

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_block_pages(int rank) {
    return 1 << (rank - 1);
}

static int get_page_index(void *ptr) {
    if (ptr < memory_pool || ptr >= memory_pool + total_pages * PAGE_SIZE)
        return -1;
    return ((char *)ptr - (char *)memory_pool) / PAGE_SIZE;
}

static int get_buddy_index(int page_idx, int rank) {
    int block_pages = get_block_pages(rank);
    int block_start = (page_idx / block_pages) * block_pages;
    return block_start ^ block_pages;
}

static void *get_page_address(int page_idx) {
    return (char *)memory_pool + page_idx * PAGE_SIZE;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0)
        return -EINVAL;

    memory_pool = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize page status
    memset(page_status, 0, sizeof(page_status));

    // Add all pages as one big block of maximum rank
    int max_rank = 1;
    while (get_block_pages(max_rank + 1) <= total_pages && max_rank < MAX_RANK) {
        max_rank++;
    }

    // Create initial free block
    free_block_t *block = (free_block_t *)p;
    block->next = NULL;
    free_lists[max_rank] = block;

    // Mark all pages with the max rank
    int block_pages = get_block_pages(max_rank);
    for (int i = 0; i < block_pages; i++) {
        page_status[i] = max_rank;
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);

    // Find the smallest available block
    int current_rank = rank;
    while (current_rank <= MAX_RANK && !free_lists[current_rank]) {
        current_rank++;
    }

    if (current_rank > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    // Remove block from free list
    free_block_t *block = free_lists[current_rank];
    free_lists[current_rank] = block->next;

    // Split blocks if necessary
    while (current_rank > rank) {
        current_rank--;
        int block_size = get_block_size(current_rank);

        // Create buddy block
        free_block_t *buddy = (free_block_t *)((char *)block + block_size);
        buddy->next = free_lists[current_rank];
        free_lists[current_rank] = buddy;

        // Update page status for buddy
        int block_idx = get_page_index(block);
        int buddy_idx = block_idx + get_block_pages(current_rank);
        for (int i = 0; i < get_block_pages(current_rank); i++) {
            page_status[buddy_idx + i] = current_rank;
        }
    }

    // Update page status for allocated block
    int block_idx = get_page_index(block);
    for (int i = 0; i < get_block_pages(rank); i++) {
        page_status[block_idx + i] = -rank; // Negative means allocated
    }

    return block;
}

int return_pages(void *p) {
    if (!p)
        return -EINVAL;

    int page_idx = get_page_index(p);
    if (page_idx < 0 || page_idx >= total_pages)
        return -EINVAL;

    // Get the rank of the allocated block
    int rank = page_status[page_idx];
    if (rank >= 0)
        return -EINVAL; // Page is not allocated

    rank = -rank; // Convert to positive rank

    // Check if all pages in the block have the same rank
    int block_pages = get_block_pages(rank);
    for (int i = 1; i < block_pages; i++) {
        if (page_status[page_idx + i] != -rank)
            return -EINVAL;
    }

    // Mark pages as free
    for (int i = 0; i < block_pages; i++) {
        page_status[page_idx + i] = rank;
    }

    // Try to merge with buddy
    while (rank < MAX_RANK) {
        // Calculate buddy index - blocks are aligned to their size
        int block_start = page_idx & ~(block_pages - 1); // Align to block boundary
        int buddy_start = block_start ^ block_pages; // Buddy is at XOR with block size

        // Check if buddy is valid and free
        if (buddy_start < 0 || buddy_start >= total_pages ||
            buddy_start + block_pages > total_pages) {
            break;
        }

        // Check if buddy is completely free with same rank
        int buddy_ok = 1;
        for (int i = 0; i < block_pages; i++) {
            if (page_status[buddy_start + i] != rank) {
                buddy_ok = 0;
                break;
            }
        }

        if (!buddy_ok)
            break;

        // Find and remove buddy from free list
        free_block_t *buddy_block = (free_block_t *)get_page_address(buddy_start);

        // Since we don't have a prev pointer, we need to search
        // But we can optimize by checking if it's the head
        if (free_lists[rank] == buddy_block) {
            free_lists[rank] = buddy_block->next;
        } else {
            // Otherwise, we need to search - this is O(n) but should be rare
            free_block_t *prev = free_lists[rank];
            if (prev) {
                while (prev->next && prev->next != buddy_block) {
                    prev = prev->next;
                }
                if (prev->next == buddy_block) {
                    prev->next = buddy_block->next;
                }
            }
        }

        // Merge blocks - use the lower address
        if (buddy_start < block_start) {
            block_start = buddy_start;
        }

        // Update for next iteration
        rank++;
        block_pages = get_block_pages(rank);
        page_idx = block_start;

        // Update page status for merged block
        for (int i = 0; i < block_pages; i++) {
            page_status[block_start + i] = rank;
        }
    }

    // Add merged block to free list
    free_block_t *block = (free_block_t *)get_page_address(page_idx);
    block->next = free_lists[rank];
    free_lists[rank] = block;

    return OK;
}

int query_ranks(void *p) {
    if (!p)
        return -EINVAL;

    int page_idx = get_page_index(p);
    if (page_idx < 0)
        return -EINVAL;

    int rank = page_status[page_idx];
    if (rank < 0) {
        // Page is allocated, return the positive rank
        return -rank;
    } else {
        // Page is free, return its rank
        return rank;
    }
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return -EINVAL;

    int count = 0;
    free_block_t *block = free_lists[rank];
    while (block) {
        count++;
        block = block->next;
    }

    return count;
}