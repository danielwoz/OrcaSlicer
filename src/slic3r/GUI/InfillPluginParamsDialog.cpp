#include "InfillPluginParamsDialog.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/Fill/FillPlugin.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Fill/InfillPlugins.hpp"
#include "libslic3r/LocalesUtils.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int control_width = 120;

// A value of a FLOAT or INT parameter as its input shows it.
wxString number_text(const InfillPluginParam& param, double value)
{
    return param.type == ORCA_INFILL_PARAM_INT ? wxString::Format("%d", int(value)) : from_u8(float_to_string_decimal_point(value));
}

wxString range_text(const InfillPluginParam& param)
{
    return wxString::Format("[%s, %s]", number_text(param, param.min_value), number_text(param, param.max_value));
}

} // namespace

InfillPluginParamsDialog::InfillPluginParamsDialog(wxWindow* parent, const std::string& text, const ConfigBase* config)
    : DPIDialog(parent, wxID_ANY, _L("Pattern parameters"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_text(text)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    std::set<InfillPattern> selected;
    if (config != nullptr)
        for (const auto& option : InfillPatternRegistry::options())
            if (const ConfigOption* opt = config->option(option.first); opt != nullptr)
                selected.insert(InfillPattern(opt->getInt()));
    std::vector<const InfillPluginPattern*> patterns = infill_plugin_patterns();
    patterns.erase(std::remove_if(patterns.begin(), patterns.end(), [](const InfillPluginPattern* p) { return p->params.empty(); }),
                   patterns.end());
    std::stable_partition(patterns.begin(), patterns.end(), [&selected](const InfillPluginPattern* p) { return selected.count(p->id) > 0; });

    auto list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    list->SetBackgroundColour(*wxWHITE);
    m_list = list;
    auto list_sizer = new wxBoxSizer(wxVERTICAL);
    for (const InfillPluginPattern* pattern : patterns) {
        auto box = new LabeledStaticBox(list, from_u8(pattern->label));
        box->SetToolTip(from_u8(pattern->key + "\n" + pattern->plugin));
        auto box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto grid      = new wxFlexGridSizer(2, FromDIP(8), FromDIP(20));
        grid->AddGrowableCol(0);

        PatternControls           pattern_controls{pattern, {}};
        const std::vector<double> values = infill_plugin_param_values(pattern->id, text);
        for (size_t i = 0; i < pattern->params.size(); ++i) {
            const InfillPluginParam& param   = pattern->params[i];
            auto                     label   = new wxStaticText(list, wxID_ANY, from_u8(param.label));
            Control                  control = {&param, create_control(list, param)};
            set_control_value(control, values[i]);
            wxString tooltip = from_u8(param.tooltip);
            if (param.type == ORCA_INFILL_PARAM_FLOAT || param.type == ORCA_INFILL_PARAM_INT)
                tooltip += (tooltip.empty() ? "" : "\n") + range_text(param);
            label->SetToolTip(tooltip);
            control.window->SetToolTip(tooltip);
            grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(control.window, 0, wxALIGN_CENTER_VERTICAL);
            pattern_controls.controls.push_back(control);
        }
        box_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(10));

        auto reset = new Button(list, _L("Reset"));
        reset->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        reset->SetToolTip(_L("Restore defaults"));
        reset->Bind(wxEVT_BUTTON, [this, index = m_patterns.size()](wxCommandEvent&) {
            for (Control& control : m_patterns[index].controls)
                set_control_value(control, control.param->default_value);
        });
        box_sizer->Add(reset, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(10));
        list_sizer->Add(box_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        m_patterns.push_back(std::move(pattern_controls));
    }
    if (m_patterns.empty())
        list_sizer->Add(new wxStaticText(list, wxID_ANY, _L("The infill patterns of the installed plugins have no parameters.")), 0,
                        wxALL, FromDIP(10));
    list->SetSizer(list_sizer);
    list->SetScrollRate(0, FromDIP(10));
    // Scrolls when the patterns do not fit.
    wxSize list_size = list_sizer->CalcMin();
    if (list_size.y > FromDIP(560)) {
        list_size.y = FromDIP(560);
        list_size.x += wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, list);
    }
    list->SetMinSize(list_size);

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(list, 1, wxEXPAND | wxBOTTOM, FromDIP(10));
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (accept())
            EndModal(wxID_OK);
    });
    sizer->Add(dlg_btns, 0, wxEXPAND);
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

