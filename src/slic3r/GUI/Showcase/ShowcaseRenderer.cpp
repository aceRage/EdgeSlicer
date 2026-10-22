#include <glad/gl.h>

#include "ShowcaseRenderer.hpp"
#include "ShowcaseTextures.hpp"
#include "ShowcaseWatermark.hpp"   // showcase_decode_image

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include "../3DScene.hpp"
#include "../Camera.hpp"
#include "../GLCanvas3D.hpp"
#include "../GLModel.hpp"
#include "../GLShader.hpp"
#include "../GUI_App.hpp"
#include "../ImGuiWrapper.hpp"
#include "../OpenGLManager.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

#include <imgui/imgui.h>

#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <sstream>

namespace Slic3r {
namespace GUI {

// The shadow map's texture unit (GLCanvas3D parks phong's shadow_map sampler there).
static constexpr int SHOWCASE_SHADOW_MAP_UNIT = 5;
// S2: the environment cube map and the layer table of showcase_material (GLShadersManager parks
// the samplers there); the floor's picture, mirror and contact shadow.
static constexpr int SHOWCASE_ENV_UNIT        = 6;
static constexpr int SHOWCASE_LAYER_UNIT      = 7;
static constexpr int SHOWCASE_FLOOR_TEX_UNIT  = 0;
static constexpr int SHOWCASE_REFLECTION_UNIT = 1;
static constexpr int SHOWCASE_CONTACT_UNIT    = 2;
// The contact shadow's footprint texture.
static constexpr int SHOWCASE_CONTACT_SIZE    = 512;

static ColorRGBA showcase_color(const std::string &hex, const ColorRGBA &fallback)
{
    std::array<float, 4> c;
    if (!Showcase::parse_hex_color(hex, c))
        return fallback;
    return ColorRGBA(c[0], c[1], c[2], c[3]);
}

ShowcaseRenderer::ShowcaseRenderer(GLCanvas3D &canvas) : m_canvas(canvas)
{
    m_canvas.set_showcase_renderer(this);
}

ShowcaseRenderer::~ShowcaseRenderer()
{
    m_canvas.set_showcase_renderer(nullptr);
    // The GL objects go with the context when the canvas is destroyed during shutdown; release
    // them here only while the canvas can still be made current.
    const bool has_gl = m_bg_texture != 0 || m_wm_texture != 0 || (m_floor_model != nullptr && m_floor_model->is_initialized()) ||
                        m_env_texture != 0 || m_floor_texture != 0 || m_contact_fbo != 0 || m_refl_fbo != 0 || !m_layers.empty();
    if (has_gl && m_canvas.get_wxglcanvas() != nullptr && m_canvas._set_current())
        release_gl();
    delete m_floor_model;
}

void ShowcaseRenderer::release_gl()
{
    if (m_bg_texture != 0) {
        glsafe(::glDeleteTextures(1, &m_bg_texture));
        m_bg_texture = 0;
    }
    if (m_wm_texture != 0) {
        glsafe(::glDeleteTextures(1, &m_wm_texture));
        m_wm_texture = 0;
    }
    if (m_floor_model != nullptr)
        m_floor_model->reset();
    m_bg_image_key.clear();
    m_wm_texture_version = 0;
    if (m_env_texture != 0) {
        glsafe(::glDeleteTextures(1, &m_env_texture));
        m_env_texture = 0;
    }
    m_env_key.clear();
    if (m_floor_texture != 0) {
        glsafe(::glDeleteTextures(1, &m_floor_texture));
        m_floor_texture = 0;
    }
    m_floor_key.clear();
    release_offscreen();
    release_layers();
}

void ShowcaseRenderer::release_offscreen()
{
    if (m_contact_fbo != 0) {
        glsafe(::glDeleteFramebuffers(1, &m_contact_fbo));
        m_contact_fbo = 0;
    }
    for (unsigned int &t : m_contact_tex)
        if (t != 0) {
            glsafe(::glDeleteTextures(1, &t));
            t = 0;
        }
    if (m_refl_fbo != 0) {
        glsafe(::glDeleteFramebuffers(1, &m_refl_fbo));
        m_refl_fbo = 0;
    }
    if (m_refl_tex != 0) {
        glsafe(::glDeleteTextures(1, &m_refl_tex));
        m_refl_tex = 0;
    }
    if (m_refl_depth != 0) {
        glsafe(::glDeleteRenderbuffers(1, &m_refl_depth));
        m_refl_depth = 0;
    }
    m_refl_w = m_refl_h = 0;
    m_contact_valid = m_refl_valid = false;
}

void ShowcaseRenderer::release_layers()
{
    for (auto &it : m_layers)
        if (it.second.texture != 0)
            glsafe(::glDeleteTextures(1, &it.second.texture));
    m_layers.clear();
    m_layers_valid = false;
}

bool ShowcaseRenderer::is_supported()
{
    return OpenGLManager::get_gl_info().is_version_greater_or_equal_to(3, 2) &&
           OpenGLManager::get_framebuffers_type() == OpenGLManager::EFramebufferType::Arb;
}

void ShowcaseRenderer::set_scene(const Showcase::Scene *scene, const Showcase::Project *project)
{
    if (scene == nullptr || m_scene == nullptr || scene->background.image != m_scene->background.image)
        m_bg_image_key.clear();
    m_scene   = scene;
    m_project = project;
    ++m_watermark_version;
}

// ------------------------------------------------------------------------------------------------
// The scene
// ------------------------------------------------------------------------------------------------

// Only the current plate's objects take part (D17), and its prime tower unless hidden. The
// Showcase canvas owns its GLVolumes, so is_active is free to use for this every frame.
void ShowcaseRenderer::update_visibility()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    PartPlateList &plates    = plater->get_partplate_list();
    PartPlate     *plate     = plates.get_curr_plate();
    const int      plate_idx = plates.get_curr_plate_index();
    const bool     show_wt   = m_scene != nullptr && !m_scene->floor.hide_wipe_tower;
    for (GLVolume *v : m_canvas.m_volumes.volumes) {
        if (v == nullptr)
            continue;
        if (v->is_wipe_tower)
            v->is_active = show_wt && v->object_idx() - 1000 == plate_idx;
        else
            v->is_active = !v->is_modifier && v->composite_id.volume_id >= 0 && plate != nullptr &&
                           plate->contain_instance(v->object_idx(), v->instance_idx());
        v->hover    = GLVolume::HS_None;
        v->selected = false;
    }
}

BoundingBoxf3 ShowcaseRenderer::objects_box() const
{
    BoundingBoxf3 box;
    for (const GLVolume *v : m_canvas.m_volumes.volumes)
        if (v != nullptr && v->is_active)
            box.merge(v->transformed_bounding_box());
    return box;
}

bool ShowcaseRenderer::floor_drawn() const
{
    return m_scene != nullptr && m_scene->floor.hide_plate && m_scene->floor.type != Showcase::FloorType::None;
}

// A disc under the objects, large enough that its faded rim is well away from them.
void ShowcaseRenderer::floor_disc(Vec3d &center, double &radius) const
{
    BoundingBoxf3 box = objects_box();
    if (!box.defined) {
        if (Plater *plater = wxGetApp().plater())
            box = plater->get_partplate_list().get_curr_plate()->get_bounding_box();
    }
    if (!box.defined) {
        center = Vec3d::Zero();
        radius = 100.;
        return;
    }
    center = Vec3d(0.5 * (box.min.x() + box.max.x()), 0.5 * (box.min.y() + box.max.y()), 0.);
    const double half_diag = 0.5 * Vec2d(box.size().x(), box.size().y()).norm();
    radius = std::max(3.5 * std::max(half_diag, 0.5 * box.size().z()), 80.);
}

BoundingBoxf3 ShowcaseRenderer::scene_box() const
{
    BoundingBoxf3 box = objects_box();
    if (floor_drawn()) {
        Vec3d  c;
        double r;
        floor_disc(c, r);
        box.merge(BoundingBoxf3(Vec3d(c.x() - r, c.y() - r, -0.1), Vec3d(c.x() + r, c.y() + r, 0.1)));
    }
    if (m_scene == nullptr || !m_scene->floor.hide_plate) {
        if (Plater *plater = wxGetApp().plater())
            box.merge(plater->get_partplate_list().get_curr_plate()->get_bounding_box(true));
    }
    if (!box.defined)
        box = BoundingBoxf3(Vec3d(-50., -50., 0.), Vec3d(50., 50., 50.));
    return box;
}

RealisticRenderSettings ShowcaseRenderer::render_settings(const DrawTarget &t) const
{
    RealisticRenderSettings rs;
    rs.realistic = true;
    rs.phong     = true;
    if (m_scene == nullptr)
        return rs;
    const Showcase::Lighting &li = m_scene->lighting;
    rs.shadows           = li.shadows;
    rs.ssao              = li.ssao;
    rs.world_light       = li.world_fixed;
    rs.world_light_dir   = Showcase::light_direction(li.azimuth_deg, li.elevation_deg);
    rs.shadow_intensity  = li.shadow_intensity;
    rs.shadow_map_size   = t.shadow_map_size;
    rs.ssao_radius_scale = t.ssao_radius_scale;
    rs.ssao_keep_alpha   = m_scene->background.type == Showcase::BackgroundType::Transparent && !t.live;
    rs.plate_shadow      = !m_scene->floor.hide_plate;
    if (floor_drawn()) {
        Vec3d  c;
        double r;
        floor_disc(c, r);
        rs.fit_min = { c.x() - r, c.y() - r, 0. };
        rs.fit_max = { c.x() + r, c.y() + r, 0. };
    }
    return rs;
}

// ------------------------------------------------------------------------------------------------
// S2: lights, layer heights, bundled assets
// ------------------------------------------------------------------------------------------------

std::vector<Showcase::LightSource> ShowcaseRenderer::scene_lights(const Camera &camera) const
{
    if (m_scene == nullptr)
        return {};
    Showcase::Lighting li = m_scene->lighting;
    if (!li.world_fixed) {
        // The editing views' headlight (phong's LIGHT_TOP_DIR, in eye space) taken to the world;
        // the fill and the rim keep their places around it.
        const Vec3d eye_dir = Vec3d(-0.4574957, 0.4574957, 0.7624929).normalized();
        const Vec3d w       = (camera.get_view_matrix().matrix().block<3, 3>(0, 0).transpose() * eye_dir).normalized();
        li.azimuth_deg   = float(std::atan2(w.y(), w.x()) * 180. / PI);
        li.elevation_deg = float(std::asin(std::clamp(w.z(), -1., 1.)) * 180. / PI);
    }
    return Showcase::three_point_lights(li);
}

// The object's layers as the slicer will make them (layer height, first layer, a variable layer
// height profile or height range modifiers), for the procedural layer lines (design 2.6.2).
// Cached until invalidate_layers(): Showcase never edits the model, so it changes only when the
// tab is entered again or the scene is reloaded.
const ShowcaseRenderer::LayerInfo *ShowcaseRenderer::layers_for(int object_idx)
{
    if (!m_layers_valid) {
        release_layers();
        m_layers_valid = true;
    }
    if (auto it = m_layers.find(object_idx); it != m_layers.end())
        return &it->second;
    LayerInfo &info = m_layers[object_idx];
    const Model &model = wxGetApp().model();
    if (object_idx < 0 || size_t(object_idx) >= model.objects.size() || wxGetApp().preset_bundle == nullptr)
        return &info;
    const ModelObject *mo = model.objects[size_t(object_idx)];
    std::vector<coordf_t> boundaries;
    try {
        const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
        const SlicingParameters  sp   = PrintObject::slicing_parameters(full, *mo, float(mo->max_z()), Vec3d::Ones());
        std::vector<coordf_t>    profile;
        PrintObject::update_layer_height_profile(*mo, sp, profile);
        boundaries = generate_object_layers(sp, profile, false);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: no layer lines for object " << object_idx << ": " << ex.what();
        return &info;
    }
    const Showcase::LayerTable table(boundaries);
    if (table.empty())
        return &info;
    if (table.uniform()) {
        info.mode   = 1;
        info.first  = table.first_layer_height();
        info.height = table.layer_height();
        return &info;
    }
    // Variable layer heights: the tops in an R32F texture, 1024 a row, binary searched in the shader.
    const int width = 1024;
    const int count = int(std::min<size_t>(table.size(), size_t(width) * size_t(width)));
    const int rows  = (count + width - 1) / width;
    std::vector<float> data(size_t(width) * size_t(rows), table.tops()[size_t(count - 1)]);
    std::copy_n(table.tops().begin(), size_t(count), data.begin());
    glsafe(::glGenTextures(1, &info.texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, info.texture));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, rows, 0, GL_RED, GL_FLOAT, data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    info.mode   = 2;
    info.count  = count;
    info.width  = width;
    info.first  = table.first_layer_height();
    info.height = table.layer_height();
    return &info;
}

const Showcase::EnvironmentInfo *ShowcaseRenderer::ensure_environment()
{
    if (m_scene == nullptr || m_scene->lighting.environment.empty())
        return nullptr;
    const Showcase::EnvironmentManifest &manifest = showcase_environments();
    const Showcase::EnvironmentInfo     *env      = manifest.find(m_scene->lighting.environment);
    if (env == nullptr)
        return nullptr;
    if (m_env_key == env->id)
        return m_env_texture != 0 ? env : nullptr;
    // One environment at a time is kept on the GPU.
    if (m_env_texture != 0) {
        glsafe(::glDeleteTextures(1, &m_env_texture));
        m_env_texture = 0;
    }
    m_env_key = env->id;   // a file that fails is not retried every frame
    ShowcaseImage atlas;
    if (!showcase_load_rgba_file(showcase_environment_path(*env), atlas)) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the environment " << env->id << " could not be read";
        return nullptr;
    }
    m_env_texture = showcase_upload_environment(atlas, manifest.face_size, manifest.levels);
    return m_env_texture != 0 ? env : nullptr;
}

