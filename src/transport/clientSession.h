#pragma once
#include "../application/pendingSend.h"

#include <vector>
#include <deque>
#include <chrono>

using SessionId = uint32_t;

enum class ClientType
{
    Unknown,
    Chat,
    Telemetry,
    File
};

enum class SessionState
{
    Handshake,
    Connected,
    Active,
    Closed
};

class ClientSession
{
public:
    SessionId id;
    ClientType type;
    SessionState state;

    // receive side
    std::vector<uint8_t> readBuffer;

    // send size
    std::deque<PendingSend> sendQueue;

    bool sendActive = false;

    std::chrono::system_clock::time_point createdAt;
    std::chrono::steady_clock::time_point lastActivity;

    int errorCount;

    // special pointer to hook some state
    void *context = nullptr;

    ClientSession(SessionId id) : id(id),
                                  createdAt(std::chrono::system_clock::now()),
                                  lastActivity(std::chrono::steady_clock::now()),
                                  errorCount(0),
                                  state(SessionState::Handshake),
                                  type(ClientType::Unknown) {};
};