#ifndef slic3r_Format_STL_hpp_
#define slic3r_Format_STL_hpp_

#include <admesh/stl.h>

#include <string>

namespace Slic3r {

class Model;
class TriangleMesh;
class ModelObject;

// Load an STL file into a provided model.
extern bool load_stl(const char *path, Model *model, const char *object_name = nullptr, ImportstlProgressFn stlFn = nullptr, int custom_header_length = 80);

extern bool store_stl(const char *path, TriangleMesh *mesh, bool binary);
extern bool store_stl(const char *path, ModelObject *model_object, bool binary);
extern bool store_stl(const char *path, Model *model, bool binary);

// "Export all objects as STLs": the file in `dir` (which ends in a separator) for an object called
// `object_name`. The name is made safe for a file name (sanitize_filename; "object" when nothing is
// left), and a file that already exists gets "(1)", "(2)"... added, as before.
extern std::string stl_export_file_path(const std::string &dir, const std::string &object_name);

}; // namespace Slic3r

#endif /* slic3r_Format_STL_hpp_ */
