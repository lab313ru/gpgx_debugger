#include "save_state.hpp"

namespace genesis {

static message_box_fn& get_callback() {
    static message_box_fn cb;
    return cb;
}

void set_save_state_message_box(message_box_fn fn) {
    get_callback() = std::move(fn);
}

message_box_fn& SaveState::get_save_state_message_box() {
    return get_callback();
}

} // namespace genesis
