#include <deque>

struct Envelope;
struct TransportState;
struct NotifyContext;

template <typename T>
class ThreadSafeQueue;

class Notifier
{
public:
    Notifier(
        ThreadSafeQueue<Envelope> &q,
        std::deque<Envelope> &squeue,
        NotifyContext *ctx)
        : queue(q), submissionQueue(squeue), notifyctx(ctx) {}
    void run();

private:
    ThreadSafeQueue<Envelope> &queue;
    std::deque<Envelope> &submissionQueue;
    NotifyContext *notifyctx;
};