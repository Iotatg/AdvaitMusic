#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace advait {

namespace ntg {
class Client;
}
namespace ubot {
class Context;
}

struct Assistant {
    int index{0};
    int64_t user_id{0};
    std::string username;
    std::string first_name;
    std::unique_ptr<ubot::Context> ntg;
};

class AssistantManager {
public:
    int count() const { return static_cast<int>(list_.size()); }
    Assistant* get(int idx);
    Assistant* first();
    Assistant* for_chat(int64_t chat_id);
    void for_each(const std::function<void(Assistant*)>& fn);
    void add(std::unique_ptr<Assistant> a);

private:
    std::vector<std::unique_ptr<Assistant>> list_;
};

extern AssistantManager* g_assistants;

}  // namespace advait
