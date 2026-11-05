#include "notifier.h"
#include "transportState.h"
#include "notifyContext.h"
#include "../runtime/threadSafeQueue.h"

#include <unistd.h>

#include <iostream>

void Notifier::run()
{
    // transport side notifier / mediater to push frames into submission queue
    // narrow coupling bw application and transport layer through messagebus
    while (auto outgoing = queue.waitAndPop())
    {
        std::cout << "pushing to submission queue\n";
        
        submissionQueue.push_back(std::move(*outgoing));
        
        notifyctx->notifySubmission();
        std::cout << "notified iocp\n";
    }
}
