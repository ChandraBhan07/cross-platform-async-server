struct Envelope;
struct MessageRouter;

template <typename T>
class ThreadSafeQueue;

class Dispatcher
{
public:
    Dispatcher(MessageRouter &router,
               ThreadSafeQueue<Envelope> &queue) : router(router), queue(queue) {}
    void run();

private:
    MessageRouter &router;
    ThreadSafeQueue<Envelope> &queue;
};