bool ShowcaseRenderer::ensure_floor_texture(float &tile_mm, float &roughness)
{
    if (m_scene == nullptr)
        return false;
    const Showcase::Floor &fl = m_scene->floor;
    std::string key;
    if (fl.type == Showcase::FloorType::Texture)
        key = "builtin:" + fl.texture;
    else if (fl.type == Showcase::FloorType::Image && !fl.image.empty())
        key = "image:" + fl.image;
    else
        return false;
    if (key != m_floor_key) {
        if (m_floor_texture != 0) {
            glsafe(::glDeleteTextures(1, &m_floor_texture));
            m_floor_texture = 0;
        }
        m_floor_key = key;
        ShowcaseImage image;
        bool          ok = false;
        if (fl.type == Showcase::FloorType::Texture) {
            if (const Showcase::FloorTextureInfo *info = showcase_floors().find(fl.texture)) {
                ok                = showcase_load_rgba_file(showcase_floor_path(*info), image);
                m_floor_tile_mm   = info->tile_mm;
                m_floor_roughness = info->roughness;
            }
        } else if (m_project != nullptr) {
            if (const Showcase::ImageBlob *blob = m_project->find_image(fl.image))
                ok = showcase_decode_image(blob->bytes, image);
            m_floor_roughness = 0.6f;
        }
        if (ok)
            m_floor_texture = showcase_upload_texture(image, true);
        else
            BOOST_LOG_TRIVIAL(warning) << "Showcase: the floor picture " << key << " could not be read";
    }
    if (m_floor_texture == 0)
        return false;
    tile_mm   = fl.type == Showcase::FloorType::Texture ? m_floor_tile_mm * fl.texture_scale : fl.texture_size;
    roughness = m_floor_roughness;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------------

void ShowcaseRenderer::ensure_fullscreen_quad()
{
    GLModel &quad = m_canvas.m_background;
    if (quad.is_initialized())
        return;
    quad.reset();
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
    init_data.reserve_vertices(4);
    init_data.reserve_indices(6);
    init_data.add_vertex(Vec2f(-1.0f, -1.0f));
    init_data.add_vertex(Vec2f(1.0f, -1.0f));
    init_data.add_vertex(Vec2f(1.0f, 1.0f));
    init_data.add_vertex(Vec2f(-1.0f, 1.0f));
    init_data.add_triangle(0, 1, 2);
    init_data.add_triangle(2, 3, 0);
    quad.init_from(std::move(init_data));
}

void ShowcaseRenderer::ensure_floor_model()
{
    if (m_floor_model == nullptr)
        m_floor_model = new GLModel();
    if (m_floor_model->is_initialized())
        return;
    const int segments = 96;
    GLModel::Geometry disc;
    disc.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    disc.reserve_vertices(segments + 1);
    disc.reserve_indices(segments * 3);
    disc.add_vertex(Vec3f(0.f, 0.f, 0.f));
    for (int i = 0; i < segments; ++i) {
        const float a = 2.f * float(PI) * float(i) / float(segments);
        disc.add_vertex(Vec3f(std::cos(a), std::sin(a), 0.f));
    }
    for (int i = 0; i < segments; ++i)
        disc.add_triangle(0, 1 + i, 1 + (i + 1) % segments);
    m_floor_model->init_from(std::move(disc));
}

bool ShowcaseRenderer::ensure_background_texture()
{
    if (m_scene == nullptr || m_project == nullptr || m_scene->background.image.empty())
        return false;
    if (m_bg_texture != 0 && m_bg_image_key == m_scene->background.image)
        return true;
    const Showcase::ImageBlob *blob = m_project->find_image(m_scene->background.image);
    if (blob == nullptr || blob->bytes.empty())
        return false;
    wxMemoryInputStream stream(blob->bytes.data(), blob->bytes.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY) || !image.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the backdrop image could not be decoded";
        m_bg_image_key = m_scene->background.image;   // do not retry every frame
        return false;
    }
    const int w = image.GetWidth(), h = image.GetHeight();
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    const unsigned char *rgb   = image.GetData();
    const unsigned char *alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = alpha ? alpha[i] : 255;
    }
    if (m_bg_texture == 0)
        glsafe(::glGenTextures(1, &m_bg_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_bg_texture));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()));
    // Mipmapped for the S2 blur; a sharp backdrop reads level 0 only, as in S1.
    glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    m_bg_image_w   = w;
    m_bg_image_h   = h;
    m_bg_image_key = m_scene->background.image;
    return true;
}

