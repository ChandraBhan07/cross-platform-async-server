#include "../transportState.h"
#include "../clientSession.h"
#include "../notifyContext.h"

#include "../../runtime/threadSafeQueue.h"
#include "../../application/pendingSend.h"
#include "../../application/envelope.h"

#include <winsock2.h>
#include <mswsock.h>
#include <cstring>
#include <functional>
#include <atomic>
#include <memory>

#include <iostream>

namespace transport
{
    HANDLE shutdownHandle = nullptr;
}

BOOL WINAPI shutdownHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT)
    {
        std::cout << "Ctrl+C detected, signaling shutdown...\n";
        SetEvent(transport::shutdownHandle);
        return true;
    }
    return false;
}

// transport logic

enum class OperationType
{
    Accept,
    Recv,
    Send,
    Wakeup,
    Shutdown
};

struct Operation
{
    OVERLAPPED overlapped{};
    OperationType type;
    explicit Operation(OperationType t) : type(t) {}
};

struct AcceptOperation : Operation
{
    SOCKET acceptSocket;
    char addressBuffer[1024];
    AcceptOperation(SOCKET socket) : Operation(OperationType::Accept),
                                     acceptSocket(std::move(socket)) {}
};

struct RecvOperation : Operation
{
    WSABUF buffer;
    char storage[4096];
    RecvOperation() : Operation(OperationType::Recv)
    {
        buffer.buf = storage;
        buffer.len = sizeof(storage);
    }
};

struct SendOperation : Operation
{
    WSABUF buffs[2];
    DWORD buffsCount = 2;
    PendingSend *pendingSend;

    SendOperation(PendingSend *task)
        : Operation(OperationType::Send),
          pendingSend(task)
    {
        prepareBuffers();
    }

    void prepareBuffers()
    {
        size_t TEST_CHUNKS = 5;

        // case 1 bytesSent < sizeof header so we have to send part(Header) + entire(payload)
        size_t offset = pendingSend->bytesSent;
        if (offset < sizeof(Header))
        {
            buffs[0].buf = reinterpret_cast<char *>(&pendingSend->frame.header) + offset;
            buffs[0].len = sizeof(Header) - offset;

            buffs[1].buf = reinterpret_cast<char *>(pendingSend->frame.payload.data.get());
            buffs[1].len = pendingSend->frame.payload.len;

            // limit visible bytes
            if (buffs[0].len >= TEST_CHUNKS)
            {
                buffs[0].len = TEST_CHUNKS;
                buffs[1].len = 0;
            }
            else
            {
                buffs[1].len = std::min(pendingSend->frame.payload.len, (uint32_t)(TEST_CHUNKS - sizeof(Header) + offset));
            }
        }
        else
        {
            // case 2 bytesSent > size of header so header is sent, we send part of payload
            size_t payloadoffset = offset - sizeof(Header);
            buffs[0].buf = reinterpret_cast<char *>(pendingSend->frame.payload.data.get()) + payloadoffset;
            buffs[0].len = pendingSend->frame.payload.len - payloadoffset;

            buffs[0].len = std::min(pendingSend->frame.payload.len - payloadoffset, TEST_CHUNKS);
        }
    }
};

struct WakeUpOperation : Operation
{
    WakeUpOperation() : Operation(OperationType::Wakeup) {}
};

struct ShutdownOperation : Operation
{
    ShutdownOperation() : Operation(OperationType::Shutdown) {}
};

// client context is a bridge to connect clientsession and transport resources
// see that client session is dumb and does not contain socket
// but it makes client session independent of platforms
struct ClientContext
{
    SOCKET socket;
    ClientSession *session;
    ClientContext(SOCKET s, ClientSession *cs) : socket(std::move(s)), session(cs) {}

    std::unique_ptr<RecvOperation> recvOp;
    std::unique_ptr<SendOperation> sendOp;
};

struct EventResources
{
    HANDLE iocp;
    SOCKET listener;

    LPFN_ACCEPTEX acceptExPtr;
    std::unique_ptr<AcceptOperation> acceptOp;

    std::unique_ptr<WakeUpOperation> wakeupOp;
    std::atomic<bool> wakeupOutstanding{false};

    std::unique_ptr<ShutdownOperation> shutdownOp;
    std::atomic<bool> shutdownOutstanding{false};
};

