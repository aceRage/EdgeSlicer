#ifndef slic3r_BRep_OcctException_hpp_
#define slic3r_BRep_OcctException_hpp_

// Type name and message of an OCCT exception, for error messages, on OCCT 7.x and 8.
// OCCT 8: Standard_Failure is a std::exception; what() is the message, ExceptionType() the type,
//         and GetMessageString() is deprecated (C4996), so it is not used.
// OCCT 7.x: Standard_Failure is not a std::exception and has no what(); the message is
//         GetMessageString() and the type is DynamicType()->Name().

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

// Never null; empty when OCCT gave no message.
inline const char* occt_exception_message(const Standard_Failure &e)
{
#if OCC_VERSION_HEX >= 0x080000
    const char *msg = e.what();
#else
    const char *msg = e.GetMessageString();
#endif
    return msg != nullptr ? msg : "";
}

} // namespace Slic3r

#endif // slic3r_BRep_OcctException_hpp_