void ShowcaseRenderer::draw_background(const DrawTarget &t)
{
    ensure_fullscreen_quad();
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_BLEND));

    const ColorRGBA white(1.f, 1.f, 1.f, 1.f);
    const ColorRGBA grey(0.85f, 0.86f, 0.88f, 1.f);
    Showcase::BackgroundType type = m_scene ? m_scene->background.type : Showcase::BackgroundType::VerticalGradient;
    ColorRGBA c1 = m_scene ? showcase_color(m_scene->background.color1, white) : white;
    ColorRGBA c2 = m_scene ? showcase_color(m_scene->background.color2, grey) : grey;

    if (type == Showcase::BackgroundType::Transparent && !t.live) {
        // An export with a transparent backdrop: leave the clear colour (alpha 0).
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        return;
    }

    GLShaderProgram *shader = wxGetApp().get_shader("showcase_background");
    if (shader == nullptr) {
        // Fallback: the editing views' two-colour gradient.
        shader = wxGetApp().get_shader("background");
        if (shader != nullptr) {
            shader->start_using();
            const bool solid = type == Showcase::BackgroundType::Solid;
            shader->set_uniform("top_color", c1);
            shader->set_uniform("bottom_color", solid ? c1 : c2);
            m_canvas.m_background.render();
            shader->stop_using();
        }
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        return;
    }

    int mode = 0;
    switch (type) {
    case Showcase::BackgroundType::Solid: mode = 0; break;
    case Showcase::BackgroundType::VerticalGradient: mode = 1; break;
    case Showcase::BackgroundType::RadialGradient: mode = 2; break;
    case Showcase::BackgroundType::Image: mode = ensure_background_texture() ? 3 : 1; break;
    case Showcase::BackgroundType::Transparent: mode = 4; break;
    case Showcase::BackgroundType::Environment: mode = 5; break;
    }
    // S2: the environment behind the model; without one, the scene's two colours as a gradient.
    const Showcase::EnvironmentInfo *env = mode == 5 ? ensure_environment() : nullptr;
    if (mode == 5 && env == nullptr)
        mode = 1;

    const float fw = std::max(1e-6f, (t.frame[2] - t.frame[0]) * float(t.width));
    const float fh = std::max(1e-6f, (t.frame[3] - t.frame[1]) * float(t.height));
    const float frame_aspect = fw / fh;

    shader->start_using();
    shader->set_uniform("mode", mode);
    shader->set_uniform("color1", c1);
    shader->set_uniform("color2", c2);
    shader->set_uniform("frame_rect", std::array<float, 4>{ t.frame[0], t.frame[1], t.frame[2], t.frame[3] });
    shader->set_uniform("frame_aspect", frame_aspect);
    shader->set_uniform("viewport_size", Vec2f(float(t.width), float(t.height)));
    const float blur = m_scene ? std::clamp(m_scene->background.blur, 0.f, 1.f) : 0.f;
    shader->set_uniform("blur", blur);
    // Level 7 of a 4096 px picture is 32 px: very soft, still recognisable.
    shader->set_uniform("image_lod", blur * 7.f);
    if (mode == 5) {
        const Camera &camera = wxGetApp().plater()->get_camera();
        const Matrix4d inv_vp = (camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix()).inverse();
        shader->set_uniform("inv_view_projection", inv_vp);
        shader->set_uniform("camera_position", camera.get_position());
        shader->set_uniform("env_max_lod", float(std::max(0, showcase_environments().levels - 1)));
        shader->set_uniform("env_rotation", float(m_scene->lighting.env_rotation_deg * PI / 180.));
        shader->set_uniform("env_exposure", m_scene->lighting.env_intensity * std::exp2(m_scene->lighting.exposure));
        shader->set_uniform("env_map", SHOWCASE_ENV_UNIT);
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_ENV_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, m_env_texture));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    if (mode == 3) {
        // Fill (cover), fit (contain, the rest in colour 1) or tile (three rows of the picture).
        const float ia = float(m_bg_image_w) / float(std::max(1, m_bg_image_h));
        float sx = 1.f, sy = 1.f;
        const Showcase::ImageFit fit = m_scene->background.fit;
        if (fit == Showcase::ImageFit::Tile) {
            sy = 3.f;
            sx = 3.f * frame_aspect / ia;
        } else {
            const bool wider = ia > frame_aspect;
            if ((fit == Showcase::ImageFit::Fill) == wider)
                sx = frame_aspect / ia;
            else
                sy = ia / frame_aspect;
        }
        const float ox = fit == Showcase::ImageFit::Tile ? 0.f : 0.5f - 0.5f * sx;
        const float oy = fit == Showcase::ImageFit::Tile ? 0.f : 0.5f - 0.5f * sy;
        shader->set_uniform("uv_transform", std::array<float, 4>{ sx, sy, ox, oy });
        shader->set_uniform("tile", fit == Showcase::ImageFit::Tile);
        shader->set_uniform("image_tex", 0);
        glsafe(::glActiveTexture(GL_TEXTURE0));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_bg_texture));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, fit == Showcase::ImageFit::Tile ? GL_REPEAT : GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, fit == Showcase::ImageFit::Tile ? GL_REPEAT : GL_CLAMP_TO_EDGE));
    }
    m_canvas.m_background.render();
    if (mode == 3)
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    if (mode == 5) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_ENV_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    shader->stop_using();

    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void ShowcaseRenderer::draw_floor(const DrawTarget &t)
{
    (void) t;
    if (!floor_drawn())
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    // From below the floor there is nothing to see of it.
    if (camera.get_position().z() < 0.)
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("showcase_floor");
    if (shader == nullptr)
        return;
    ensure_floor_model();

    Vec3d  center;
    double radius;
    floor_disc(center, radius);
    Transform3d model = Transform3d::Identity();
    model.translate(center);
    model.scale(Vec3d(radius, radius, 1.));

    const Showcase::Floor &fl = m_scene->floor;
    int mode = 1;
    if (fl.type == Showcase::FloorType::Solid)
        mode = 2;
    else if (fl.type == Showcase::FloorType::Grid)
        mode = 3;
    // S2: a photo floor, plain colour while its picture is missing.
    float tile_mm = 500.f, roughness = 0.2f;
    if (fl.type == Showcase::FloorType::Texture || fl.type == Showcase::FloorType::Image)
        mode = ensure_floor_texture(tile_mm, roughness) ? 4 : 2;

    const bool shadows = m_canvas.m_shadow_map_valid && m_canvas.m_shadow_map_texture_id != 0;
    const std::vector<Showcase::LightSource> lights = scene_lights(camera);
    const Vec3f light = lights.empty() ? Vec3f(0.f, 0.f, 1.f) : Vec3f(lights[0].dir[0], lights[0].dir[1], lights[0].dir[2]);
    Vec3f tint(0.f, 0.f, 0.f);
    if (!lights.empty()) {
        const float m = std::max({ lights[0].color[0], lights[0].color[1], lights[0].color[2], 1e-6f });
        tint = Vec3f(lights[0].color[0] / m, lights[0].color[1] / m, lights[0].color[2] / m);
    }
    GLint vp[4] = { 0, 0, 1, 1 };
    glsafe(::glGetIntegerv(GL_VIEWPORT, vp));

    // The floor is under every object, so it can never hide one: no depth test or write. Objects
    // drawn afterwards cover it, and SSAO sees only the objects.
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glEnable(GL_BLEND));
    // Alpha accumulates as coverage, so a transparent export keeps the shadow as alpha.
    glsafe(::glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));

    shader->start_using();
    if (shadows) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_SHADOW_MAP_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_canvas.m_shadow_map_texture_id));
    }
    if (mode == 4) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_FLOOR_TEX_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_floor_texture));
    }
    if (m_refl_valid) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_REFLECTION_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_refl_tex));
    }
    if (m_contact_valid) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_CONTACT_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_contact_tex[0]));
    }
    glsafe(::glActiveTexture(GL_TEXTURE0));
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("model_matrix", model);
    shader->set_uniform("use_shadow", shadows);
    shader->set_uniform("shadow_map", SHOWCASE_SHADOW_MAP_UNIT);
    shader->set_uniform("shadow_light_vp", m_canvas.m_shadow_light_vp);
    shader->set_uniform("shadow_map_texel", 1.0f / float(std::max(1u, m_canvas.m_shadow_map_size)));
    shader->set_uniform("shadow_softness", m_scene->lighting.shadow_softness);
    shader->set_uniform("mode", mode);
    shader->set_uniform("floor_color", showcase_color(fl.color, ColorRGBA(0.95f, 0.95f, 0.95f, 1.f)));
    shader->set_uniform("shadow_strength", fl.shadow_strength);
    shader->set_uniform("fade_start", 0.45f);
    shader->set_uniform("grid_spacing", fl.grid_spacing);
    shader->set_uniform("light_dir", light);
    shader->set_uniform("light_tint", tint);
    shader->set_uniform("floor_tex", SHOWCASE_FLOOR_TEX_UNIT);
    shader->set_uniform("tile_mm", tile_mm);
    shader->set_uniform("contact_tex", SHOWCASE_CONTACT_UNIT);
    shader->set_uniform("use_contact", m_contact_valid);
    shader->set_uniform("contact_rect", std::array<float, 4>{ m_contact_rect[0], m_contact_rect[1], m_contact_rect[2], m_contact_rect[3] });
    shader->set_uniform("contact_strength", fl.contact_shadow);
    shader->set_uniform("reflection_tex", SHOWCASE_REFLECTION_UNIT);
    shader->set_uniform("use_reflection", m_refl_valid);
    shader->set_uniform("reflection_strength", fl.reflection);
    // Rougher floors blur the mirror image through its mips.
    shader->set_uniform("reflection_lod", 0.5f + 3.5f * std::clamp(roughness, 0.f, 1.f));
    shader->set_uniform("viewport_origin", Vec2f(float(vp[0]), float(vp[1])));
    shader->set_uniform("viewport_size", Vec2f(float(vp[2]), float(vp[3])));
    shader->set_uniform("camera_position", camera.get_position());
    m_floor_model->render();
    for (int unit : { SHOWCASE_SHADOW_MAP_UNIT, SHOWCASE_FLOOR_TEX_UNIT, SHOWCASE_REFLECTION_UNIT, SHOWCASE_CONTACT_UNIT }) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + unit));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    }
    glsafe(::glActiveTexture(GL_TEXTURE0));
    shader->stop_using();

    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void ShowcaseRenderer::draw_objects()
{
    GLVolumeCollection &volumes = m_canvas.m_volumes;
    if (volumes.empty())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("phong");
    if (shader == nullptr)
        shader = wxGetApp().get_shader("gouraud");
    if (shader == nullptr)
        return;

    const Camera                 &camera = wxGetApp().plater()->get_camera();
    const RealisticRenderSettings rs     = m_canvas._render_settings();

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    // Every volume's colour, its own print volume test off (a presentation never darkens an object
    // that overhangs the plate), no clipping, no sinking contours.
    volumes.set_print_volume({ 3, { -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, FLT_MAX } });
    volumes.set_z_range(-FLT_MAX, FLT_MAX);
    volumes.set_clipping_plane(ClippingPlane::ClipsNothing().get_data());
    volumes.set_show_sinking_contours(false);

    shader->start_using();
    const bool sample_shadows = m_canvas.m_shadow_map_valid && m_canvas.m_shadow_map_texture_id != 0;
    if (sample_shadows) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_SHADOW_MAP_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_canvas.m_shadow_map_texture_id));
        glsafe(::glActiveTexture(GL_TEXTURE0));
        shader->set_uniform("shadow_map", SHOWCASE_SHADOW_MAP_UNIT);
        shader->set_uniform("shadow_light_vp", m_canvas.m_shadow_light_vp);
        shader->set_uniform("shadow_map_texel", 1.0f / static_cast<float>(m_canvas.m_shadow_map_size));
        shader->set_uniform("shadow_intensity", rs.shadow_intensity);
    } else
        shader->set_uniform("shadow_intensity", 0.0f);
    volumes.set_shadow_map_texture(sample_shadows ? m_canvas.m_shadow_map_texture_id : 0);
    GLCanvas3D::_set_key_light_uniforms(*shader, rs, camera.get_view_matrix());

    const Size cnv  = m_canvas.get_canvas_size();
    auto       keep = [](const GLVolume &v) { return v.is_active && (!v.is_modifier || v.is_wipe_tower); };
    volumes.render(GLVolumeCollection::ERenderType::Opaque, false, camera, cnv, keep, false);
    volumes.render(GLVolumeCollection::ERenderType::Transparent, false, camera, cnv, keep, false);

    if (sample_shadows) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_SHADOW_MAP_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    // gouraud and phong serve the editing views too: leave them as those expect.
    shader->set_uniform("shadow_intensity", 0.0f);
    shader->set_uniform("use_custom_key_light", false);
    volumes.set_shadow_map_texture(0);
    shader->stop_using();
}

