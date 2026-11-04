// The log of the sources that libslic3r shares with the built-in pattern libraries (Fill/Library/KitLog.hpp), on the
// application's Boost.Log core.
#include "KitLog.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

bool kit_log_enabled(int severity)
{
    return bool(::boost::log::trivial::logger::get().open_record(
        ::boost::log::keywords::severity = ::boost::log::trivial::severity_level(severity)));
}

void kit_log_write(int severity, const char *message)
{
    BOOST_LOG_STREAM_WITH_PARAMS(::boost::log::trivial::logger::get(),
                                 (::boost::log::keywords::severity = ::boost::log::trivial::severity_level(severity)))
        << message;
}

} // namespace Slic3r
