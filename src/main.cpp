
#include "runtime/threadSafeQueue.h"
#include "application/envelope.h"

#include "transport/transport.h"
#include "transport/transportState.h"
#include "transport/notifier.h"

#include "application/messageRouter.h"
#include "application/dispatcher.h"
#include "application/workers/workers.h"

#include <thread>

struct MessageBus
{
    ThreadSafeQueue<Envelope> incomingQueue;
    ThreadSafeQueue<Envelope> outgoingQueue;
};

int main()
{

    MessageBus bus;

    TransportState transportState;

    transport::initialize(transportState);
    std::thread transportThread(transport::run,
                                &transportState,
                                std::ref(bus.incomingQueue));

    MessageRouter router;
    Dispatcher dispatcher(router, bus.incomingQueue);
    std::thread dispatchThread(&Dispatcher::run, &dispatcher);

    std::thread chatWorkerThread(Workers::chat::run,
                                 std::ref(router.chatQueue),
                                 std::ref(bus.outgoingQueue));

    Notifier notifier(bus.outgoingQueue, transportState.submissionQueue, transportState.notifyctx);
    std::thread notifyThread(&Notifier::run, &notifier);

    transport::awaitShutdown(transportState);

    transportThread.join();

    // * owner should stop and not consumer thats why no stop fx in consumer
    bus.incomingQueue.stop();

    router.chatQueue.stop();
    router.authQueue.stop();
    router.videoQueue.stop();

    bus.outgoingQueue.stop();

    dispatchThread.join();
    chatWorkerThread.join();
    notifyThread.join();
    return 0;
}