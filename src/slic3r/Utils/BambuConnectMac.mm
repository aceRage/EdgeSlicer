#import <AppKit/AppKit.h>

#include "BambuConnect.hpp"

namespace Slic3r {
namespace BambuConnect {

// The app LaunchServices would open bambu-connect:// with ("/Applications/Bambu Connect.app"), or an
// empty string when no app registers the scheme. NSWorkspace URLForApplicationToOpenURL: is the
// non-deprecated replacement for LSCopyDefaultHandlerForURLScheme.
std::string macos_url_handler_app_path()
{
    @autoreleasepool {
        NSURL *probe = [NSURL URLWithString:@"bambu-connect://import-file"];
        if (probe == nil)
            return {};
        NSURL *app = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:probe];
        if (app == nil || app.path == nil)
            return {};
        const char *utf8 = [app.path UTF8String];
        return utf8 != nullptr ? std::string(utf8) : std::string();
    }
}

} // namespace BambuConnect
} // namespace Slic3r