// S2: the objects with the filament materials. Draws each visible volume itself (rather than through
// GLVolumeCollection::render) so every filament - also every painted colour of a multi-material
// volume - gets its own finish. Opaque first; translucent filaments after, sorted back to front per
// volume and drawn back faces then front faces (D15). `mirrored` draws them reflected in the floor
// (Z = 0) for the glossy floor.
bool ShowcaseRenderer::draw_objects_material(bool mirrored)
{
    GLShaderProgram *shader = wxGetApp().get_shader("showcase_material");
    if (shader == nullptr || m_scene == nullptr)
        return false;
    GLVolumeCollection &volumes = m_canvas.m_volumes;
    if (volumes.empty())
        return true;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Transform3d   view   = camera.get_view_matrix();
    Vec3d         eye    = camera.get_position();
    if (mirrored) {
        Transform3d m = Transform3d::Identity();
        m.scale(Vec3d(1., 1., -1.));
        view = view * m;
        eye.z() = -eye.z();
    }
    const Transform3d &projection = camera.get_projection_matrix();
    const RealisticRenderSettings rs = m_canvas._render_settings();

    // Every filament's colour (painted and mixed ones too) and finish.
    const std::vector<ColorRGBA>        colors    = get_extruders_colors();
    const std::vector<ShowcaseFilament> filaments = showcase_filaments();
    auto finish_of = [this, &filaments](int filament) {
        if (filament >= 1 && size_t(filament) <= filaments.size())
            return m_scene->materials.finish_for(filament, filaments[size_t(filament - 1)].preset_name, filaments[size_t(filament - 1)].type);
        const Showcase::Finish *f = m_scene->materials.override_for(filament);
        return f != nullptr ? *f : Showcase::Finish::Standard;
    };
    auto color_of = [&colors](int filament, const ColorRGBA &fallback) {
        return filament >= 1 && size_t(filament) <= colors.size() ? colors[size_t(filament - 1)] : fallback;
    };

    struct Item
    {
        GLVolume        *volume = nullptr;
        int              sub    = -1;   // painted part (GLVolume::mmuseg_models), -1 = the whole volume
        ColorRGBA        color;
        Showcase::Finish finish = Showcase::Finish::Standard;
        double           depth  = 0.;
    };
    std::vector<Item> opaque, translucent;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || !v->is_active || (v->is_modifier && !v->is_wipe_tower))
            continue;
        Item it;
        it.volume = v;
        it.depth  = v->transformed_bounding_box().transformed(view).max.z();
        if (v->is_wipe_tower) {
            it.color = v->color;
            opaque.push_back(it);
            continue;
        }
        const ModelObject *mo = v->object_idx() >= 0 && size_t(v->object_idx()) < objects.size() ? objects[size_t(v->object_idx())] : nullptr;
        const ModelVolume *mv = mo != nullptr && v->volume_idx() >= 0 && size_t(v->volume_idx()) < mo->volumes.size() ?
                                    mo->volumes[size_t(v->volume_idx())] : nullptr;
        int extruder = mv != nullptr ? mv->extruder_id() : v->extruder_id;
        if (extruder <= 0)
            extruder = 1;
        auto push = [&](Item item, int filament) {
            item.color  = color_of(filament, v->color);
            item.finish = finish_of(filament);
            (Showcase::material_params(item.finish).alpha < 1.f || item.color.a() < 1.f ? translucent : opaque).push_back(item);
        };
        if (mv != nullptr && v->printable && !mv->mmu_segmentation_facets.empty()) {
            // Multi-material painting: one part per filament, as GLVolume::simple_render keeps them.
            if (mv->mmu_segmentation_facets.timestamp() != v->mmuseg_ts) {
                v->mmuseg_models.clear();
                std::vector<indexed_triangle_set> its_per_color;
                mv->mmu_segmentation_facets.get_facets(*mv, its_per_color);
                v->mmuseg_models.resize(its_per_color.size());
                for (size_t i = 0; i < its_per_color.size(); ++i)
                    v->mmuseg_models[i].init_from(its_per_color[i]);
                v->mmuseg_ts = mv->mmu_segmentation_facets.timestamp();
            }
            for (size_t i = 0; i < v->mmuseg_models.size(); ++i) {
                if (!v->mmuseg_models[i].is_initialized())
                    continue;
                Item part  = it;
                part.sub   = int(i);
                const int filament = i == 0 ? extruder : (i <= colors.size() ? int(i) : 1);
                push(part, filament);
            }
        } else
            push(it, extruder);
    }
    std::sort(translucent.begin(), translucent.end(), [](const Item &a, const Item &b) { return a.depth < b.depth; });

    // Frame uniforms.
    shader->start_using();
    const bool sample_shadows = m_canvas.m_shadow_map_valid && m_canvas.m_shadow_map_texture_id != 0;
    if (sample_shadows) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_SHADOW_MAP_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_canvas.m_shadow_map_texture_id));
    }
    const Showcase::EnvironmentInfo *env = ensure_environment();
    if (env != nullptr) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_ENV_UNIT));
        glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, m_env_texture));
    }
    glsafe(::glActiveTexture(GL_TEXTURE0));
    shader->set_uniform("shadow_map", SHOWCASE_SHADOW_MAP_UNIT);
    shader->set_uniform("shadow_light_vp", m_canvas.m_shadow_light_vp);
    shader->set_uniform("shadow_map_texel", 1.0f / float(std::max(1u, m_canvas.m_shadow_map_size)));
    shader->set_uniform("shadow_intensity", sample_shadows ? rs.shadow_intensity : 0.f);
    shader->set_uniform("shadow_softness", m_scene->lighting.shadow_softness);
    const std::vector<Showcase::LightSource> lights = scene_lights(camera);
    shader->set_uniform("light_count", int(std::min<size_t>(lights.size(), 3)));
    for (size_t i = 0; i < 3; ++i) {
        const Showcase::LightSource l = i < lights.size() ? lights[i] : Showcase::LightSource();
        const std::string           n = std::to_string(i);
        shader->set_uniform(("light_dir[" + n + "]").c_str(), Vec3f(l.dir[0], l.dir[1], l.dir[2]));
        shader->set_uniform(("light_color[" + n + "]").c_str(), Vec3f(l.color[0], l.color[1], l.color[2]));
    }
    shader->set_uniform("camera_position", eye);
    shader->set_uniform("exposure", std::exp2(m_scene->lighting.exposure));
    shader->set_uniform("use_env", env != nullptr);
    shader->set_uniform("env_map", SHOWCASE_ENV_UNIT);
    shader->set_uniform("env_max_lod", float(std::max(0, showcase_environments().levels - 1)));
    shader->set_uniform("env_intensity", m_scene->lighting.env_intensity);
    shader->set_uniform("env_rotation", float(m_scene->lighting.env_rotation_deg * PI / 180.));
    for (size_t i = 0; i < 9; ++i) {
        const std::array<float, 3> c = env != nullptr ? env->sh[i] : std::array<float, 3>{ 0.f, 0.f, 0.f };
        shader->set_uniform(("env_sh[" + std::to_string(i) + "]").c_str(), Vec3f(c[0], c[1], c[2]));
    }
    const float line_depth = Showcase::layer_line_depth(m_scene->materials.layer_lines);
    shader->set_uniform("line_depth", line_depth);
    shader->set_uniform("layer_table", SHOWCASE_LAYER_UNIT);
    // Nothing below the floor: a sinking part would show through a floor that writes no depth.
    shader->set_uniform("clip_floor", mirrored || floor_drawn());

    auto draw = [&](const Item &it) {
        GLVolume *v = it.volume;
        const Showcase::MaterialParams mp = Showcase::material_params(it.finish);
        shader->set_uniform("mat_roughness", mp.roughness);
        shader->set_uniform("mat_f0", mp.f0);
        shader->set_uniform("mat_metal_tint", mp.metal_tint);
        shader->set_uniform("mat_anisotropy", mp.anisotropy);
        shader->set_uniform("mat_clearcoat", mp.clearcoat);
        shader->set_uniform("mat_alpha", v->is_wipe_tower ? 1.f : mp.alpha);
        shader->set_uniform("mat_line_shade", mp.line_shade);
        shader->set_uniform("mat_fiber_noise", mp.fiber_noise);

        const LayerInfo *layers = line_depth > 0.f && !v->is_wipe_tower ? layers_for(v->object_idx()) : nullptr;
        const int        mode   = layers != nullptr ? layers->mode : 0;
        shader->set_uniform("layer_mode", mode);
        if (layers != nullptr) {
            shader->set_uniform("first_layer_height", layers->first);
            shader->set_uniform("layer_height", layers->height);
            shader->set_uniform("layer_count", layers->count);
            shader->set_uniform("layer_table_width", layers->width);
            if (mode == 2) {
                glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_LAYER_UNIT));
                glsafe(::glBindTexture(GL_TEXTURE_2D, layers->texture));
                glsafe(::glActiveTexture(GL_TEXTURE0));
            }
        }

        const Transform3d world = v->world_matrix();
        shader->set_uniform("view_model_matrix", view * world);
        shader->set_uniform("projection_matrix", projection);
        shader->set_uniform("volume_world_matrix", world);
        shader->set_uniform("world_normal_matrix", Matrix3d(world.matrix().block<3, 3>(0, 0).inverse().transpose()));
        if (v->is_left_handed())
            glsafe(::glFrontFace(GL_CW));
        if (v->is_wipe_tower)
            v->render();
        else if (it.sub >= 0) {
            GLModel &m = v->mmuseg_models[size_t(it.sub)];
            m.set_color(it.color);
            m.render();
        } else {
            v->model.set_color(it.color);
            v->model.render();
        }
        if (v->is_left_handed())
            glsafe(::glFrontFace(GL_CCW));
    };

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));
    // A mirror flips the winding: draw both sides there.
    if (mirrored)
        glsafe(::glDisable(GL_CULL_FACE));
    else {
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glCullFace(GL_BACK));
    }
    for (const Item &it : opaque)
        draw(it);

    if (!translucent.empty()) {
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
        glsafe(::glDepthMask(GL_FALSE));
        glsafe(::glEnable(GL_CULL_FACE));
        for (const Item &it : translucent) {
            glsafe(::glCullFace(GL_FRONT));
            draw(it);
            glsafe(::glCullFace(GL_BACK));
            draw(it);
        }
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        glsafe(::glDisable(GL_BLEND));
    }
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glCullFace(GL_BACK));

    for (int unit : { SHOWCASE_SHADOW_MAP_UNIT, SHOWCASE_LAYER_UNIT }) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + unit));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    }
    glsafe(::glActiveTexture(GL_TEXTURE0 + SHOWCASE_ENV_UNIT));
    glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    shader->stop_using();
    return true;
}

