#ifndef slic3r_CustomModels_hpp_
#define slic3r_CustomModels_hpp_

// "Add Custom Models": the user's own library of frequently used models, a folder of STL / OBJ /
// STEP files and single-plate 3MFs that can be dropped onto the plate from the right-click menu.
//
// Everything here is plain C++ (no wx) so the rules can be unit tested on their own: how the
// library folder is listed and sorted, what a name typed for "Save selected object to Custom
// Models" becomes on disk, how the selection is written to a 3MF, and what is done to the objects
// of a library 3MF before they join the current project.
//
// A saved 3MF carries the objects' geometry, parts / modifiers / negative volumes, per-object and
// per-part setting overrides, layer ranges, painted data (supports, seams, fuzzy skin, colours)
// and filament assignment. It carries no printer, filament or process preset: the project settings
// of the file are written empty.

#include <cstddef>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

class ModelObject;

namespace custom_models {

// "custom_models", the folder inside the data directory.
const char *library_folder_name();
// <data_dir>/custom_models
boost::filesystem::path library_dir(const std::string &data_dir);

// What a library file is, decided by its extension (case-insensitive).
enum class FileType {
    Unsupported,
    Mesh,        // .stl .obj .step .stp: added like a handy model
    Project3mf,  // .3mf: its objects come with their object / part settings
};
FileType file_type(const boost::filesystem::path &path);

struct Entry
{
    std::string             label; // file name without extension (the extension is added back only to tell two files of one name apart)
    boost::filesystem::path path;
    FileType                type = FileType::Unsupported;
};

struct Folder
{
    std::string             label; // directory name; empty for the root
    boost::filesystem::path path;
    std::vector<Folder>     folders; // sorted; only folders that hold at least one model, at any depth
    std::vector<Entry>      files;   // sorted
    // Set on the root when the scan stopped at the file limit; the folder holds more models.
    bool                    truncated = false;

    std::size_t file_count() const; // files at any depth
    bool        empty() const { return file_count() == 0; }
};

struct ScanLimits
{
    std::size_t max_files = 400; // models listed, over the whole tree
    int         max_depth = 4;   // sub-folder levels below the root
};

// Lists the library below `root`: files with a supported extension, sub-folders as sub-folders.
// Sorted case-insensitively with numbers compared as numbers ("part 2" before "part 10"), folders
// separately from files. Hidden entries (a leading '.') and the library's own scratch files are
// skipped. A missing or unreadable root gives an empty Folder.
Folder scan_library(const boost::filesystem::path &root, const ScanLimits &limits = ScanLimits());

// Case-insensitive natural order, the order the menu uses. Equal under the case-folded
// comparison falls back to the raw bytes so the order is total and stable.
bool natural_less(const std::string &a, const std::string &b);

// The file name (with ".3mf") a name typed in "Save selected object to Custom Models" is saved
// under, or "" when nothing usable is left. Only the last path component survives, reserved and
// control characters become '_', Windows device names are defused (untrusted::sanitize_download_filename),
// and a ".3mf" the user typed is not doubled.
std::string library_file_name(const std::string &typed_name);

struct SaveResult
{
    bool        ok = false;
    std::string error;
};

// One object of the plate and the one instance of it that is saved.
struct SourceObject
{
    const ModelObject *object       = nullptr;
    std::size_t        instance_idx = 0;
};

// Writes the objects (one instance of each, moved together so the group is centred on the
// origin) to `file` as a single-plate 3MF with no printer, filament or process settings. The
// file is written beside the target first and moved into place, so a failed save leaves an
// existing file of that name untouched. Needs Slic3r::set_temporary_dir() to have been called, as
// every project export does. `file`'s folder is created when missing.
SaveResult save_objects(const std::vector<SourceObject> &objects, const boost::filesystem::path &file);

struct ImportReport
{
    // Keys of per-object / per-part / per-layer-range settings removed because a file we did not
    // write may not set them (post-processing scripts, an output name leaving the output folder,
    // print host endpoints and credentials, remote bed files).
    std::size_t untrusted_settings_removed = 0;
    // Per-object / per-part filament settings (extruder, wall_filament, ...) that named a
    // filament the project does not have and now name filament 1.
    std::size_t filament_settings_clamped = 0;
    // Volumes whose painted colours used a filament the project does not have; those regions are
    // now filament 1.
    std::size_t painted_volumes_clamped = 0;
    // The highest filament number the objects asked for, before clamping (0 when none).
    int         highest_filament_requested = 0;

