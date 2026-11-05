#include "../transportState.h"
#include "../clientSession.h"
#include "../notifyContext.h"

#include "../../runtime/threadSafeQueue.h"
#include "../../application/pendingSend.h"
#include "../../application/envelope.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <cstring>

#include <iostream>

// events
enum class EventSourceType
{
    SubmissionEvent,
    ListenerEvent,
    ClientEvent,
    ShutdownEvent
};

// contexts
struct EventContext
{
    EventSourceType type;
    int fd;
    EventContext(EventSourceType type, int fd) : type(type), fd(fd) {}
};

struct SubmissionContext : EventContext
{
    SubmissionContext(int fd)
        : EventContext(EventSourceType::SubmissionEvent, fd) {}
};

struct ListenerContext : EventContext
{
    int port;
    ListenerContext(int fd, int port)
        : EventContext(EventSourceType::ListenerEvent, fd), port(port) {}
};

struct ShutdownContext : EventContext
{
    ShutdownContext(int fd)
        : EventContext(EventSourceType::ShutdownEvent, fd) {}
};

struct ClientContext : EventContext
{
    SessionId sessionId;
    ClientSession *session;
    ClientContext(int fd, SessionId id, ClientSession *cs)
        : EventContext(EventSourceType::ClientEvent, fd), sessionId(id), session(cs) {}
};

struct EventResources
{
    int epoll;

    std::unique_ptr<SubmissionContext> submission;
    std::unique_ptr<ListenerContext> listener;
    std::unique_ptr<ShutdownContext> shutdown;
};