namespace {

// The bound framebuffer and viewport, put back when the off-screen passes are done.
struct ShowcaseTargetState
{
    GLint fbo = 0;
    GLint viewport[4] = { 0, 0, 0, 0 };
    ShowcaseTargetState()
    {
        glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo));
        glsafe(::glGetIntegerv(GL_VIEWPORT, viewport));
    }
    ~ShowcaseTargetState()
    {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fbo)));
        glsafe(::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]));
    }
};

GLuint showcase_render_texture(int w, int h, bool mipmapped)
{
    GLuint tex = 0;
    glsafe(::glGenTextures(1, &tex));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    if (mipmapped)
        glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    return tex;
}

} // namespace

// Design 2.3: the objects seen from straight above, near-floor parts only, blurred. The floor reads
// it as the soft shade right under the model - which SSAO cannot give (the floor writes no depth),
// and which a transparent export keeps in its alpha.
void ShowcaseRenderer::render_contact_shadow()
{
    m_contact_valid = false;
    if (m_scene == nullptr || m_scene->floor.contact_shadow <= 0.f || !floor_drawn())
        return;
    GLShaderProgram *footprint = wxGetApp().get_shader("showcase_contact");
    GLShaderProgram *blur      = wxGetApp().get_shader("showcase_blur");
    if (footprint == nullptr || blur == nullptr)
        return;
    const BoundingBoxf3 box = objects_box();
    if (!box.defined)
        return;
    const double size   = std::max(box.size().x(), box.size().y());
    const double height = std::clamp(0.1 * std::max(size, box.size().z()), 1.5, 12.);
    const double margin = std::max(4. * height, 0.15 * size);
    m_contact_rect[0] = float(box.min.x() - margin);
    m_contact_rect[1] = float(box.min.y() - margin);
    m_contact_rect[2] = float(box.size().x() + 2. * margin);
    m_contact_rect[3] = float(box.size().y() + 2. * margin);

    const int n = SHOWCASE_CONTACT_SIZE;
    ShowcaseTargetState keep;
    if (m_contact_fbo == 0) {
        glsafe(::glGenFramebuffers(1, &m_contact_fbo));
        m_contact_tex[0] = showcase_render_texture(n, n, false);
        m_contact_tex[1] = showcase_render_texture(n, n, false);
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_contact_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_contact_tex[0], 0));
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, bufs));
    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return;
    glsafe(::glViewport(0, 0, n, n));
    glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendEquation(GL_MAX));
    glsafe(::glBlendFunc(GL_ONE, GL_ONE));

    // Orthographic from above over the rectangle; Z is kept whole (no depth test anyway).
    Matrix4d ortho = Matrix4d::Identity();
    ortho(0, 0) = 2. / m_contact_rect[2];
    ortho(1, 1) = 2. / m_contact_rect[3];
    ortho(2, 2) = -1e-4;
    ortho(0, 3) = -1. - 2. * m_contact_rect[0] / m_contact_rect[2];
    ortho(1, 3) = -1. - 2. * m_contact_rect[1] / m_contact_rect[3];
    footprint->start_using();
    footprint->set_uniform("projection_matrix", ortho);
    footprint->set_uniform("contact_height", float(height));
    for (GLVolume *v : m_canvas.m_volumes.volumes) {
        if (v == nullptr || !v->is_active || (v->is_modifier && !v->is_wipe_tower))
            continue;
        const Transform3d world = v->world_matrix();
        footprint->set_uniform("view_model_matrix", world);
        footprint->set_uniform("volume_world_matrix", world);
        footprint->set_uniform("world_normal_matrix", Matrix3d(Matrix3d::Identity()));
        v->model.render();
    }
    footprint->stop_using();
    glsafe(::glBlendEquation(GL_FUNC_ADD));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_BLEND));

    // Separable Gaussian, horizontally into the second texture, vertically back: about as wide as
    // the contact height.
    ensure_fullscreen_quad();
    const float spread = std::max(1.f, float(height / 4. / (double(m_contact_rect[2]) / n)));
    blur->start_using();
    blur->set_uniform("source", 0);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    for (int pass = 0; pass < 2; ++pass) {
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_contact_tex[1 - pass], 0));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_contact_tex[pass]));
        blur->set_uniform("direction", pass == 0 ? Vec2f(spread / float(n), 0.f) : Vec2f(0.f, spread / float(n)));
        m_canvas.m_background.render();
    }
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    blur->stop_using();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    m_contact_valid = true;
}

