// Audio-only simulator harness: the production adapter and DSP, with its
// default-off settings mailbox in place of the settings UI and preferences.
#include "../../tweak/Sources/Shared/QuickEQ/QuickEQInternal.hpp"
sgquickeq::Mailbox &SGQuickEQConfigurationMailbox() { static sgquickeq::Mailbox box; return box; }
extern "C" void SGQuickEQInitializeSettings() {
    SGQuickEQConfigurationMailbox().publish({});
}
