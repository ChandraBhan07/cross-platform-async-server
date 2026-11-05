#pragma once
#include "../application/envelope.h"
#include "clientSession.h"

#include <unordered_map>

struct EventResources;
struct NotifyContext;

struct TransportState
{
    EventResources *resources = nullptr;
    NotifyContext *notifyctx = nullptr;

    std::deque<Envelope> submissionQueue;
    std::unordered_map<SessionId, std::unique_ptr<ClientSession>> sessions;
};