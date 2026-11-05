#include "dispatcher.h"
#include "../runtime/threadSafeQueue.h"
#include "messageRouter.h"

//
#include <iostream>

void Dispatcher::run()
{
    std::cout << "dispatcher armed\n";

    while (auto incoming = queue.waitAndPop())
    {

        std::cout << "dispatcher woke with an envelope\n";

        auto it = router.registry.find(
            incoming->frame.header.type);

        if (it == router.registry.end())
        {
            std::cout << "wrong frame type\n";
            continue;
        }

        it->second->enqueue(std::move(*incoming));
    }
}