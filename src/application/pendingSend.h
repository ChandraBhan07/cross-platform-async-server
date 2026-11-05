#pragma once
#include "frame.h"

struct PendingSend
{
    Frame frame;
    size_t bytesSent; // actually an offset to shift pointers
    PendingSend(Frame f) : frame(std::move(f)), bytesSent(0) {}

    size_t totalSize() const
    {
        return sizeof(Header) + frame.payload.len;
    }

    bool isComplete() const
    {
        return totalSize() == bytesSent;
    }
};