#pragma once
#include "frame.h"

using SessionId = uint32_t;

struct Envelope
{
    SessionId sessionId;
    Frame frame;
};
