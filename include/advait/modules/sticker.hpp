#pragma once

namespace advait {
namespace tg {
class TdClient;
}
namespace modules {
// Sticker reply engine (sticker -> sticker) + owner curation commands.
void init_sticker(tg::TdClient& bot);
}
}  // namespace advait
