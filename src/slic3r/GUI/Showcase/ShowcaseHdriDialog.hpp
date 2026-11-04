#ifndef slic3r_GUI_ShowcaseHdriDialog_hpp_
#define slic3r_GUI_ShowcaseHdriDialog_hpp_

// Showcase realism 1h-3 (tests/design_showcase_realism.md 2.8): "Environments..." - the HDRI download
// catalogue (from our own mirror, D-R3: size and SHA-256 checked before a file is kept) and the user's
// own HDRIs (Load my own HDRI: .hdr or a scanline .exr, checked, kept as .hdr). Everything goes to
// <data folder>/showcase/hdri, and only when the user asks. libslic3r/Showcase/ShowcaseHdriLibrary.hpp
// holds the rules.

#include "libslic3r/Showcase/ShowcaseHdr.hpp"

#include <wx/dialog.h>

#include <string>

class wxBoxSizer;
class wxScrolledWindow;

namespace Slic3r {
namespace GUI {

class ShowcaseHdriDialog : public wxDialog
{
public:
    explicit ShowcaseHdriDialog(wxWindow *parent);
    // Something was downloaded, added or deleted: the environment lists need rebuilding.
    bool changed() const { return m_changed; }
    // The id of an HDRI the user just added or downloaded (to select it), else empty.
    const std::string &added_id() const { return m_added_id; }

private:
    void rebuild();
    void on_download(const Showcase::HdriInfo &hdri);
    void on_delete_download(const Showcase::HdriInfo &hdri);
    void on_add_own();
    void on_delete_own(const Showcase::HdriInfo &hdri);

    wxScrolledWindow *m_list = nullptr;
    wxBoxSizer       *m_list_sizer = nullptr;
    bool              m_changed = false;
    std::string       m_added_id;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseHdriDialog_hpp_
