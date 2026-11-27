#ifndef slic3r_GUI_FinBakeJob_hpp_
#define slic3r_GUI_FinBakeJob_hpp_

// "Bake fins": the background half of the action (ObjectList::bake_fins), modelled on StabilizerBakeJob.
//
// The fins are planned, sliced and meshed off the UI thread from the SLICED PrintObject, which the
// worker may read because the plate is sliced and the background process is idle (the caller checks both
// before queueing; the plate is invalidated only in finalize(), on the main thread). Everything that
// touches the Model happens in finalize(), under one undo snapshot.
//
// Credit: the technique is Slant3D's designed-in supports, as specified by Support Fins
// (gittrahan/support-fins, printfins.com, MIT); EdgeSlicer's own implementation.

#include "Job.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Support/FinBake.hpp"

#include <string>

namespace Slic3r {
class PrintObject;
namespace GUI {

class Plater;

class FinBakeJob : public Job
{
public:
    // `print_object` is only read on the worker thread; finalize() re-finds the source ModelObject by id,
    // so a model edited while the bake ran is never written through a stale pointer.
    FinBakeJob(Plater *plater, const PrintObject *print_object, ObjectID model_object_id, const FinBakeOptions &options,
               const std::string &object_name);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater            *m_plater       = nullptr;
    const PrintObject *m_print_object = nullptr;
    ObjectID           m_object_id;
    FinBakeOptions     m_options;
    std::string        m_name;

    FinBakeResult      m_result;
    std::string        m_error;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FinBakeJob_hpp_
