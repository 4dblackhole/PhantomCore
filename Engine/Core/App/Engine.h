#pragma once

#include "Client/IGameClient.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Takes exclusive ownership of the Client and runs the complete engine
// lifetime. See Docs/EngineOverview.md for the engine call order.
int Run(std::unique_ptr<IGameClient> client);

// Error and corruption messages from one engine run, when the graphics
// backend's diagnostic facility is available.
struct GraphicsDiagnostics
{
    enum class Status : std::uint8_t
    {
        Unavailable,
        Collected,
        ReadFailed,
    };
    enum class Severity : std::uint8_t
    {
        Error,
        Corruption,
    };
    struct Message
    {
        Severity severity{};
        std::string description;
    };

    Status status{};
    std::vector<Message> messages;
};

// Fills diagnostics after Client and engine resource cleanup. The messages
// own their descriptions and remain usable after Run returns.
int RunWithGraphicsDiagnostics(std::unique_ptr<IGameClient> client, GraphicsDiagnostics& diagnostics);
