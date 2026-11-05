#include "../runtime/threadSafeQueue.h"
#include "envelope.h"
#include <unordered_map>

enum class ResponseType : uint16_t
{
    Chat = 1,
    Auth = 2,
    Video = 3
};

struct MessageRouter
{
    ThreadSafeQueue<Envelope> chatQueue;
    ThreadSafeQueue<Envelope> authQueue;
    ThreadSafeQueue<Envelope> videoQueue;

    std::unordered_map<uint16_t, ThreadSafeQueue<Envelope> *> registry;

    MessageRouter()
    {
        registry[(uint16_t)ResponseType::Chat] = &chatQueue;
        registry[(uint16_t)ResponseType::Auth] = &authQueue;
        registry[(uint16_t)ResponseType::Video] = &videoQueue;
    }
};