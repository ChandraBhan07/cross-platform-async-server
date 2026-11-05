struct Envelope;

template <typename T>
class ThreadSafeQueue;

namespace Workers::chat
{
    void run(ThreadSafeQueue<Envelope> &inqueue,
               ThreadSafeQueue<Envelope> &outqueue);

}