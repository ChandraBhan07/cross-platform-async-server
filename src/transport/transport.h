#pragma once

struct Envelope;
struct TransportState;

template <typename T>
class ThreadSafeQueue;

namespace transport
{
    void initialize(TransportState &);
    void run(TransportState *, ThreadSafeQueue<Envelope> &);
    void awaitShutdown(TransportState &);
}