// The glossy floor (design 2.3): the scene's objects mirrored in Z = 0 at half the viewport's size,
// mipmapped so a rougher floor can read a blurrier level. Over transparent black, premultiplied.
void ShowcaseRenderer::render_reflection()
{
    m_refl_valid = false;
    if (m_scene == nullptr || m_scene->floor.reflection <= 0.f || !floor_drawn() || wxGetApp().get_shader("showcase_material") == nullptr)
        return;
    if (wxGetApp().plater()->get_camera().get_position().z() <= 0.)
        return;
    ShowcaseTargetState keep;
    const int w = std::max(1, keep.viewport[2] / 2);
    const int h = std::max(1, keep.viewport[3] / 2);
    if (m_refl_fbo == 0 || m_refl_w != w || m_refl_h != h) {
        if (m_refl_tex != 0)
            glsafe(::glDeleteTextures(1, &m_refl_tex));
        if (m_refl_depth != 0)
            glsafe(::glDeleteRenderbuffers(1, &m_refl_depth));
        if (m_refl_fbo == 0)
            glsafe(::glGenFramebuffers(1, &m_refl_fbo));
        m_refl_tex = showcase_render_texture(w, h, true);
        glsafe(::glGenRenderbuffers(1, &m_refl_depth));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, m_refl_depth));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
        m_refl_w = w;
        m_refl_h = h;
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_refl_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_refl_tex, 0));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_refl_depth));
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, bufs));
    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return;
    glsafe(::glViewport(0, 0, w, h));
    glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    if (!draw_objects_material(true))
        return;
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_refl_tex));
    glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    m_refl_valid = true;
}

void ShowcaseRenderer::draw_scene(const DrawTarget &t)
{
    Camera &camera = wxGetApp().plater()->get_camera();

    glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    update_visibility();
    m_canvas.set_render_settings_override(render_settings(t));
    m_canvas.m_shadow_map_valid = false;

    // 1. The backdrop.
    draw_background(t);

    const Transform3d &view = camera.get_view_matrix();
    const Transform3d &proj = camera.get_projection_matrix();

    // 2. The printer's plate ("Build plate" look), or nothing: the floor follows the shadows.
    const bool show_plate = m_scene == nullptr || !m_scene->floor.hide_plate;
    if (show_plate) {
        m_canvas._render_bed(view, proj, !camera.is_looking_downward(), false);
        m_canvas._render_platelist(view, proj, !camera.is_looking_downward(), true, true, -1, false, true);
    }

    // 3. The shadow map, with the light where the scene puts it (and its shadow on the plate).
    if (m_canvas._is_realistic_shadows_enabled())
        m_canvas._render_shadows(view, proj);

    // 4. The floor, receiving those shadows, with S2's contact shadow and mirror image when on.
    m_contact_valid = m_refl_valid = false;
    if (!show_plate) {
        render_contact_shadow();
        render_reflection();
        draw_floor(t);
    }

    // 5. The objects: the filament materials (S2), or S1's phong when that shader is missing.
    if (!draw_objects_material(false))
        draw_objects();

    // 6. Ambient occlusion over the finished colour and depth.
    if (t.run_ssao && m_canvas._is_ssao_enabled()) {
        ensure_fullscreen_quad();
        m_canvas._render_ssao_pass(unsigned(t.width), unsigned(t.height));
    }
}

