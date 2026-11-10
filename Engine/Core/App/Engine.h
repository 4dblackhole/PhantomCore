#pragma once

#include "Client/IGameClient.h"

#include <memory>

// Takes exclusive ownership of the Client and runs the complete engine
// lifetime. See Docs/EngineOverview.md for the engine call order.
int Run(std::unique_ptr<IGameClient> client);
