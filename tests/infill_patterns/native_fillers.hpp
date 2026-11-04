#pragma once

#include <memory>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::InfillCorpus {

// A new filler of the row's Fill class itself, also for a row a pattern library serves: the native reference the
// libraries are compared with. nullptr for a row this build leaves out.
std::unique_ptr<Fill> native_filler(InfillPattern pattern);

} // namespace Slic3r::InfillCorpus
