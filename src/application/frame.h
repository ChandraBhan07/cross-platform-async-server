#pragma once
#include <memory>
#include <stdint.h>

struct __attribute__((packed)) Header
{
    uint16_t magic;
    uint8_t version;
    uint8_t flags;
    uint8_t type;
    uint32_t length;
    uint8_t reserved;
};

struct OwnedBuffer
{
    std::unique_ptr<uint8_t[]> data;
    uint32_t len;
};

struct Frame
{
    Header header;
    OwnedBuffer payload;
};