#pragma once

namespace advait {
namespace tg {
class TdClient;
}
namespace modules {
// AI chatbot: DMs always, groups only when addressed.
void init_chatbot(tg::TdClient& bot);
}
}  // namespace advait
