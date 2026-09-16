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

#endif // SLIC3R_TEST_UTILS
