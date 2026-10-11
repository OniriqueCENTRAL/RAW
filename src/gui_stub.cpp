// RAW - plateformes sans interface (Linux pour l'instant) : le DAW affiche ses curseurs génériques
#include "gui.h"
#include <memory>
#if !defined(_WIN32) && !defined(__APPLE__)
namespace raw {
std::unique_ptr<WebUi> WebUi::create() { return nullptr; }
}
#endif
