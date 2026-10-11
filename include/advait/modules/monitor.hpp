#pragma once

#include "advait/core/assistant.hpp"
#include "advait/core/room.hpp"
#include "advait/core/models.hpp"

#include <optional>

namespace advait::modules {

// Go: modules/monitor.go + call.go stream-end continuation.
void start_room_monitor();
void wire_stream_end(AssistantManager* assistants);
// Plays the next queued/autoplay track after the current stream finished;
// safe to call from a detached thread.
void advance_room(int64_t chat_id);

}  // namespace advait::modules
