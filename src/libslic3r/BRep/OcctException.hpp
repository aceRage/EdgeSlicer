#ifndef slic3r_BRep_OcctException_hpp_
#define slic3r_BRep_OcctException_hpp_

// The type name of an OCCT exception, for error messages. OCCT 8 makes Standard_Failure a plain
// std::exception with ExceptionType() and no DynamicType(); OCCT 7.x has only DynamicType().
// The message itself comes from what(), which both versions have.

#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>

namespace Slic3r {

inline const char* occt_exception_type(const Standard_Failure &e)
{
#if OCC_VERSION_HEX >= 0x080000
    return e.ExceptionType();
#else
    return e.DynamicType()->Name();
#endif
}

} // namespace Slic3r

#endif // slic3r_BRep_OcctException_hpp_