void processReadBuffer(ClientSession &session, ThreadSafeQueue<Envelope> &queue)
{
    while (true)
    {
        // not enough header
        if (session.readBuffer.size() < sizeof(Header))
            return;

        Header hdr;
        std::memcpy(&hdr,
                    session.readBuffer.data(),
                    sizeof(Header));

        size_t frameSize = sizeof(Header) + hdr.length;

        std::cout << "we now know the frame size " << frameSize << std::endl;

        // not enough payload
        if (session.readBuffer.size() < frameSize)
            return;

        // extract frame - move to some higher level app buffer
        // for now its simple string so
        std::string payload(
            reinterpret_cast<char *>(session.readBuffer.data() + sizeof(Header)), hdr.length);

        // dispatch frame
        std::cout << "complete msg - " << payload << std::endl;

        Frame f;

        f.header = hdr;

        f.payload.len = hdr.length;
        f.payload.data = std::make_unique<uint8_t[]>(hdr.length);

        std::memcpy(f.payload.data.get(), session.readBuffer.data() + sizeof(Header), hdr.length);

        // -----------------------------
        Envelope env;
        env.frame = std::move(f);
        env.sessionId = session.id;
        std::cout << "envelope created with session id " << env.sessionId << " \n";

        queue.enqueue(std::move(env));

        // remove consumed bytes
        session.readBuffer.erase(
            session.readBuffer.begin(),
            session.readBuffer.begin() + frameSize);
    }
}

namespace transport
{
    void initialize(TransportState &state)
    {
        WSAData wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        SOCKET listener = WSASocket(AF_INET,
                                    SOCK_STREAM,
                                    IPPROTO_TCP,
                                    nullptr,
                                    0,
                                    WSA_FLAG_OVERLAPPED);

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(5347);
        addr.sin_addr.s_addr = INADDR_ANY;

        BOOL reuse = TRUE;

        setsockopt(listener,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   (const char *)&reuse,
                   sizeof(reuse));

        bind(listener, (sockaddr *)&addr, sizeof(addr));

        listen(listener, SOMAXCONN);

        std::cout << "listener socket " << listener << "\n";

        GUID guidAcceptEx = WSAID_ACCEPTEX;
        DWORD bytes = 0;

        LPFN_ACCEPTEX acceptExPtr = nullptr;

        WSAIoctl(listener,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guidAcceptEx,
                 sizeof(guidAcceptEx),
                 &acceptExPtr,
                 sizeof(acceptExPtr),
                 &bytes,
                 nullptr,
                 nullptr);

        state.resources = new EventResources;
        state.resources->listener = listener;
        state.resources->acceptExPtr = acceptExPtr;

        // see there is no completion key  with this listener as its the only one
        state.resources->iocp =
            CreateIoCompletionPort((HANDLE)listener,
                                   nullptr,
                                   0,
                                   0);

        // see acceptEx is just operation and requires overlapped and we can deduce
        SOCKET acceptSocket = WSASocket(AF_INET,
                                        SOCK_STREAM,
                                        IPPROTO_TCP,
                                        nullptr,
                                        0,
                                        WSA_FLAG_OVERLAPPED);

        //
        state.resources->acceptOp = std::make_unique<AcceptOperation>(std::move(acceptSocket));

        auto acceptOpAlias = state.resources->acceptOp.get();

        // check acceptex needs both the sockets and gqcs will return listener iocp completion key
        // and accept operation overlapped

        BOOL result = acceptExPtr(listener,
                                  acceptOpAlias->acceptSocket,
                                  acceptOpAlias->addressBuffer,
                                  0,
                                  sizeof(sockaddr_in) + 16,
                                  sizeof(sockaddr_in) + 16,
                                  &bytes,
                                  &acceptOpAlias->overlapped);

        if (!result)
        {
            DWORD err = WSAGetLastError();
            if (err != ERROR_IO_PENDING)
                std::cout << "Acceptex failed " << err << std::endl;
        }

        // wakeup init, when application side processes and submits
        state.notifyctx = new NotifyContext();

        state.notifyctx->notifySubmission = [&state]()
        {
            if (!state.resources->wakeupOutstanding.load())
            {
                state.resources->wakeupOp = std::make_unique<WakeUpOperation>();
                PostQueuedCompletionStatus(
                    state.resources->iocp,
                    0,
                    0,
                    &state.resources->wakeupOp->overlapped);
                std::cout << "wakeup posted \n";
            }
        };

        // shutdown init
        transport::shutdownHandle = CreateEvent(NULL,
                                                TRUE,
                                                FALSE,
                                                NULL);

        SetConsoleCtrlHandler(shutdownHandler, true);
    }

