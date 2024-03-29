#include "mm.h"

/* size of a VM page on this system */
static size_t SYSTEM_PAGE_SIZE = 0;

/* pointer to the current head of the VM page lists containing struct records */
static vm_page_for_struct_records_t *vm_page_record_head = NULL;

/**
 * @brief Requests a virtual memory page.
 *
 * This function requests a virtual memory page by mapping it into the process's address space.
 *
 * @param units Number of units (pages) to request.
 * @return Pointer to the requested virtual memory page, or NULL if the request failed.
 */
static void *_mm_request_vm_page(uint32_t units)
{
    /* the virtual mapping should be done in the heap */
    uint8_t *vm_page =
        mmap(sbrk(0), units * SYSTEM_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (vm_page == MAP_FAILED)
    {
        return NULL;
    }
    memset(vm_page, 0, units * SYSTEM_PAGE_SIZE);

    return (void *)vm_page;
}

/**
 * @brief Releases a virtual memory page.
 *
 * This function releases a virtual memory page by unmapping it from the process's address space.
 *
 * @param vm_page Pointer to the virtual memory page to release.
 * @param units Number of units (pages) to release.
 * @return 0 if the page was successfully released, -1 otherwise.
 */
static int8_t _mm_release_vm_page(void *vm_page, uint32_t units)
{
    return munmap(vm_page, units * SYSTEM_PAGE_SIZE);
}

/**
 * @brief Looks up a struct_record_t object by struct name.
 *
 * This function searches for a struct_record_t object with a matching struct name
 * within the linked list of struct records. It iterates through the list of vm_page_for_struct_records_t
 * objects to find the desired record. If a matching record is found, it is returned.
 *
 * @param struct_name Pointer to the struct name to search for.
 * @return Pointer to the matching struct_record_t object, or NULL if not found.
 */
static struct_record_t *_mm_lookup_struct_record_by_name(const char *struct_name)
{
    for (vm_page_for_struct_records_t *vm_page_record = vm_page_record_head; vm_page_record != NULL;
         vm_page_record = vm_page_record->next)
    {
        struct_record_t *record = NULL;
        MM_ITERATE_STRUCT_RECORDS_BEGIN(vm_page_record->struct_record_list, record)
        {
            if (strncmp(record->struct_name, struct_name, MM_MAX_STRUCT_NAME_SIZE) == 0)
            {
                return record;
            }
        }
        MM_ITERATE_STRUCT_RECORDS_END;
    }

    return NULL;
}

/**
 * @brief Merges two free memory blocks into a single block.
 *
 * This function merges two free memory blocks into a single block. The function assumes
 * that both input blocks are free (is_free == MM_FREE). It updates the data_block_size,
 * next, and prev fields of the first block to incorporate the second block into a single block.
 *
 * @param first Pointer to the first meta_block_t object.
 * @param second Pointer to the second meta_block_t object.
 */
static void _mm_merge_free_blocks(meta_block_t *first, meta_block_t *second)
{
    assert((first->is_free == MM_FREE) && (second->is_free == MM_FREE));

    first->data_block_size += sizeof(meta_block_t) + second->data_block_size;
    first->next = second->next;
    if (second->next != NULL)
    {
        first->next->prev = first;
    }
}

/**
 * @brief Checks if a data virtual memory page is empty.
 *
 * This function checks if a data virtual memory page is empty by examining its meta_block_info
 * fields, including next, prev, and is_free.
 *
 * @param data_vm_page Pointer to the vm_page_for_data_t object to check.
 * @return MM_FREE if the page is empty, MM_ALLOCATED otherwise.
 */
static vm_bool_t _mm_is_data_vm_page_empty(vm_page_for_data_t *data_vm_page)
{
    if (data_vm_page->meta_block_info.next == NULL && data_vm_page->meta_block_info.prev == NULL &&
        data_vm_page->meta_block_info.is_free == MM_FREE)
    {
        return MM_FREE;
    }

    return MM_ALLOCATED;
}

/**
 * @brief Calculates the maximum amount of virtual memory page memory available.
 *
 * This function calculates the maximum amount of virtual memory page memory available,
 * taking into account the system page size and the offset of the page memory within
 * the vm_page_for_data_t structure.
 *
 * @param units Number of units to calculate memory for.
 * @return Maximum memory available for the specified number of units, in bytes.
 */
static uint32_t _mm_max_vm_page_memory_available(uint32_t units)
{
    return (uint32_t)((SYSTEM_PAGE_SIZE * units) - MM_BLOCK_OFFSETOF(vm_page_for_data_t, page_memory));
}

/**
 * @brief Allocates a virtual memory page for data.
 *
 * This function allocates a virtual memory page for data and initializes its fields.
 *
 * @param record Pointer to the struct_record_t object associated with the data page.
 * @return Pointer to the allocated vm_page_for_data_t object.
 */
static vm_page_for_data_t *mm_allocate_data_vm_page(struct_record_t *record)
{
    vm_page_for_data_t *data_vm_page = (vm_page_for_data_t *)_mm_request_vm_page(1);

    MM_MARK_DATA_VM_PAGE_FREE(data_vm_page);

    data_vm_page->meta_block_info.data_block_size = _mm_max_vm_page_memory_available(1);
    data_vm_page->meta_block_info.offset = MM_BLOCK_OFFSETOF(vm_page_for_data_t, meta_block_info);
    data_vm_page->next = NULL;
    data_vm_page->prev = NULL;
    data_vm_page->record = record;
    glthread_init_node(&data_vm_page->meta_block_info.glue_node);

    if (!record->first_page)
    {
        record->first_page = data_vm_page;
    }
    else
    {
        data_vm_page->next = record->first_page;
        record->first_page->prev = data_vm_page;
        record->first_page = data_vm_page;
    }

    return data_vm_page;
}

/**
 * @brief Deletes and frees a data virtual memory page.
 *
 * This function deletes and frees a data virtual memory page. It removes the page from the associated struct_record_t's
 * page list and releases the virtual memory page using `_mm_release_vm_page`.
 *
 * @param data_vm_page Pointer to the data virtual memory page to delete and free.
 */
static void _mm_delete_and_free_data_vm_page(vm_page_for_data_t *data_vm_page)
{
    struct_record_t *record = data_vm_page->record;

    if (record->first_page == data_vm_page)
    {
        record->first_page = data_vm_page->next;
        if (data_vm_page->next)
        {
            data_vm_page->next->prev = NULL;
        }
        data_vm_page->next = NULL;
        data_vm_page->prev = NULL;
    }
    else
    {
        if (data_vm_page->next)
        {
            data_vm_page->next->prev = data_vm_page->prev;
            data_vm_page->prev->next = data_vm_page->next;
        }
    }

    _mm_release_vm_page((void *)data_vm_page, 1);
}

/**
 * @brief Compares two free memory blocks for sorting.
 *
 * This function compares two free memory blocks for sorting purposes. It compares the data_block_size
 * field of the meta_block_t objects. The function assumes that both input blocks are free (is_free == MM_FREE).
 *
 * @param meta_block_A Pointer to the first meta_block_t object.
 * @param meta_block_B Pointer to the second meta_block_t object.
 * @return -1 if the first block has a larger data_block_size, 1 if the second block has a larger data_block_size,
 *         or 0 if both blocks have the same data_block_size.
 */
static int8_t _mm_free_block_comparison(void *meta_block_A, void *meta_block_B)
{
    meta_block_t *meta_block_A_ptr = (meta_block_t *)meta_block_A;
    meta_block_t *meta_block_B_ptr = (meta_block_t *)meta_block_B;

    assert((meta_block_A_ptr->is_free == MM_FREE) && (meta_block_B_ptr->is_free == MM_FREE));

    if (meta_block_A_ptr->data_block_size > meta_block_B_ptr->data_block_size)
    {
        return -1;
    }
    else if (meta_block_A_ptr->data_block_size < meta_block_B_ptr->data_block_size)
    {
        return 1;
    }

    return 0;
}

/**
 * @brief Adds free data block meta information to a struct_record_t object.
 *
 * This function adds free data block meta information to a struct_record_t object by inserting
 * the meta_block_t object into the free_block_priority_list of the struct_record_t. The insertion
 * is done based on priority using the _mm_free_block_comparison function.
 *
 * @param record Pointer to the struct_record_t object.
 * @param free_meta_block Pointer to the meta_block_t object representing the free data block.
 */
static void _mm_add_free_data_block_meta_info(struct_record_t *record, meta_block_t *free_meta_block)
{
    assert(free_meta_block->is_free == MM_FREE);

    glthread_priority_insert(&record->free_block_priority_list, &free_meta_block->glue_node, _mm_free_block_comparison,
                             MM_BLOCK_OFFSETOF(meta_block_t, glue_node));
}

/**
 * @brief Retrieves the largest free data block from a struct_record_t object.
 *
 * This function retrieves the largest free data block from a struct_record_t object
 * by accessing the head of the free_block_priority_list and converting it to a pointer
 * to the meta_block_t object. The largest free data block is determined based on the
 * priority order defined by the _mm_free_block_comparison function.
 *
 * @param record Pointer to the struct_record_t object.
 * @return Pointer to the largest free meta_block_t object, or NULL if the list is empty.
 */
static meta_block_t *_mm_get_largest_free_data_block(struct_record_t *record)
{
    if(record->free_block_priority_list.head != NULL)
    {
        return (meta_block_t *)(GLTHREAD_BASEOF(record->free_block_priority_list.head,
                                            MM_BLOCK_OFFSETOF(meta_block_t, glue_node)));
    }
    else
    {
        return NULL;
    }
}

/**
 * @brief Binds meta blocks after splitting.
 *
 * This function binds meta blocks after splitting. It updates the next and prev pointers of the
 * allocated meta block and the free meta block to maintain the correct linkage. If the allocated
 * meta block has a next block, it updates the next block's prev pointer to point to the free meta
 * block. Finally, it updates the next and prev pointers of the allocated and free meta blocks.
 *
 * @param allocated_meta_block Pointer to the allocated meta block.
 * @param free_meta_block Pointer to the free meta block.
 */
static void _mm_bind_blocks_after_splitting(meta_block_t *allocated_meta_block, meta_block_t *free_meta_block)
{
    free_meta_block->next = allocated_meta_block->next;
    free_meta_block->prev = allocated_meta_block;
    if (allocated_meta_block->next)
    {
        allocated_meta_block->next->prev = free_meta_block;
    }
    allocated_meta_block->next = free_meta_block;
}

/**
 * @brief Splits a free data block for allocation.
 *
 * This function splits a free data block for allocation. It first checks if the meta block is free,
 * and if not, it asserts an error. It then checks if the requested size exceeds the available size
 * in the data block, and if so, returns false to indicate that the allocation cannot be done. If the
 * requested size can be accommodated, the function splits the free data block based on the remaining size.
 * There are four possible cases: no splitting, partial splitting with soft internal fragmentation, partial
 * splitting with hard internal fragmentation, and full splitting. In each case, the function updates the
 * meta block information, adds the free meta block to the meta block list, and binds the blocks after splitting.
 * Finally, it returns true to indicate successful splitting and allocation.
 *
 * @param record Pointer to the structure record.
 * @param meta_block_info Pointer to the meta block information.
 * @param req_size The requested size of the data block to allocate.
 * @return True if the splitting and allocation were successful, false otherwise.
 */
static bool _mm_split_free_data_block_for_allocation(struct_record_t *record, meta_block_t *meta_block_info,
                                                     uint32_t req_size)
{
    assert(meta_block_info->is_free == MM_FREE);

    meta_block_t *next_meta_block_info = NULL;

    /* memory allocation request cannot be done as requested size is greater than available size in the data block */
    if (meta_block_info->data_block_size < req_size)
    {
        return false;
    }

    uint32_t remaining_size = meta_block_info->data_block_size - req_size;

    meta_block_info->is_free = MM_ALLOCATED;
    meta_block_info->data_block_size = req_size;
    /* remove the meta block from the priority queue */
    glthread_remove_node(&record->free_block_priority_list, &meta_block_info->glue_node);

    if (remaining_size == 0) /* case 1: no splitting */
    {
        return true;
    }
    else if (sizeof(meta_block_t) < remaining_size &&
             remaining_size <
                 sizeof(meta_block_t) + record->size) /* case 2: partial splitting [soft internal fragmentation] */
    {
        next_meta_block_info = MM_NEXT_META_BLOCK_BY_SIZE(meta_block_info);
        next_meta_block_info->is_free = MM_FREE;
        next_meta_block_info->data_block_size = remaining_size - sizeof(meta_block_t);
        next_meta_block_info->offset =
            meta_block_info->offset + sizeof(meta_block_t) + meta_block_info->data_block_size;
        _mm_add_free_data_block_meta_info(record, next_meta_block_info);
        _mm_bind_blocks_after_splitting(meta_block_info, next_meta_block_info);
    }
    else if (remaining_size < sizeof(meta_block_t)) /* case 3: partial splitting [hard internal fragmentation] */
    {
        /* no action required */
    }
    else /* case 4: full split [same as case 2] */
    {
        next_meta_block_info = MM_NEXT_META_BLOCK_BY_SIZE(meta_block_info);
        next_meta_block_info->is_free = MM_FREE;
        next_meta_block_info->data_block_size = remaining_size - sizeof(meta_block_t);
        next_meta_block_info->offset =
            meta_block_info->offset + sizeof(meta_block_t) + meta_block_info->data_block_size;
        _mm_add_free_data_block_meta_info(record, next_meta_block_info);
        _mm_bind_blocks_after_splitting(meta_block_info, next_meta_block_info);
    }

    return true;
}

/**
 * @brief Allocates a free data block for a given structure record.
 *
 * This function allocates a free data block for a given structure record. It first checks if there
 * is a free data block with sufficient size in the record. If not, it adds a new page for the record
 * and allocates memory from the free data block of the newly added VM data page. If there is a free data
 * block with sufficient size, it allocates memory from the largest data block in the priority queue.
 * The function splits the free data block for allocation and returns a pointer to the allocated data
 * block. If allocation fails, it returns NULL.
 *
 * @param record Pointer to the structure record.
 * @param req_size The requested size of the data block to allocate.
 * @return A pointer to the allocated meta_block_t data block, or NULL if allocation fails.
 */
static meta_block_t *_mm_allocate_free_data_block(struct_record_t *record, uint32_t req_size)
{
    vm_page_for_data_t *data_vm_page = NULL;

    meta_block_t *largest_free_meta_block = _mm_get_largest_free_data_block(record);
    if (largest_free_meta_block == NULL || largest_free_meta_block->data_block_size < req_size)
    {
        /* add a new page for this record */
        data_vm_page = mm_allocate_data_vm_page(record);

        /* allocate memory from the free data block of the newly added VM data page */
        bool status = _mm_split_free_data_block_for_allocation(record, &data_vm_page->meta_block_info, req_size);

        return (status ? &data_vm_page->meta_block_info : NULL);
    }
    else
    {
        /* allocate memory from the largest data block from the priority queue */
        bool status = _mm_split_free_data_block_for_allocation(record, largest_free_meta_block, req_size);

        return (status ? largest_free_meta_block : NULL);
    }
}

/**
 * @brief Calculates the size of hard internal fragmentation between two meta blocks.
 *
 * This function calculates the size of hard internal fragmentation between two meta blocks. It takes two meta block
 * pointers, `first` and `second`, and calculates the size by subtracting the address of the next meta block from the
 * address of the second meta block. It returns the size of hard internal fragmentation as a `uint32_t` value.
 *
 * @param first Pointer to the first meta block.
 * @param second Pointer to the second meta block.
 * @return The size of hard internal fragmentation between the two meta blocks.
 */
static uint32_t _mm_get_hard_internal_frag_size(meta_block_t *first, meta_block_t *second)
{
    meta_block_t *next_meta_block = MM_NEXT_META_BLOCK_BY_SIZE(first);
    return (uint32_t)((uint8_t *)second - (uint8_t *)next_meta_block);
}

/**
 * @brief Frees a data block and performs block merging if possible.
 *
 * This function frees a data block specified by the `app_data_meta_block` pointer. It marks the block as free by setting
 * its `is_free` field to `MM_FREE`. It then checks if the freed block is followed by another free block (`next_meta_block`)
 * or if it is the last block in the VM page. In case 1, it merges the freed block with the next free block, and in case 2,
 * it calculates the size of hard internal fragmentation by subtracting the end of the freed block from the end of the VM
 * page. The function performs block merging with the previous free block (`prev_meta_block`) if applicable.
 *
 * After merging, the function checks if the data VM page becomes empty. If it does, the data VM page is deleted and freed.
 * Finally, the merged meta block is added to the free meta block priority queue.
 *
 * @param app_data_meta_block Pointer to the meta block of the data block to be freed.
 */
static void _mm_free_data_block(meta_block_t *app_data_meta_block)
{
    vm_page_for_data_t *hosting_data_vm_page = (vm_page_for_data_t *)MM_GET_PAGE_FROM_META_BLOCK(app_data_meta_block);

    app_data_meta_block->is_free = MM_FREE;

    meta_block_t *next_meta_block = MM_NEXT_META_BLOCK(app_data_meta_block);
    if(next_meta_block != NULL)     /* case 1: if the meta block is not the last block in the VM page, also handling hard IF memory if present */
    {
        app_data_meta_block->data_block_size += _mm_get_hard_internal_frag_size(app_data_meta_block, next_meta_block);
    }
    else    /* case 2: if the meta block is the last block in the VM page, also handling hard IF memory if present */
    {
        uint8_t *data_vm_page_end = (uint8_t *)((uint8_t *)hosting_data_vm_page + SYSTEM_PAGE_SIZE);
        uint8_t *app_data_block_end = (uint8_t *)app_data_meta_block + sizeof(meta_block_t) + app_data_meta_block->data_block_size;
        app_data_meta_block->data_block_size += (uint32_t)((uintptr_t)((void *)data_vm_page_end) - (uintptr_t)((void *)app_data_block_end));
    }

    meta_block_t *final_merged_meta_block = app_data_meta_block;

    /* perform block merging */
    if(next_meta_block != NULL && next_meta_block->is_free == MM_FREE)
    {
        _mm_merge_free_blocks(app_data_meta_block, next_meta_block);
    }

    meta_block_t *prev_meta_block = MM_PREV_META_BLOCK(app_data_meta_block);
    if(prev_meta_block != NULL && prev_meta_block->is_free == MM_FREE)
    {
        _mm_merge_free_blocks(prev_meta_block, app_data_meta_block);
        final_merged_meta_block = prev_meta_block;
    }

    /* check if the data VM page empty */
    if(_mm_is_data_vm_page_empty(hosting_data_vm_page) == MM_FREE)
    {
        _mm_delete_and_free_data_vm_page(hosting_data_vm_page);
        return;
    }

    /* add the final meta block to the free meta block PQ */
    _mm_add_free_data_block_meta_info(hosting_data_vm_page->record, final_merged_meta_block);
}

/**
 * @brief Initializes the memory management system.
 *
 * This function initializes the memory management system by retrieving the system page size
 * using the `sysconf` function and storing it in the `SYSTEM_PAGE_SIZE` global variable.
 * It is typically called at the start of the program to set up the memory management system.
 */
void mm_init(void)
{
    SYSTEM_PAGE_SIZE = sysconf(_SC_PAGESIZE);
}

/**
 * @brief Registers a struct record in the memory management system.
 *
 * This function registers a struct record in the memory management system. The struct record
 * represents a struct with the given name and size. The function checks if the size exceeds
 * the system page size and returns -1 if it does. If the struct name already exists in the
 * record list, it returns -2. Otherwise, it creates a new virtual memory page if needed and
 * inserts the struct record into the appropriate VM page or creates a new VM page for it.
 *
 * @param struct_name The name of the struct to register.
 * @param size The size of the struct.
 * @return 0 if the struct record is registered successfully, -1 if the size exceeds the system page size,
 *         -2 if the struct name already exists in the record list.
 */
int8_t mm_register_struct_record(const char *struct_name, size_t size)
{
    /* we cannot accomodate a struct whose size is greater than the page size */
    if (size > SYSTEM_PAGE_SIZE)
    {
        return -1;
    }
    else
    {
        /* allocating a VM page for the first time */
        if (!vm_page_record_head)
        {
            vm_page_record_head = (vm_page_for_struct_records_t *)_mm_request_vm_page(1);
            vm_page_record_head->next = NULL;
            strncpy(vm_page_record_head->struct_record_list[0].struct_name, struct_name, MM_MAX_STRUCT_NAME_SIZE);
            vm_page_record_head->struct_record_list[0].size = size;
            vm_page_record_head->struct_record_list[0].first_page = NULL;
            glthread_init(&vm_page_record_head->struct_record_list[0].free_block_priority_list);
        }
        else
        {
            uint32_t count = 0;
            struct_record_t *record = NULL;
            /* iterating through records in all existing VM pages to insert a record
             * at the end */
            for (vm_page_for_struct_records_t *vm_page_record = vm_page_record_head; vm_page_record != NULL;
                 vm_page_record = vm_page_record->next)
            {
                count = 0;
                record = NULL;
                MM_ITERATE_STRUCT_RECORDS_BEGIN(vm_page_record->struct_record_list, record)
                {
                    if (strncmp(record->struct_name, struct_name, MM_MAX_STRUCT_NAME_SIZE) == 0)
                    {
                        /* struct name already exists in the record list */
                        return -2;
                    }
                    else
                    {
                        count++;
                    }
                }
                MM_ITERATE_STRUCT_RECORDS_END;
            }

            if (count == MM_MAX_RECORDS_PER_VM_PAGE)
            {
                /* the previous VM page is full, create a new VM page to add this record
                 */
                vm_page_for_struct_records_t *new_vm_page_record =
                    (vm_page_for_struct_records_t *)_mm_request_vm_page(1);
                new_vm_page_record->next = vm_page_record_head;
                /* the record to be added will be the first record in the new VM page*/
                record = new_vm_page_record->struct_record_list;
                vm_page_record_head = new_vm_page_record;
            }

            strncpy(record->struct_name, struct_name, MM_MAX_STRUCT_NAME_SIZE);
            record->size = size;
            record->first_page = NULL;
            glthread_init(&record->free_block_priority_list);
        }
        return 0;
    }
}

/**
 * @brief Prints the registered struct records in the memory management system.
 *
 * This function prints the registered struct records in the memory management system.
 * It iterates through the linked list of vm_page_for_struct_records_t objects and the
 * struct_record_list within each object, printing the struct name and size of each record.
 */
void mm_print_registered_struct_records(void)
{
    for (vm_page_for_struct_records_t *vm_page_record = vm_page_record_head; vm_page_record != NULL;
         vm_page_record = vm_page_record->next)
    {
        struct_record_t *record = NULL;
        MM_ITERATE_STRUCT_RECORDS_BEGIN(vm_page_record->struct_record_list, record)
        {
            printf("%s: %ld\n", record->struct_name, record->size);
        }
        MM_ITERATE_STRUCT_RECORDS_END;
    }
}

/**
 * @brief Prints memory usage statistics for registered structure records.
 *
 * This function prints memory usage statistics for registered structure records. It first prints the page size.
 * Then, it iterates through the VM pages and structure records. If a specific struct name is provided, it prints
 * the statistics for that struct only. Otherwise, it prints the statistics for all registered structs. For each
 * struct, it iterates through the data VM pages and meta blocks to print detailed information about each block,
 * including its address, status, size, offset, previous block, and next block.
 *
 * @param struct_name Optional. The name of the struct to print statistics for. If NULL, print stats for all registered structs.
 */
void mm_print_mem_usage(const char *struct_name)
{
    printf("\nPage Size = %zd\n\n", SYSTEM_PAGE_SIZE);

    for (vm_page_for_struct_records_t *vm_page_record = vm_page_record_head; vm_page_record != NULL;
         vm_page_record = vm_page_record->next)
    {
        struct_record_t *record = NULL;
        MM_ITERATE_STRUCT_RECORDS_BEGIN(vm_page_record->struct_record_list, record)
        {
            vm_page_for_data_t *data_vm_page_ptr = NULL;
            if(struct_name != NULL) /* print stats of specified struct */
            {
                if(strncmp(record->struct_name, struct_name, MM_MAX_STRUCT_NAME_SIZE) == 0)
                {
                    printf("%s: %ld\n", record->struct_name, record->size);
                    uint32_t page_num = 0;
                    MM_ITERATE_DATA_VM_PAGES_BEGIN(record, data_vm_page_ptr)
                    {
                        meta_block_t *meta_block_ptr = NULL;
                        uint32_t block_count = 0;
                        printf("\tPage Number: %d\n", page_num++);
                        MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_BEGIN(data_vm_page_ptr, meta_block_ptr)
                        {
                            printf("\t\t\t%14p\tBlock: %5d\tStatus: %s\tBlock Size: %5d\tOffset: %5d\tPrev: %14p\tNext: %14p\n", (void *)meta_block_ptr, block_count, meta_block_ptr->is_free ? "ALLOCATED" : "F R E E D", meta_block_ptr->data_block_size, meta_block_ptr->offset, (void *)meta_block_ptr->prev, (void *)meta_block_ptr->next);
                            block_count++;
                        }MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_END;
                    }
                    MM_ITERATE_DATA_VM_PAGES_END;
                    
                    return;
                }
            }
            else /* print stats of all registered structs */
            {
                printf("%s: %ld\n", record->struct_name, record->size);
                uint32_t page_num = 0;
                MM_ITERATE_DATA_VM_PAGES_BEGIN(record, data_vm_page_ptr)
                {
                    meta_block_t *meta_block_ptr = NULL;
                    uint32_t block_count = 0;
                    printf("\tPage Number: %d\n", page_num++);
                    MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_BEGIN(data_vm_page_ptr, meta_block_ptr)
                    {
                        printf("\t\t\t%14p\tBlock: %5d\tStatus: %s\tBlock Size: %5d\tOffset: %5d\tPrev: %14p\tNext: %14p\n", (void *)meta_block_ptr, block_count, meta_block_ptr->is_free ? "ALLOCATED" : "F R E E D", meta_block_ptr->data_block_size, meta_block_ptr->offset, (void *)meta_block_ptr->prev, (void *)meta_block_ptr->next);
                        block_count++;
                    }MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_END;
                }
                MM_ITERATE_DATA_VM_PAGES_END;
            }
        }
        MM_ITERATE_STRUCT_RECORDS_END;
    }
}

/**
 * @brief Prints the block usage statistics for all registered structure records.
 *
 * This function prints the block usage statistics for all registered structure records. It iterates through
 * the VM pages and structure records, and for each record, it iterates through the data VM pages and meta blocks
 * to calculate the number of allocated blocks and free blocks. It also calculates the application memory usage
 * by multiplying the number of allocated blocks with the size of each block. The statistics are then printed
 * for each structure record.
 */
void mm_print_block_usage(void)
{
    printf("\n");
    for (vm_page_for_struct_records_t *vm_page_record = vm_page_record_head; vm_page_record != NULL;
         vm_page_record = vm_page_record->next)
    {
        struct_record_t *record = NULL;
        MM_ITERATE_STRUCT_RECORDS_BEGIN(vm_page_record->struct_record_list, record)
        {
            printf("%-20s\t", record->struct_name);
            vm_page_for_data_t *data_vm_page_ptr = NULL;
            uint32_t allocated_block_count = 0;
            uint32_t free_block_count = 0;
            MM_ITERATE_DATA_VM_PAGES_BEGIN(record, data_vm_page_ptr)
            {
                meta_block_t *meta_block_ptr = NULL;
                MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_BEGIN(data_vm_page_ptr, meta_block_ptr)
                {
                    if(meta_block_ptr->is_free == MM_ALLOCATED)
                    {
                        allocated_block_count++;
                    }
                    else
                    {
                        free_block_count++;
                    }
                }
                MM_ITERATE_ALL_BLOCKS_OF_SINGLE_DATA_VM_PAGE_END;
            }
            MM_ITERATE_DATA_VM_PAGES_END;
            printf("TBC: %5d\tFBC: %5d\tABC: %5d\tAppMemUsage: %10ld\n", allocated_block_count + free_block_count, free_block_count, allocated_block_count, allocated_block_count * (sizeof(meta_block_t) + record->size));
        }
        MM_ITERATE_STRUCT_RECORDS_END;
    }
}

/**
 * @brief Allocates and initializes memory for a structure array.
 *
 * This function allocates and initializes memory for a structure array of the specified size.
 * It performs checks to ensure that the structure has been registered and that the requested
 * memory size is within the available memory in a completely free VM data page. It then finds
 * a suitable data block to satisfy the memory request and initializes it with zeros. Finally,
 * it returns a pointer to the allocated and initialized memory.
 *
 * @param struct_name The name of the structure to allocate memory for.
 * @param units The number of structure units to allocate.
 * @return A pointer to the allocated and initialized memory, or NULL if allocation failed or
 *         the structure is not registered.
 */
void *xcalloc(const char *struct_name, uint32_t units)
{
    /* we cannot allocate memory for a struct that has not been registered */
    struct_record_t *record = _mm_lookup_struct_record_by_name(struct_name);
    if (record == NULL)
    {
        return NULL;
    }

    /* we cannot allocate memory that is greater than the memory available in a completely free VM data page */
    if (units * record->size > _mm_max_vm_page_memory_available(1))
    {
        return NULL;
    }

    /* find a data block that can satisfy the memory request from the application */
    meta_block_t *free_meta_block = _mm_allocate_free_data_block(record, record->size * units);

    if (free_meta_block)
    {
        memset((uint8_t *)(free_meta_block + 1), 0, free_meta_block->data_block_size);
        return (void *)(free_meta_block + 1);
    }
    else
    {
        return NULL;
    }
}

/**
 * @brief Frees a dynamically allocated memory block.
 *
 * The `xfree` function frees the memory block pointed to by `app_data`. It calculates the pointer to the meta block
 * corresponding to the data block by subtracting the size of the meta block from the `app_data` pointer. The function
 * asserts that the meta block's `is_free` field is already set to `MM_FREE` to ensure that the block is not already freed.
 *
 * After the assertion, the function calls `_mm_free_data_block` to perform the actual freeing of the data block,
 * including block merging and memory management operations.
 *
 * @param app_data Pointer to the dynamically allocated memory block to be freed.
 */
void xfree(void *app_data)
{
    meta_block_t *app_data_meta_block = (meta_block_t *)((uint8_t *)app_data - sizeof(meta_block_t));

    assert(app_data_meta_block->is_free == MM_ALLOCATED);

    _mm_free_data_block(app_data_meta_block);
}
