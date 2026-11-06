#pragma once

// Included ahead of every source of libslic3r_fillkit and of the infill pattern objects, which the built-in pattern
// libraries link as well as libslic3r. BOOST_LOG_TRIVIAL writes through kit_log_enabled() and kit_log_write(): libslic3r
// defines them on Boost.Log (KitLogHost.cpp), a pattern library on the log of its host (ModuleSeams.cpp), so that a
// library carries no logging core of its own.

#ifdef __cplusplus

#include <optional>
#include <sstream>

// Declares the severities and defines BOOST_LOG_TRIVIAL once, so that a later include leaves the definition below.
#include <boost/log/trivial.hpp>

namespace Slic3r {

// Whether a line of this boost::log::trivial::severity_level would be written.
bool kit_log_enabled(int severity);
// Writes one line.
void kit_log_write(int severity, const char *message);

// The line of one BOOST_LOG_TRIVIAL statement, formatted only when it is written.
class KitLogLine
{
public:
    explicit KitLogLine(int severity) : m_severity(severity), m_open(kit_log_enabled(severity))
    {
        if (m_open)
            m_stream.emplace();
    }
    bool          open() const { return m_open; }
    std::ostream& stream() { return *m_stream; }
    void          close()
    {
        kit_log_write(m_severity, m_stream->str().c_str());
        m_open = false;
    }

private:
    int                                m_severity;
    bool                               m_open;
    std::optional<std::ostringstream>  m_stream;
};

} // namespace Slic3r

#undef BOOST_LOG_TRIVIAL
#define BOOST_LOG_TRIVIAL(lvl)                                                                                    \
    for (::Slic3r::KitLogLine kit_log_line_(static_cast<int>(::boost::log::trivial::lvl)); kit_log_line_.open();   \
         kit_log_line_.close())                                                                                     \
        kit_log_line_.stream()

#endif // __cplusplus
