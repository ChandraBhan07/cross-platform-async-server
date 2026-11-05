#include "workers.h"
#include "../../runtime/threadSafeQueue.h"

// delete later
#include <string>
#include <cstring>
#include <iostream>
#include "../frame.h"
#include "../envelope.h"

namespace Workers::chat
{
    void run(ThreadSafeQueue<Envelope> &inqueue,
             ThreadSafeQueue<Envelope> &outqueue)
    {
        while (auto incoming = inqueue.waitAndPop())
        {
            std::cout << "Chat worker got frame\nand it says ";
            std::string text(
                reinterpret_cast<const char *>(incoming->frame.payload.data.get()), incoming->frame.payload.len);

            std::cout << text << "\n";

            const char *msg = "Hello client, greeting received!";

            // process frame
            Frame f;
            f.header.magic = 0xCAFE;
            f.header.version = 0x01;
            f.header.flags = 0x00;
            f.header.type = 0x01;
            f.header.length = strlen(msg);
            f.header.reserved = 0x00;

            f.payload.len = strlen(msg);
            f.payload.data =
                std::make_unique<uint8_t[]>(f.payload.len);

            std::memcpy(
                f.payload.data.get(), msg, f.payload.len);

            Envelope outgoing;
            outgoing.frame = std::move(f);
            outgoing.sessionId = incoming->sessionId;
            outqueue.enqueue(std::move(outgoing));
            std::cout<<"chat worker pushed into pipeline\n";
        }
    }
}