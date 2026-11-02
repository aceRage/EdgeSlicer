#ifndef SLIC3R_TEST_UTILS
#define SLIC3R_TEST_UTILS

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Format/OBJ.hpp>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#if defined(WIN32) || defined(_WIN32)
#define PATH_SEPARATOR R"(\)"
#else
#define PATH_SEPARATOR R"(/)"
#endif

inline Slic3r::TriangleMesh load_model(const std::string &obj_filename)
{
    Slic3r::TriangleMesh mesh;
    auto fpath = TEST_DATA_DIR PATH_SEPARATOR + obj_filename;
    Slic3r::ObjInfo obj_info;
    std::string message;
    Slic3r::load_obj(fpath.c_str(), &mesh, obj_info, message);
    return mesh;
}

// Changes the working directory and restores the previous one on scope exit, including when an
// assertion throws. It is process wide state shared with every other test.
class ScopedWorkingDirectory
{
public:
    explicit ScopedWorkingDirectory(const boost::filesystem::path &dir)
        : m_previous(boost::filesystem::current_path())
    {
        boost::filesystem::current_path(dir);
    }
    ~ScopedWorkingDirectory()
    {
        boost::system::error_code ec;
        boost::filesystem::current_path(m_previous, ec);
    }
    ScopedWorkingDirectory(const ScopedWorkingDirectory &)            = delete;
    ScopedWorkingDirectory &operator=(const ScopedWorkingDirectory &) = delete;

private:
    boost::filesystem::path m_previous;
};

// Owns a unique path under the system temp dir, "<prefix>-<unique>[<extension>]"
// (parallel-safe, cross-platform). Shared base for the RAII temp guards below.
class ScopedTemporaryPath
{
public:
    const boost::filesystem::path &path() const { return m_path; }
    std::string string() const { return m_path.string(); }
    ScopedTemporaryPath(const ScopedTemporaryPath &) = delete;
    ScopedTemporaryPath &operator=(const ScopedTemporaryPath &) = delete;

protected:
    ScopedTemporaryPath(const std::string &prefix, const std::string &extension)
        : m_path(boost::filesystem::temp_directory_path()
                 / boost::filesystem::unique_path(prefix + "-%%%%-%%%%-%%%%" + extension))
    {}
    ~ScopedTemporaryPath() = default;

    boost::filesystem::path m_path;
};

// A temp file the caller creates by writing to path()/string(); the guard only
// reserves the name and removes the file on scope exit.
class ScopedTemporaryFile : public ScopedTemporaryPath
{
public:
    explicit ScopedTemporaryFile(const std::string &extension = ".tmp")
        : ScopedTemporaryPath("slic3r", extension) {}
    ~ScopedTemporaryFile() { boost::system::error_code ec; boost::filesystem::remove(m_path, ec); }
};

#endif // SLIC3R_TEST_UTILS