// functions
void disconnectClient(TransportState *state, ClientContext *cctx)
{
    epoll_ctl(state->resources->epoll, EPOLL_CTL_DEL, cctx->fd, nullptr);
    close(cctx->fd);
    state->sessions.erase(cctx->sessionId);
    delete cctx;
}

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
        state.resources = new EventResources;
        state.resources->epoll = epoll_create1(0);

        // ----------------------- Listener Event Slice -------------------------

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);

        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        std::cout << "server fd is " << server_fd << "\n";

        int opt = 1;
        setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt));

        int port = 5347;
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        bind(server_fd,
             reinterpret_cast<sockaddr *>(&server_addr),
             sizeof(server_addr));

        listen(server_fd, SOMAXCONN);

        state.resources->listener = std::make_unique<ListenerContext>(server_fd, port);

        epoll_event evl{};
        evl.events = EPOLLIN;
        evl.data.ptr =
            state.resources->listener.get();

        epoll_ctl(state.resources->epoll, EPOLL_CTL_ADD, server_fd, &evl);

        std::cout << "error in listener - " << errno << "\n";

        // -------------------------------------------------------------------------

        // ----------------------- Submission Event Slice -------------------------

        int submissionfd = eventfd(0, EFD_NONBLOCK);
        std::cout << "submission fd is " << submissionfd << "\n";

        state.resources->submission =
            std::make_unique<SubmissionContext>(submissionfd);

        epoll_event evsub{};
        evsub.events = EPOLLIN;
        evsub.data.ptr =
            state.resources->submission.get();

        epoll_ctl(state.resources->epoll,
                  EPOLL_CTL_ADD,
                  submissionfd,
                  &evsub);

        std::cout << "error in submission - " << errno << "\n";

        // -------------------------------------------------------------------------

        // ----------------------- Submission Event Slice -------------------------

        // wakeup init, when application side processes and submits
        state.notifyctx = new NotifyContext();

        state.notifyctx->notifySubmission = [&state]()
        {
            uint64_t value = 1;
            write(state.resources->submission.get()->fd, &value, sizeof(value));
        };

        // -------------------------------------------------------------------------

        // ----------------------- Shutdown Event Slice -------------------------

        int shutdownfd = eventfd(0, 0);
        state.resources->shutdown =
            std::make_unique<ShutdownContext>(shutdownfd);

        epoll_event evshut{};
        evshut.events = EPOLLIN;
        evshut.data.ptr =
            state.resources->shutdown.get();

        epoll_ctl(state.resources->epoll, EPOLL_CTL_ADD, shutdownfd, &evshut);
    }

    void run(TransportState *state, ThreadSafeQueue<Envelope> &queue)
    {
        epoll_event ev[64];
        SessionId nextSessionId = 1;

        while (true)
        {
            int n = epoll_wait(state->resources->epoll, ev, 64, -1);
            std::cout << "epoll woke, no of events " << n << std::endl;

            for (int i = 0; i < n; i++)
            {
                EventContext *ctx =
                    reinterpret_cast<EventContext *>(ev[i].data.ptr);

                std::cout << "ctx ptr = " << ctx << "\n";
                std::cout << "ctx type = " << static_cast<int>(ctx->type) << "\n";
                std::cout << "ctx fd = " << ctx->fd << "\n";

                switch (ctx->type)
                {
                case EventSourceType::ListenerEvent:
                {
                    std::cout << "listener event\n";
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);

                    int client_fd = accept(ctx->fd,
                                           reinterpret_cast<sockaddr *>(&client_addr),
                                           &client_len);

                    if (client_fd == -1)
                    {
                        std::cout << "error code during accept is " << errno;
                        break;
                    }

                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    std::cout << "Client accepted with fd " << client_fd << std::endl;

                    auto session =
                        std::make_unique<ClientSession>(nextSessionId);

                    ClientSession *cs = session.get();

                    state->sessions.emplace(nextSessionId, std::move(session));

                    ClientContext *cctx = new ClientContext(client_fd, nextSessionId++, cs);
                    cs->context = cctx;

                    epoll_event evc{};
                    evc.events = EPOLLIN;
                    evc.data.ptr = cctx;

                    epoll_ctl(state->resources->epoll, EPOLL_CTL_ADD, client_fd, &evc);

                    std::cout << "Client registered with epoll\n";
                    break;
                }

                case EventSourceType::ClientEvent:
                {
                    std::cout << "client event occured \n";
                    ClientContext *cctx = reinterpret_cast<ClientContext *>(ctx);

                    size_t totalBytesReceived = 0;

                    if (ev[i].events & (EPOLLERR | EPOLLHUP))
                    {
                        disconnectClient(state, cctx);
                        continue;
                    }

                    if (ev[i].events & (EPOLLIN | EPOLLRDHUP))
                    {
                        char buffer[4096];

                        while (true)
                        {
                            ssize_t bytes_read = recv(ctx->fd, buffer, sizeof(buffer), 0);

                            totalBytesReceived += bytes_read;

                            std::cout
                                << "recv " << bytes_read << " bytes, total=" << totalBytesReceived << "\n";

                            if (bytes_read > 0)
                            {
                                // append to read buffer
                                cctx->session->readBuffer
                                    .insert(cctx->session->readBuffer.end(), buffer, buffer + bytes_read);
                            }
                            else if (bytes_read == 0)
                            {
                                // orderly shutdown
                                // disconnect client
                                disconnectClient(state, cctx);
                                break;
                            }
                            else
                            {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                {
                                    // socket drained
                                    break;
                                }

                                // actual recv error
                                // disconnect client
                                disconnectClient(state, cctx);
                                break;
                            }
                        }
                        // call parser
                        processReadBuffer(*cctx->session, queue);
                    }

                    if (ev[i].events & EPOLLOUT)
                    {
                        while (!cctx->session->sendQueue.empty())
                        {
                            PendingSend &sendTask = cctx->session->sendQueue.front();

                            ssize_t bytesTransfered = 0;

                            ssize_t remainingBytes = sendTask.frame.header.length + sizeof(Header) - sendTask.bytesSent;
                            std::cout << "bytes sent already, remaining bytes " << sendTask.bytesSent << " " << remainingBytes << "\n";

                            constexpr size_t TEST_CHUNK = 5;

                            if (sendTask.bytesSent < sizeof(Header))
                            {
                                iovec vecs[2];

                                size_t headerRemaining =
                                    sizeof(Header) - sendTask.bytesSent;

                                vecs[0].iov_base =
                                    reinterpret_cast<uint8_t *>(&sendTask.frame.header) + sendTask.bytesSent;

                                vecs[0].iov_len = headerRemaining;

                                vecs[1].iov_base =
                                    sendTask.frame.payload.data.get();

                                vecs[1].iov_len =
                                    sendTask.frame.payload.len;

                                // limit visible bytes
                                if (vecs[0].iov_len >= TEST_CHUNK)
                                {
                                    vecs[0].iov_len = TEST_CHUNK;
                                    vecs[1].iov_len = 0;
                                }
                                else
                                {
                                    vecs[1].iov_len =
                                        std::min(
                                            vecs[1].iov_len,
                                            TEST_CHUNK - vecs[0].iov_len);
                                }

                                bytesTransfered =
                                    writev(ctx->fd, vecs, 2);
                            }
                            else
                            {
                                size_t payloadOffset =
                                    sendTask.bytesSent - sizeof(Header);

                                iovec vec[1];

                                vec[0].iov_base =
                                    sendTask.frame.payload.data.get() + payloadOffset;

                                vec[0].iov_len =
                                    std::min(
                                        sendTask.frame.payload.len - payloadOffset,
                                        TEST_CHUNK);

                                bytesTransfered =
                                    writev(ctx->fd, vec, 1);
                            }

                            if (bytesTransfered > 0 && bytesTransfered == remainingBytes)
                            {
                                cctx->session->sendQueue.pop_front();
                            }
                            else if (bytesTransfered > 0 && bytesTransfered < remainingBytes)
                            {
                                sendTask.bytesSent += bytesTransfered;
                            }
                            else if (bytesTransfered == -1)
                            {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                {
                                    break;
                                }

                                disconnectClient(state, cctx);
                                break;
                            }
                            else if (bytesTransfered == 0)
                            {
                                std::cout << "writev returned 0 - something abnormal\n";
                                disconnectClient(state, cctx);
                                break;
                            }
                        }

                        if (cctx->session->sendQueue.empty())
                        {
                            cctx->session->sendActive = false;

                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLRDHUP;
                            ev.data.ptr = cctx;

                            epoll_ctl(state->resources->epoll, EPOLL_CTL_MOD, ctx->fd, &ev);
                        }
                    }
                    break;
                }

                case EventSourceType::SubmissionEvent:
                {
                    // submission event role is to prepare the object and make client session write ready
                    std::cout << "Submission Event\n";
                    uint64_t buff;

                    read(ctx->fd, &buff, sizeof(buff));

                    while (!state->submissionQueue.empty())
                    {
                        std::cout << "enters while loop\n";
                        Envelope env = std::move(state->submissionQueue.front());

                        ClientSession *cs = state->sessions.at(env.sessionId).get();

                        cs->sendQueue.emplace_back(std::move(env.frame));
                        state->submissionQueue.pop_back();

                        if (!cs->sendActive)
                        {
                            cs->sendActive = true;

                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLRDHUP | EPOLLOUT;
                            ev.data.ptr = cs->context;

                            epoll_ctl(state->resources->epoll, EPOLL_CTL_MOD, reinterpret_cast<ClientContext *>(cs->context)->fd, &ev);
                        }

                        std::cout << "epoll error in bw " << errno << "\n";
                    }

                    std::cout << "exit while loop\n";
                    break;
                }

                case EventSourceType::ShutdownEvent:
                {
                    std::cout << "shutdown event occured\n";
                    // step 1 stop listening
                    close(state->resources->listener.get()->fd);

                    // step 2 close connected clients
                    for (auto &[id, session] : state->sessions)
                    {
                        close(reinterpret_cast<ClientContext *>(session->context)->fd);
                        delete static_cast<ClientContext *>(session->context);
                        session->context = nullptr;
                    }
                    state->sessions.clear();

                    // step 3 close rest fds
                    close(state->resources->submission.get()->fd);
                    close(state->resources->submission.get()->fd);

                    // step 4 close epoll fd
                    close(state->resources->epoll);

                    delete state->resources;
                    state->resources = nullptr;
                    return;
                }
                }
            }
        }
    }

    void awaitShutdown(TransportState &state)
    {
        sigset_t set;
        int sig;

        // Step 1 Build the set
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        // sigaddset(&set, SIGTERM); <- can add like this

        // Step 2 Block signals
        sigprocmask(SIG_BLOCK, &set, NULL);
        std::cout << "Shutdown sequence armed!\n";

        // Step 3 Block thread and wait
        sigwait(&set, &sig);
        std::cout << "Shutdown requested.\n";

        // Step 4 Wake up epoll wait
        uint64_t value = 1;
        write(state.resources->shutdown.get()->fd, &value, sizeof(value));
    }
}