wxWindow* InfillPluginParamsDialog::create_control(wxWindow* parent, const InfillPluginParam& param)
{
    const wxSize size = FromDIP(wxSize(control_width, -1));
    switch (param.type) {
    case ORCA_INFILL_PARAM_BOOL: return new ::CheckBox(parent);
    case ORCA_INFILL_PARAM_ENUM: {
        auto combo = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size, 0, nullptr, wxCB_READONLY);
        for (const std::string& label : param.enum_labels)
            combo->Append(from_u8(label));
        return combo;
    }
    default: return new ::TextInput(parent, wxEmptyString, from_u8(param.unit), "", wxDefaultPosition, size);
    }
}

void InfillPluginParamsDialog::set_control_value(Control& control, double value)
{
    control.value = value;
    switch (control.param->type) {
    case ORCA_INFILL_PARAM_BOOL: static_cast<::CheckBox*>(control.window)->SetValue(value != 0.); break;
    case ORCA_INFILL_PARAM_ENUM: static_cast<::ComboBox*>(control.window)->SetSelection(int(value)); break;
    default: static_cast<::TextInput*>(control.window)->GetTextCtrl()->SetValue(number_text(*control.param, value)); break;
    }
}

bool InfillPluginParamsDialog::accept()
{
    std::string text = m_text;
    for (const PatternControls& pattern : m_patterns) {
        std::vector<double> values;
        for (const Control& control : pattern.controls) {
            const InfillPluginParam& param = *control.param;
            if (param.type == ORCA_INFILL_PARAM_BOOL) {
                values.push_back(static_cast<::CheckBox*>(control.window)->GetValue() ? 1. : 0.);
            } else if (param.type == ORCA_INFILL_PARAM_ENUM) {
                values.push_back(double(static_cast<::ComboBox*>(control.window)->GetSelection()));
            } else {
                wxTextCtrl* input = static_cast<::TextInput*>(control.window)->GetTextCtrl();
                wxString    str   = input->GetValue();
                if (str == number_text(param, control.value)) {
                    values.push_back(control.value);
                    continue;
                }
                str.Replace(",", ".");
                double     value  = 0.;
                const bool number = str.Trim().Trim(false).ToCDouble(&value) && std::isfinite(value) &&
                                    (param.type == ORCA_INFILL_PARAM_FLOAT || value == std::round(value));
                // The bounds as the range text shows them are accepted too and clamped to the declared range.
                double min_shown = param.min_value, max_shown = param.max_value;
                number_text(param, param.min_value).ToCDouble(&min_shown);
                number_text(param, param.max_value).ToCDouble(&max_shown);
                if (!number || value < std::min(param.min_value, min_shown) || value > std::max(param.max_value, max_shown)) {
                    MessageDialog(this, (number ? _L("Value is out of range.") : _L("Invalid numeric input.")) + "\n" + from_u8(param.label) + ": " + range_text(param),
                                  wxEmptyString, wxICON_WARNING | wxOK)
                        .ShowModal();
                    input->SetFocus();
                    input->SelectAll();
                    return false;
                }
                values.push_back(std::clamp(value, param.min_value, param.max_value));
            }
        }
        // Only the patterns whose values changed are rewritten; the entries of the others stay as written, so text that
        // differs only in form does not turn the preset dirty.
        if (values != infill_plugin_param_values(pattern.pattern->id, m_text))
            text = infill_plugin_params_with(text, pattern.pattern->id, values);
    }
    m_text = std::move(text);
    return true;
}

void InfillPluginParamsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    for (wxWindow* child : m_list->GetChildren()) {
        if (auto check = dynamic_cast<::CheckBox*>(child))
            check->Rescale();
        else if (auto input = dynamic_cast<::TextInput*>(child)) {
            input->SetMinSize(FromDIP(wxSize(control_width, -1)));
            input->Rescale();
        } else if (auto button = dynamic_cast<Button*>(child))
            button->Rescale();
    }
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