void ShowcaseRenderer::ensure_watermark_texture(int frame_w, int frame_h)
{
    if (!m_watermark_provider || frame_w <= 0 || frame_h <= 0)
        return;
    if (m_wm_texture_version == m_watermark_version && m_wm_frame_w == frame_w && m_wm_frame_h == frame_h)
        return;
    m_wm_texture_version = m_watermark_version;
    m_wm_frame_w         = frame_w;
    m_wm_frame_h         = frame_h;
    ShowcaseImage mark;
    m_wm_rect = Showcase::RectI();
    if (!m_watermark_provider(frame_w, frame_h, mark, m_wm_rect) || mark.empty()) {
        m_wm_rect = Showcase::RectI();
        return;
    }
    if (m_wm_texture == 0)
        glsafe(::glGenTextures(1, &m_wm_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_wm_texture));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mark.width, mark.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, mark.rgba.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
}

// The safe frame: everything outside the export's aspect is dimmed, with the export size in the
// corner, and the watermark preview inside it. ImGui coordinates are the canvas' pixels.
void ShowcaseRenderer::draw_overlay(int vp_w, int vp_h)
{
    const Showcase::RectI f = m_live_frame;
    if (f.empty())
        return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (dl == nullptr)
        return;
    const ImU32 dim  = IM_COL32(0, 0, 0, 110);
    const float x0 = float(f.x), y0 = float(f.y), x1 = float(f.x + f.w), y1 = float(f.y + f.h);
    if (y0 > 0.f)
        dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(float(vp_w), y0), dim);
    if (y1 < float(vp_h))
        dl->AddRectFilled(ImVec2(0.f, y1), ImVec2(float(vp_w), float(vp_h)), dim);
    if (x0 > 0.f)
        dl->AddRectFilled(ImVec2(0.f, y0), ImVec2(x0, y1), dim);
    if (x1 < float(vp_w))
        dl->AddRectFilled(ImVec2(x1, y0), ImVec2(float(vp_w), y1), dim);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 140), 0.f, 0, 1.f);

    if (m_scene != nullptr) {
        ensure_watermark_texture(f.w, f.h);
        if (m_wm_texture != 0 && !m_wm_rect.empty() && m_scene->export_settings.watermark.enabled) {
            const float op = std::clamp(m_scene->export_settings.watermark.opacity, 0.f, 1.f);
            dl->AddImage((ImTextureID) (intptr_t) m_wm_texture, ImVec2(x0 + float(m_wm_rect.x), y0 + float(m_wm_rect.y)),
                         ImVec2(x0 + float(m_wm_rect.x + m_wm_rect.w), y0 + float(m_wm_rect.y + m_wm_rect.h)), ImVec2(0.f, 0.f),
                         ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, int(std::lround(op * 255.f))));
        }
        const std::string label = (boost::format("%1% x %2%") % m_scene->export_settings.width % m_scene->export_settings.height).str();
        const ImVec2      size  = ImGui::CalcTextSize(label.c_str());
        const ImVec2      pos(x1 - size.x - 8.f, y1 + 4.f <= float(vp_h) - size.y ? y1 + 4.f : y1 - size.y - 6.f);
        dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 160), label.c_str());
        dl->AddText(pos, IM_COL32(255, 255, 255, 220), label.c_str());
    }
}

// ------------------------------------------------------------------------------------------------
// The live frame
// ------------------------------------------------------------------------------------------------

void ShowcaseRenderer::render_frame()
{
    const Size         cnv  = m_canvas.get_canvas_size();
    const unsigned int vp_w = std::max(10u, (unsigned int) cnv.get_width());
    const unsigned int vp_h = std::max(10u, (unsigned int) cnv.get_height());
    m_canvas._resize(vp_w, vp_h);

    Camera &camera = wxGetApp().plater()->get_camera();
    camera.set_viewport(0, 0, vp_w, vp_h);
    camera.apply_viewport();

    const int ex_w = m_scene ? m_scene->export_settings.width : 16;
    const int ex_h = m_scene ? m_scene->export_settings.height : 9;
    m_live_vp_w    = int(vp_w);
    m_live_vp_h    = int(vp_h);
    m_live_frame   = Showcase::fit_frame(int(vp_w), int(vp_h), ex_w, ex_h, int(std::lround(0.04 * double(std::min(vp_w, vp_h)))));

    update_visibility();
    camera.apply_projection(scene_box());
    camera.UpdateFrustum();
    wxGetApp().imgui()->new_frame();

    DrawTarget t;
    t.width  = int(vp_w);
    t.height = int(vp_h);
    t.live   = true;
    if (!m_live_frame.empty()) {
        t.frame[0] = float(m_live_frame.x) / float(vp_w);
        t.frame[2] = float(m_live_frame.x + m_live_frame.w) / float(vp_w);
        t.frame[1] = 1.f - float(m_live_frame.y + m_live_frame.h) / float(vp_h);
        t.frame[3] = 1.f - float(m_live_frame.y) / float(vp_h);
    }
    draw_scene(t);
    draw_overlay(int(vp_w), int(vp_h));

    wxGetApp().imgui()->render();
    m_canvas.report_frame_gl_errors("frame");
    if (m_canvas._set_current()) {
        m_canvas.m_canvas->SwapBuffers();
        m_canvas.m_render_stats.increment_fps_counter();
    }
}

// ------------------------------------------------------------------------------------------------
// Placing the camera
// ------------------------------------------------------------------------------------------------

void ShowcaseRenderer::frame_model()
{
    update_visibility();
    BoundingBoxf3 box = objects_box();
    if (!box.defined)
        box = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_bounding_box();
    if (!box.defined)
        return;
    Camera &camera = wxGetApp().plater()->get_camera();
    // Fit into the safe frame rather than the whole canvas: zoom_to_box() measures the viewport.
    const std::array<int, 4> viewport = camera.get_viewport();
    if (!m_live_frame.empty())
        camera.set_viewport(0, 0, unsigned(m_live_frame.w), unsigned(m_live_frame.h));
    camera.zoom_to_box(box, 1.15);
    camera.set_viewport(viewport[0], viewport[1], unsigned(std::max(1, viewport[2])), unsigned(std::max(1, viewport[3])));
    m_canvas.set_as_dirty();
}

void ShowcaseRenderer::place_default_camera()
{
    update_visibility();
    BoundingBoxf3 box = objects_box();
    if (!box.defined)
        box = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_bounding_box();
    if (!box.defined)
        return;
    Camera     &camera = wxGetApp().plater()->get_camera();
    const Vec3d target = box.center();
    // A three-quarter view from the front right, a little above: the usual product angle.
    const Vec3d dir      = Vec3d(0.55, -0.75, 0.45).normalized();
    const double distance = std::max(Camera::DefaultDistance, 4.0 * box.radius());
    camera.look_at(target + dir * distance, target, Vec3d::UnitZ());
    frame_model();
}

// ------------------------------------------------------------------------------------------------
// Export
// ------------------------------------------------------------------------------------------------

namespace {

// Off-screen render targets of one export, freed on scope exit.
struct ShowcaseFramebuffers
{
    GLuint msaa_fbo = 0, msaa_color = 0, msaa_depth = 0;
    GLuint fbo = 0, color = 0, depth = 0;

    ~ShowcaseFramebuffers() { release(); }
    void release()
    {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        if (msaa_color) glsafe(::glDeleteRenderbuffers(1, &msaa_color));
        if (msaa_depth) glsafe(::glDeleteRenderbuffers(1, &msaa_depth));
        if (msaa_fbo) glsafe(::glDeleteFramebuffers(1, &msaa_fbo));
        if (color) glsafe(::glDeleteTextures(1, &color));
        if (depth) glsafe(::glDeleteRenderbuffers(1, &depth));
        if (fbo) glsafe(::glDeleteFramebuffers(1, &fbo));
        msaa_fbo = msaa_color = msaa_depth = fbo = color = depth = 0;
    }

