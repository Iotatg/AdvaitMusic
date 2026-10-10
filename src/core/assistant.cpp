#include "advait/core/assistant.hpp"
#include "advait/database/database.hpp"
#include "advait/ubot/context.hpp"

namespace advait {

AssistantManager* g_assistants = nullptr;

Assistant* AssistantManager::get(int idx) {
    if (idx < 1 || idx > static_cast<int>(list_.size())) return nullptr;
    return list_[static_cast<size_t>(idx - 1)].get();
}

Assistant* AssistantManager::first() { return get(1); }

Assistant* AssistantManager::for_chat(int64_t chat_id) {
    if (list_.empty()) return nullptr;
    int idx = db::get_assistant(chat_id);
    auto* a = get(idx);
    return a ? a : first();
}

void AssistantManager::for_each(const std::function<void(Assistant*)>& fn) {
    for (auto& a : list_) fn(a.get());
}

void AssistantManager::add(std::unique_ptr<Assistant> a) { list_.push_back(std::move(a)); }

}  // namespace advait
