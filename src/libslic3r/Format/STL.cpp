#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../Utils.hpp"

#include "STL.hpp"

#include <string>

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

namespace Slic3r {

bool load_stl(const char *path, Model *model, const char *object_name_in, ImportstlProgressFn stlFn, int custom_header_length)
{
    TriangleMesh mesh;
    std::string design_id;

    if (!mesh.ReadSTLFile(path, true, stlFn, custom_header_length)) {
        //    die "Failed to open $file\n" if !-e $path;
        return false;
    }
    if (mesh.empty()) {
        // die "This STL file couldn't be read because it's empty.\n"
        return false;
    }

    std::string object_name;
    if (object_name_in == nullptr) {
        const char *last_slash = strrchr(path, DIR_SEPARATOR);
        object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
    } else
       object_name.assign(object_name_in);

    model->add_object(object_name.c_str(), path, std::move(mesh));
    return true;
}

bool store_stl(const char *path, TriangleMesh *mesh, bool binary)
{
    // Written beside the target and renamed over it once complete, so a failed write (a full disk, a
    // name the file system refuses) leaves no partial file and keeps a file that was there before.
    const boost::filesystem::path target(path);
    boost::filesystem::path       temp = target;
    temp += ".tmp";
    const std::string temp_name = temp.string();
    const bool        written   = binary ? mesh->write_binary(temp_name.c_str()) : mesh->write_ascii(temp_name.c_str());
    boost::system::error_code ec;
    if (written)
        boost::filesystem::rename(temp, target, ec);
    if (!written || ec) {
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "store_stl: Couldn't rename " << temp_name << " to " << path << ": " << ec.message();
        boost::system::error_code ec_remove;
        if (boost::filesystem::is_regular_file(temp, ec_remove))
            boost::filesystem::remove(temp, ec_remove);
        return false;
    }
    return true;
}

std::string stl_export_file_path(const std::string &dir, const std::string &object_name)
{
    std::string name = sanitize_filename(object_name);
    boost::trim(name);
    if (name.empty())
        name = "object";
    std::string path = dir + name + ".stl";
    int         n    = 1;
    while (boost::filesystem::exists(path))
        path = dir + name + "(" + std::to_string(n++) + ").stl";
    return path;
}

bool store_stl(const char *path, ModelObject *model_object, bool binary)
{
    TriangleMesh mesh = model_object->mesh();
    return store_stl(path, &mesh, binary);
}

bool store_stl(const char *path, Model *model, bool binary)
{
    TriangleMesh mesh = model->mesh();
    return store_stl(path, &mesh, binary);
}

}; // namespace Slic3r