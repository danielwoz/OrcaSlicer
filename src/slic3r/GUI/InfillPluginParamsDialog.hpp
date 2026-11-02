#pragma once

#include "GUI_Utils.hpp"

#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {

class ConfigBase;
struct InfillPluginParam;
struct InfillPluginPattern;

namespace GUI {

// Edits the parameters of infill plugin patterns with controls built from the parameters the plugins declare: one box
// per pattern that has parameters, the patterns that config selects first.
class InfillPluginParamsDialog : public DPIDialog
{
public:
    // text is the value of infill_plugin_params; config, which may be nullptr, holds the selected patterns.
    InfillPluginParamsDialog(wxWindow* parent, const std::string& text, const ConfigBase* config);

    // The edited text: the entries of the patterns whose values changed rewritten in canonical form, every other entry
    // as given.
    const std::string& text() const { return m_text; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // The control of a parameter: ::CheckBox for BOOL, ::ComboBox for ENUM, ::TextInput otherwise.
    struct Control
    {
        const InfillPluginParam* param;
        wxWindow*                window;
        // The value last set; a ::TextInput that still shows it reads it back with every digit.
        double                   value { 0. };
    };
    struct PatternControls
    {
        const InfillPluginPattern* pattern;
        std::vector<Control>       controls;
    };

    wxWindow* create_control(wxWindow* parent, const InfillPluginParam& param);
    void      set_control_value(Control& control, double value);
    // Reads the controls into m_text; false, with a message, when an input is not a valid value.
    bool      accept();

    std::string                  m_text;
    wxWindow*                    m_list { nullptr }; // scrolled panel holding the pattern boxes and their controls
    std::vector<PatternControls> m_patterns;
};

}} // namespace Slic3r::GUI
