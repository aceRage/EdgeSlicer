#pragma once

#include "GUI_Utils.hpp"

#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

// Asks which of the current rotation, scale and position become the new baseline
// (Plater::apply_transformation). Styled like the Clone dialog.
class ApplyTransformDialog : public DPIDialog
{
public:
    // position_available: false for whole objects, whose position is their place on the plate.
    ApplyTransformDialog(wxWindow *parent, bool position_available, bool position, bool rotation, bool scale);

    bool position() const { return m_position_cb->IsEnabled() && m_position_cb->GetValue(); }
    bool rotation() const { return m_rotation_cb->GetValue(); }
    bool scale() const { return m_scale_cb->GetValue(); }

private:
    ::CheckBox *m_rotation_cb{nullptr};
    ::CheckBox *m_scale_cb{nullptr};
    ::CheckBox *m_position_cb{nullptr};
    Button     *m_apply_btn{nullptr};

    void update_apply_button();
    void on_dpi_changed(const wxRect &suggested_rect) override {}
};

}} // namespace Slic3r::GUI