    bool filaments_clamped() const { return filament_settings_clamped > 0 || painted_volumes_clamped > 0; }
};

// What a library 3MF's objects go through before they are added to the current project:
// untrusted settings removed, filament numbers above `filament_count` set to 1.
// Everything else (settings overrides, modifiers, paint) is left as the file has it.
ImportReport prepare_imported_objects(const std::vector<ModelObject *> &objects, std::size_t filament_count);

// The three ways a project 3MF can be loaded into the plate (the "Drop project file" question, the
// Load Behaviour preference). Only the third one is new; the first two keep their meaning.
enum class ProjectImportMode {
    OpenAsProject,              // the file's printer / filament / process settings and its objects
    GeometryOnly,               // objects only: their object / part settings are reset (extruder kept)
    GeometryWithObjectSettings, // objects with their object / part / modifier settings, scrubbed
};

// The value kept in the app config key "import_project_action". 1 and 2 are the two original
// choices and keep their numbers (3 was LoadType::LoadConfig and is never stored by the dialog; an
// older build reading 4 falls back to "geometry only"). An empty, unreadable or other value is
// read as the dialog read it before: below 2 is "open as project", anything else "geometry only".
ProjectImportMode project_import_mode_from_action(const std::string &stored);
std::string       project_import_action_value(ProjectImportMode mode);

// What happens to the objects of a project 3MF that is added to the current project in `mode`.
// OpenAsProject leaves them as they are; GeometryOnly resets the object / part settings except
// the extruder; GeometryWithObjectSettings keeps them and runs prepare_imported_objects. The
// report is empty unless settings were kept.
ImportReport apply_project_import_mode(const std::vector<ModelObject *> &objects, ProjectImportMode mode, std::size_t filament_count);

// File > Import / Add of a project 3MF adds it to the current project, so "Open as project" is not on
// offer there; what is asked is how its objects come in. The question is only worth asking when a
// file actually carries per-object / per-part / modifier / height-range settings.
// True when the 3MF at `path` holds any (a setting is a key of the print configuration set in the
// object or part settings, or an entry in the height-range settings; the extruder alone does not count).
// A file that cannot be read or is no 3MF gives false.
bool project_3mf_has_object_settings(const boost::filesystem::path &path);

struct ImportChoice
{
    bool              ask          = false;                         // show the question
    ProjectImportMode mode         = ProjectImportMode::GeometryOnly; // used when not asking
    ProjectImportMode default_mode = ProjectImportMode::GeometryOnly; // pre-selected when asking
};
// `load_behaviour` is the Preferences > Load Behaviour value, `last_action` the stored
// "import_project_action". Not asked: no 3MF with settings in the batch (geometry only, as before);
// a preference that fixes the behaviour ("load_geometry_only", "load_geometry_with_settings", and "load_all",
// which cannot open a project here so adds the geometry as before). Asked once for the batch otherwise
// ("ask_when_relevant", "always_ask", or an empty / unknown value).
ImportChoice decide_import_project_choice(const std::string &load_behaviour, const std::string &last_action, bool any_3mf_has_object_settings);

// The per-object / per-part keys that hold a filament number (extruder, wall_filament, ...).
const std::vector<std::string> &filament_index_keys();

} // namespace custom_models
} // namespace Slic3r

#endif // slic3r_CustomModels_hpp_
