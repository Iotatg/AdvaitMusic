#pragma once

namespace advait {
namespace tg {
class TdClient;
}
namespace modules {
// Welcome system + notes + filters (Aishamusic-2 group_moderation.go parity).
void init_moderation(tg::TdClient& bot);
}
}  // namespace advait
