#ifndef slic3r_GLShader_hpp_
#define slic3r_GLShader_hpp_

#include <array>
#include <initializer_list>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class ColorRGB;
class ColorRGBA;

class GLShaderProgram
{
public:
    enum class EShaderType
    {
        Vertex,
        Fragment,
        Geometry,
        TessEvaluation,
        TessControl,
        Compute,
        Count
    };

    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderFilenames;
    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderSources;

private:
    std::string m_name;
    unsigned int m_id{ 0 };
    std::vector<std::pair<std::string, int>> m_attrib_location_cache;
    std::vector<std::pair<std::string, int>> m_uniform_location_cache;

public:
    // An active sampler uniform of the linked program (see get_samplers()).
    struct SamplerUniform
    {
        std::string  name;
        unsigned int type{ 0 };     // GL_SAMPLER_2D, GL_SAMPLER_3D, GL_SAMPLER_BUFFER, ...
        int          location{ -1 };
    };

private:
    mutable bool                        m_samplers_listed{ false };
    mutable std::vector<SamplerUniform> m_samplers;
    // EDGE: (unit, texture target) of the samplers set_sampler_units() parked on their own units.
    // start_using() binds OpenGLManager's 1x1 fallback texture there, so a draw that does not use
    // the feature behind the sampler (gouraud's curved/drawn cut) never samples texture 0.
    mutable std::vector<std::pair<int, unsigned int>> m_parked_units;

    void bind_parked_fallback_textures() const;

public:
    ~GLShaderProgram();

    bool init_from_files(const std::string& name, const ShaderFilenames& filenames, const std::initializer_list<std::string_view> &defines = {});
    bool init_from_texts(const std::string& name, const ShaderSources& sources);

    const std::string& get_name() const { return m_name; }
    unsigned int get_id() const { return m_id; }

    void start_using() const;
    void stop_using() const;

    // Sets sampler uniforms to fixed texture units once, outside any draw. The program is bound
    // for the duration and the previously bound program is restored. A uniform the linker
    // removed (inactive) is skipped. From then on start_using() binds a complete 1x1 fallback
    // texture to each of these units (of the sampler's type); the caller binds its own texture
    // there after start_using() when the feature is on.
    void set_sampler_units(std::initializer_list<std::pair<const char*, int>> units) const;
    // The program's active sampler uniforms, listed once after linking (needs the context).
    const std::vector<SamplerUniform>& get_samplers() const;
    // GL_TEXTURE_2D / GL_TEXTURE_3D for the 2D / 3D sampler types (float, int, unsigned), 0 otherwise.
    static unsigned int sampler_target(unsigned int sampler_type);
    // Returns a description of every pair of active samplers of DIFFERENT types that read the same
    // texture unit, empty if there is none. Such a program is invalid at draw time (GL spec:
    // GL_INVALID_OPERATION at the next draw); Windows drivers tolerate it, macOS does not and
    // skips the draw entirely.
    std::string sampler_unit_conflicts() const;

    void set_uniform(const char* name, int value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, bool value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, float value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, double value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<double, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const float* value, size_t size) const { set_uniform(get_uniform_location(name), value, size); }
    void set_uniform(const char* name, const Transform3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Transform3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGB& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGBA& value) const { set_uniform(get_uniform_location(name), value); }

    void set_uniform(int id, int value) const;
    void set_uniform(int id, bool value) const;
    void set_uniform(int id, float value) const;
    void set_uniform(int id, double value) const;
    void set_uniform(int id, const std::array<int, 2>& value) const;
    void set_uniform(int id, const std::array<int, 3>& value) const;
    void set_uniform(int id, const std::array<int, 4>& value) const;
    void set_uniform(int id, const std::array<float, 2>& value) const;
    void set_uniform(int id, const std::array<float, 3>& value) const;
    void set_uniform(int id, const std::array<float, 4>& value) const;
    void set_uniform(int id, const std::array<double, 4>& value) const;
    void set_uniform(int id, const float* value, size_t size) const;
    void set_uniform(int id, const Transform3f& value) const;
    void set_uniform(int id, const Transform3d& value) const;
    void set_uniform(int id, const Matrix3f& value) const;
    void set_uniform(int id, const Matrix3d& value) const;
    void set_uniform(int id, const Matrix4f& value) const;
    void set_uniform(int id, const Matrix4d& value) const;
    void set_uniform(int id, const Vec2f& value) const;
    void set_uniform(int id, const Vec2d& value) const;
    void set_uniform(int id, const Vec3f& value) const;
    void set_uniform(int id, const Vec3d& value) const;
    void set_uniform(int id, const ColorRGB& value) const;
    void set_uniform(int id, const ColorRGBA& value) const;

    // returns -1 if not found
    int get_attrib_location(const char* name) const;
    // returns -1 if not found
    int get_uniform_location(const char* name) const;
};

} // namespace Slic3r

#endif /* slic3r_GLShader_hpp_ */
