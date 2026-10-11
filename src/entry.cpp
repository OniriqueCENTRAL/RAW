// RAW - point d'entrée CLAP
#include <clap/clap.h>

bool raw_entry_init(const char* path);
void raw_entry_deinit();
const void* raw_entry_get_factory(const char* id);

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    raw_entry_init,
    raw_entry_deinit,
    raw_entry_get_factory,
};
}
