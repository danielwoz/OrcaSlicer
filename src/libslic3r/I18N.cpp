#include "I18N.hpp"

namespace Slic3r {

// Translate function callback, to call wxWidgets translate function to convert non-localized UTF8 string to a localized
// one. A built-in infill pattern library has a copy of its own that is never set, so its messages are not translated.
I18N::translate_fn_type I18N::translate_fn = nullptr;

} // namespace Slic3r
