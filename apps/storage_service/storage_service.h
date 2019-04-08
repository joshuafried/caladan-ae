#pragma once

#define CMD_READ 0
#define CMD_WRITE 1

#define STORAGE_SERVICE_PORT 5000;

#define BUF_SIZE 4096

struct storage_cmd {
    uint8_t cmd;
    uint64_t lba;
    uint32_t lba_count;
    char data[BUF_SIZE];
};