    // Sized formats only: macOS core rejects the unsized ones (see the thumbnail path).
    bool create(int w, int h, int samples)
    {
        release();
        // Drain errors left by earlier code, so an out-of-memory below is this allocation's.
        for (int i = 0; i < 16 && ::glGetError() != GL_NO_ERROR; ++i) {}
        glsafe(::glGenFramebuffers(1, &fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        glsafe(::glGenTextures(1, &color));
        glsafe(::glBindTexture(GL_TEXTURE_2D, color));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0));
        glsafe(::glGenRenderbuffers(1, &depth));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth));
        const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
        glsafe(::glDrawBuffers(1, bufs));
        glsafe(::glReadBuffer(GL_COLOR_ATTACHMENT0));
        if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || ::glGetError() == GL_OUT_OF_MEMORY) {
            release();
            return false;
        }
        if (samples > 1) {
            glsafe(::glGenFramebuffers(1, &msaa_fbo));
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, msaa_fbo));
            glsafe(::glGenRenderbuffers(1, &msaa_color));
            glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, msaa_color));
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h));
            glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_color));
            glsafe(::glGenRenderbuffers(1, &msaa_depth));
            glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, msaa_depth));
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h));
            glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msaa_depth));
            glsafe(::glDrawBuffers(1, bufs));
            glsafe(::glReadBuffer(GL_COLOR_ATTACHMENT0));
            if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || ::glGetError() == GL_OUT_OF_MEMORY) {
                release();
                return false;
            }
        }
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
        return true;
    }
};

} // namespace

bool ShowcaseRenderer::render_export(ShowcaseImage &out, std::string &report)
{
    std::ostringstream log;
    out = ShowcaseImage();
    if (m_scene == nullptr || !is_supported()) {
        report = "Showcase export: this OpenGL context cannot render off-screen.";
        return false;
    }
    if (!m_canvas._set_current()) {
        report = "Showcase export: the canvas' OpenGL context could not be made current.";
        return false;
    }

    GLint max_samples = 0, max_tex = 0, max_rb = 0;
    GLint max_vp[2]   = { 0, 0 };
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex));
    glsafe(::glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb));
    glsafe(::glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp));
    int cap = std::max(64, max_tex);
    if (max_rb > 0) cap = std::min(cap, int(max_rb));
    if (max_vp[0] > 0) cap = std::min(cap, int(max_vp[0]));
    if (max_vp[1] > 0) cap = std::min(cap, int(max_vp[1]));

    const Showcase::ExportSettings &ex = m_scene->export_settings;
    const int  reference_h = !m_live_frame.empty() ? m_live_frame.h : 540;
    Showcase::ExportPlan plan = Showcase::plan_export(ex.width, ex.height, ex.quality, OpenGLManager::can_multisample() ? max_samples : 0,
                                                      reference_h, size_t(1) << 30, cap);
    log << "Showcase export " << plan.width << "x" << plan.height << ": " << plan.ssaa << "x supersampling ("
        << plan.internal_width << "x" << plan.internal_height << "), " << (plan.msaa > 1 ? std::to_string(plan.msaa) + "x MSAA" : "no MSAA")
        << ", SSAO radius x" << plan.ssao_radius_scale << (plan.degraded ? " (reduced to fit the size cap / memory budget)" : "");

    // Framebuffers, degrading on failure: MSAA first, then the supersampling.
    ShowcaseFramebuffers fb;
    for (;;) {
        if (fb.create(plan.internal_width, plan.internal_height, plan.msaa))
            break;
        if (plan.msaa > 1) {
            plan.msaa = 0;
            log << "; framebuffer refused, retrying without MSAA";
        } else if (plan.ssaa > 1) {
            --plan.ssaa;
            plan.internal_width  = plan.width * plan.ssaa;
            plan.internal_height = plan.height * plan.ssaa;
            plan.ssao_radius_scale = std::max(1.f, float(plan.internal_height) / float(std::max(1, reference_h)));
            log << "; framebuffer refused, retrying at " << plan.ssaa << "x";
        } else {
            log << "; no framebuffer could be created";
            report = log.str();
            BOOST_LOG_TRIVIAL(error) << report;
            return false;
        }
    }

    Camera      &camera = wxGetApp().plater()->get_camera();
    const Camera saved  = camera;

    // The export sees exactly the safe frame of the live view: crop the live frustum to it.
    update_visibility();
    const BoundingBoxf3 box  = scene_box();
    const int           lvw  = m_live_vp_w > 0 ? m_live_vp_w : plan.width;
    const int           lvh  = m_live_vp_h > 0 ? m_live_vp_h : plan.height;
    const Showcase::RectI frame = !m_live_frame.empty() ? m_live_frame : Showcase::RectI{ 0, 0, lvw, lvh };
    camera.set_viewport(0, 0, unsigned(lvw), unsigned(lvh));
    camera.apply_projection(box);
    const Matrix4d &p    = camera.get_projection_matrix().matrix();
    const bool      ortho = camera.get_type() == Camera::EType::Ortho;
    const double    near_z = camera.get_near_z(), far_z = camera.get_far_z();
    const double    half_w = ortho ? 1.0 / p(0, 0) : near_z / p(0, 0);
    const double    half_h = ortho ? 1.0 / p(1, 1) : near_z / p(1, 1);
    const double    l = -half_w + 2. * half_w * double(frame.x) / double(lvw);
    const double    r = -half_w + 2. * half_w * double(frame.x + frame.w) / double(lvw);
    const double    t = half_h - 2. * half_h * double(frame.y) / double(lvh);
    const double    b = half_h - 2. * half_h * double(frame.y + frame.h) / double(lvh);
    camera.set_viewport(0, 0, unsigned(plan.internal_width), unsigned(plan.internal_height));
    camera.apply_viewport();
    camera.apply_projection(l, r, b, t, near_z, far_z);
    camera.UpdateFrustum();

    DrawTarget dt;
    dt.width             = plan.internal_width;
    dt.height            = plan.internal_height;
    dt.live              = false;
    dt.run_ssao          = false;
    dt.ssao_radius_scale = plan.ssao_radius_scale;
    dt.shadow_map_size   = plan.shadow_map_size;

    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, plan.msaa > 1 ? fb.msaa_fbo : fb.fbo));
    glsafe(::glViewport(0, 0, plan.internal_width, plan.internal_height));
    draw_scene(dt);
    OpenGLManager::report_gl_errors("in a Showcase export's draw");

    if (plan.msaa > 1) {
        // Resolve colour and depth (SSAO reads both, and cannot read a multisampled buffer).
        glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.msaa_fbo));
        glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.fbo));
        glsafe(::glBlitFramebuffer(0, 0, plan.internal_width, plan.internal_height, 0, 0, plan.internal_width, plan.internal_height,
                                   GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST));
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo));
    if (m_canvas._is_ssao_enabled()) {
        ensure_fullscreen_quad();
        m_canvas._render_ssao_pass(unsigned(plan.internal_width), unsigned(plan.internal_height));
    }

    std::vector<uint8_t> pixels(size_t(plan.internal_width) * size_t(plan.internal_height) * 4);
    glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 4));
    glsafe(::glReadBuffer(GL_COLOR_ATTACHMENT0));
    glsafe(::glReadPixels(0, 0, plan.internal_width, plan.internal_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));
    OpenGLManager::report_gl_errors("in a Showcase export's read back");

    fb.release();
    // Design 6: no export-size buffers stay behind (the mirror was sized for the export).
    release_offscreen();
    camera = saved;
    camera.apply_viewport();
    // The SSAO copies were sized for the export; the next live frame sizes them back.
    m_canvas.set_as_dirty();

    out.width  = plan.width;
    out.height = plan.height;
    out.rgba   = Showcase::downsample_rgba(pixels, plan.internal_width, plan.internal_height, plan.ssaa);
    pixels.clear();
    pixels.shrink_to_fit();
    Showcase::flip_rows_rgba(out.rgba, out.width, out.height);
    if (m_scene->background.type == Showcase::BackgroundType::Transparent)
        Showcase::unpremultiply_rgba(out.rgba);
    else
        for (size_t i = 3; i < out.rgba.size(); i += 4)
            out.rgba[i] = 255;

    report = log.str();
    BOOST_LOG_TRIVIAL(info) << report;
    return true;
}

} // namespace GUI
} // namespace Slic3r
