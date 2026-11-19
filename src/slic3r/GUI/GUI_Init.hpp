#ifndef slic3r_GUI_Init_hpp_
#define slic3r_GUI_Init_hpp_

#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <memory>

namespace Slic3r {

namespace Showcase { struct RenderRequest; }

namespace GUI {

struct GUI_InitParams
{
	int		                    argc;
	char	                  **argv;

	// Substitutions of unknown configuration values done during loading of user presets.
	PresetsConfigSubstitutions  preset_substitutions;

    std::vector<std::string>    load_configs;
    DynamicPrintConfig          extra_config;
    std::vector<std::string>    input_files;

    //BBS: remove start_as_gcodeviewer logic
	//bool	                    start_as_gcodeviewer;
	bool                        input_gcode { false };
    // Ultra: --hidden / SNORCA_HIDDEN: no splash, no main-frame Show; the hub owns visibility.
    bool                        start_hidden { false };
    // --no-hub / --take-hub (RemoteHub::init_hub_mode): whether this copy may start or take over the phone hub.
    bool                        no_hub_flag { false };
    bool                        take_hub_flag { false };
    // Showcase S5: --showcase-render. A hidden instance that loads the project, renders and exits:
    // no single-instance hand-off, no network, no window, no dialogs (answered by the hidden policy).
    std::shared_ptr<Showcase::RenderRequest> showcase_render;
};

int GUI_Run(GUI_InitParams &params);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Init_hpp_
