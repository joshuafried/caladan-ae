/*
 * storage.h - Storage
 */

#pragma once

#include <base/stddef.h>
#include <runtime/storage.h>

#if __has_include("spdk/nvme.h")

extern int storage_write(const void *payload, uint64_t lba, uint32_t lba_count);
extern int storage_read(void *dest, uint64_t lba, uint32_t lba_count);
extern unsigned int storage_block_size(void);
extern unsigned int storage_num_blocks(void);

#else

int storage_write(const void *payload,
		uint64_t lba, uint32_t lba_count)
{
	return -1;
}

int storage_read(void *dest, uint64_t lba, uint32_t lba_count)
{
	return -1;
}

unsigned int storage_block_size(void)
{
	return 0;
}

unsigned int storage_num_blocks(void)
{
	return 0;
}

#endif
