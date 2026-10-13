#pragma once

#include <string>

namespace advait {

class RoomState;

namespace chat_state {

// KanhaMusic makeVoiceChatReady parity: makes sure the assistant assigned to the
// room is a member of the room's channel, auto-joining via public username or a
// fresh invite link (with join-request approval) when needed. Returns "" on
// success, otherwise a ready-to-display locale error formatted for
// notify_chat_id (the user-facing chat, which may differ from r->id for
// channel-linked playback).
std::string ensure_voice_chat_ready(RoomState* r, int64_t notify_chat_id);

}  // namespace chat_state
}  // namespace advait
