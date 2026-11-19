#include "RealisticRenderSettings.hpp"

#include "AppConfig.hpp"

namespace Slic3r {

RealisticRenderSettings RealisticRenderSettings::from_app_config(const AppConfig &config)
{
    RealisticRenderSettings s;
    s.realistic = config.get_bool(SETTING_OPENGL_REALISTIC_MODE);
    s.phong     = config.get_bool(SETTING_OPENGL_REALISTIC_PHONG);
    s.shadows   = config.get_bool(SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS);
    s.ssao      = config.get_bool(SETTING_OPENGL_PHONG_SSAO);
    return s;
}

} // namespace Slic3r