    void run(TransportState *state, ThreadSafeQueue<Envelope> &queue)
    {
        SessionId nextSessionId = 1;
        while (true)
        {
            DWORD bytesTransferred;
            ULONG_PTR key;
            LPOVERLAPPED lpOv;

            // rn we use key to identify accept sockets but if multiple listeners
            // then associate listeners keys too, with iocp

            // also we never explicitely connect iocp with any op(acceptex, wsarecv, wsasend) as iocp uses endpoint
            // but gqcs combines both

            // result == true  -> The operation finished, and it finished successfully.
            // result == false -> lpov == null -> Windows couldn't retrieve a completion packet.
            //                    lpov != null -> A completion packet was dequeued, but the operation itself failed.

            BOOL result = GetQueuedCompletionStatus(state->resources->iocp,
                                                    &bytesTransferred,
                                                    &key,
                                                    &lpOv,
                                                    INFINITE);

            if (result == false)
            {
                DWORD error = GetLastError();

                if (lpOv != nullptr)
                {
                    // An asynchronous I/O operation completed,
                    // but it completed with an error.

                    Operation *op = reinterpret_cast<Operation *>(lpOv);
                    std::cout << "Failed operation: " << static_cast<int>(op->type) << std::endl;
                    std::cout << "with error code " << error << std::endl;

                    continue;
                }
                else
                {
                    // GQCS itself failed.
                    // No completion packet was dequeued.

                    std::cerr << "Serious issue found, with error " << error << std::endl;
                    if (state->resources->iocp == nullptr ||
                        state->resources->iocp == INVALID_HANDLE_VALUE)
                    {
                        // Invalid IOCP handle
                        std::cerr << "Invalid IOCP handle, do not ignore" << std::endl;

                        // either shuddown or decide, rn returning but needs proper memory cleanup
                        return; // change later asap
                    }
                }
            }

            if (lpOv == nullptr)
            {
                std::cerr << "gqcs result " << result << " but lpov is null too, do not ignore" << std::endl;
                continue;
            }

            Operation *op = reinterpret_cast<Operation *>(lpOv);
            std::cout << "Operation type " << static_cast<int>(op->type) << std::endl;
            std::cout << "Offset " << offsetof(Operation, overlapped) << std::endl;

            switch (op->type)
            {
            case OperationType::Accept:
            {
                std::cout << "Accept opertion\n";
                AcceptOperation *aop = (AcceptOperation *)op;
                std::cout << "New client arrived at handle " << aop->acceptSocket << std::endl;

                // 1. update accept socket context, windows needs this to update its own context
                // see we pass listener in sense of context like kind

                setsockopt(
                    aop->acceptSocket,
                    SOL_SOCKET,
                    SO_UPDATE_ACCEPT_CONTEXT,
                    (char *)&state->resources->listener,
                    sizeof(SOCKET));

                std::cout << "setsockopt in accept operation " << WSAGetLastError() << std::endl;

                // 2. Create client session
                auto session = std::make_unique<ClientSession>(nextSessionId);

                ClientSession *cs = session.get();
                ClientContext *cctx = new ClientContext{std::move(aop->acceptSocket), cs};

                // cs-> context is void pointer implementation
                cs->context = cctx;

                state->sessions.emplace(nextSessionId++, std::move(session));

                // 3. Associate accept socket with iocp to request completion packets
                // see we provide old iocp as extension to this
                // check the completion key
                CreateIoCompletionPort((HANDLE)cctx->socket,
                                       state->resources->iocp,
                                       (ULONG_PTR)cctx,
                                       0);

                // 4. Post first recv
                cctx->recvOp = std::make_unique<RecvOperation>();

                DWORD flag = 0;
                BOOL recv = WSARecv(cctx->socket,
                                    &cctx->recvOp->buffer,
                                    1,
                                    nullptr,
                                    &flag,
                                    &cctx->recvOp->overlapped,
                                    nullptr);

                if (!recv)
                {
                    DWORD err = WSAGetLastError();
                    if (err != ERROR_IO_PENDING)
                        std::cout << "Inital recv failed " << err << std::endl;
                }

                // 5. repost AcceptEx (IMPORTANT)
                // aop->acceptSocket is already std moved to cctx
                aop->acceptSocket =
                    WSASocket(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);

                BOOL accept = state->resources->acceptExPtr(state->resources->listener,
                                                            aop->acceptSocket,
                                                            aop->addressBuffer,
                                                            0,
                                                            sizeof(sockaddr_in) + 16,
                                                            sizeof(sockaddr_in) + 16,
                                                            0,
                                                            &aop->overlapped);

                if (!accept)
                {
                    DWORD err = WSAGetLastError();
                    if (err != ERROR_IO_PENDING)
                        std::cout << "Repost Acceptex failed " << err << std::endl;
                }

                break;
            }

            case OperationType::Recv:
            {
                std::cout << "Recv Operation\n";
                RecvOperation *rop = (RecvOperation *)op;

                std::cout << "num of bytes received " << bytesTransferred << std::endl;

                // remember cctx is associated with completion key
                ClientContext *cctx = reinterpret_cast<ClientContext *>(key);

                // client disconneted
                if (bytesTransferred == 0)
                {
                    std::cout << "Client with session id " << cctx->session->id << " is gone ";

                    closesocket(cctx->socket);
                    state->sessions.erase(cctx->session->id);
                    cctx->session = nullptr;
                    delete cctx;
                    std::cout << "connected clients = " << state->sessions.size() << std::endl;
                    break;
                }

                cctx->session->readBuffer.insert(cctx->session->readBuffer.end(),
                                                 rop->storage,
                                                 rop->storage + bytesTransferred);

                processReadBuffer(*cctx->session, queue);

                DWORD flag = 0;
                DWORD result = WSARecv(cctx->socket,
                                       &cctx->recvOp->buffer,
                                       1,
                                       nullptr,
                                       &flag,
                                       &cctx->recvOp->overlapped,
                                       nullptr);

                if (!result)
                {
                    DWORD err = WSAGetLastError();
                    if (err != ERROR_IO_PENDING)
                        std::cout << "Reusing Recv failed " << err << std::endl;
                }

                break;
            }

            case OperationType::Send:
            {
                std::cout << "Send operation\n";
                std::cout << "bytes transferred " << bytesTransferred << std::endl;

                SendOperation *sop = (SendOperation *)op;
                sop->pendingSend->bytesSent += bytesTransferred;

                auto *cctx = reinterpret_cast<ClientContext *>(key);

                if (sop->pendingSend->isComplete())
                {
                    // cleanup here
                    cctx->session->sendActive = false;
                    cctx->session->sendQueue.pop_front();
                    cctx->sendOp.reset();
                }
                else
                {
                    std::cout << "pending bytes " << sop->pendingSend->totalSize() - sop->pendingSend->bytesSent << std::endl;
                    sop->prepareBuffers();
                    DWORD result = WSASend(cctx->socket,
                                           sop->buffs,
                                           sop->buffsCount,
                                           0,
                                           0,
                                           &sop->overlapped,
                                           nullptr);

                    if (!result)
                    {
                        DWORD err = WSAGetLastError();
                        if (err != ERROR_IO_PENDING)
                            std::cout << "Send remaining failed " << err << std::endl;
                    }
                }

                break;
            }

            case OperationType::Wakeup:
            {
                std::cout << "Wakeup operation\n";

                while (!state->submissionQueue.empty())
                {
                    std::cout << "enters while loop\n";
                    Envelope env = std::move(state->submissionQueue.front());

                    ClientSession *cs = state->sessions.at(env.sessionId).get();

                    cs->sendQueue.emplace_back(std::move(env.frame));
                    state->submissionQueue.pop_back();

                    if (!cs->sendActive)
                    {
                        PendingSend &sendTask = cs->sendQueue.front();
                        cs->sendActive = true;

                        // prepare send
                        auto *cctx = static_cast<ClientContext *>(cs->context);
                        cctx->sendOp = std::make_unique<SendOperation>(&sendTask);

                        DWORD flag = 0;
                        DWORD bytesSent = 0;
                        DWORD result = WSASend(cctx->socket,
                                               cctx->sendOp->buffs,
                                               2,
                                               &bytesSent,
                                               flag,
                                               &cctx->sendOp->overlapped,
                                               nullptr);

                        if (!result)
                        {
                            DWORD err = WSAGetLastError();
                            if (err != ERROR_IO_PENDING)
                                std::cout << "Wakeup send failed " << err << std::endl;
                        }

                        std::cout << "payload len = " << sendTask.frame.payload.len << std::endl;
                    }
                }
                state->resources->wakeupOutstanding.store(false);
                state->resources->wakeupOp.reset();
                break;
            }

            case OperationType::Shutdown:
            {
                std::cout << "shutdown event called\n";

                // step 1 stop listening
                closesocket(state->resources->listener);
                state->resources->acceptExPtr = nullptr;

                std::cout << "step 1 complete \n";

                // step 2 close connected clients
                for (auto &[id, session] : state->sessions)
                {
                    closesocket(static_cast<ClientContext *>(session->context)->socket);
                    delete static_cast<ClientContext *>(session->context);
                    session->context = nullptr;
                }
                state->sessions.clear();
                std::cout << "step 2 complete \n";

                // step 3 close rest
                CloseHandle(state->resources->iocp);
                CloseHandle(transport::shutdownHandle);
                state->resources->shutdownOp.reset();
                delete state->resources;
                state->resources = nullptr;
                std::cout << "step 3 complete \n";

                return;
            }
            }
        }
    }

    void awaitShutdown(TransportState &state)
    {
        // Block until Ctrl+C
        WaitForSingleObject(transport::shutdownHandle, INFINITE);

        if (!state.resources->shutdownOutstanding.load())
        {
            state.resources->shutdownOp = std::make_unique<ShutdownOperation>();

            // post shutdown
            PostQueuedCompletionStatus(
                state.resources->iocp,
                0,
                0,
                &state.resources->shutdownOp->overlapped);

            state.resources->shutdownOutstanding.store(true);
            std::cout << "shutdown posted \n";
        }
    }

}
