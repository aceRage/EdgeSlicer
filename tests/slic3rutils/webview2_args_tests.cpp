#include <catch2/catch.hpp>

#include "slic3r/Utils/WebView2Args.hpp"

using namespace Slic3r::WebView2Args;

// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS (WebView2Args.hpp): disable HideCursorWhileTyping without
// replacing an already-set value (user, Stream/hub, a future Edge path). Leave the string alone
// when that feature is already listed.

TEST_CASE("webview2 args: unset or blank becomes the disable-features flag", "[WebView2Args]")
{
    CHECK(with_hide_cursor_while_typing(nullptr) == FLAG);
    CHECK(with_hide_cursor_while_typing("") == FLAG);
    CHECK(with_hide_cursor_while_typing("   ") == FLAG);
    CHECK(with_hide_cursor_while_typing("\t") == FLAG);
}

TEST_CASE("webview2 args: leave alone when HideCursorWhileTyping is already disabled", "[WebView2Args]")
{
    CHECK(with_hide_cursor_while_typing(FLAG) == FLAG);
    CHECK(with_hide_cursor_while_typing("--disable-features=HideCursorWhileTyping --lang=en-US") ==
          "--disable-features=HideCursorWhileTyping --lang=en-US");
    CHECK(with_hide_cursor_while_typing("--lang=en-US --disable-features=HideCursorWhileTyping") ==
          "--lang=en-US --disable-features=HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo,HideCursorWhileTyping,Bar") ==
          "--disable-features=Foo,HideCursorWhileTyping,Bar");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo, HideCursorWhileTyping") ==
          "--disable-features=Foo, HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=\"Foo,HideCursorWhileTyping\"") ==
          "--disable-features=\"Foo,HideCursorWhileTyping\"");
}

TEST_CASE("webview2 args: append the flag when the env is already set", "[WebView2Args]")
{
    CHECK(with_hide_cursor_while_typing("--enable-logging") == "--enable-logging --disable-features=HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--enable-logging ") == "--enable-logging --disable-features=HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo") == "--disable-features=Foo,HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo, Bar") == "--disable-features=Foo, Bar,HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo --lang=en-US") ==
          "--disable-features=Foo,HideCursorWhileTyping --lang=en-US");
    CHECK(with_hide_cursor_while_typing("--lang=en-US --disable-features=Foo") ==
          "--lang=en-US --disable-features=Foo,HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=") == "--disable-features=HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=Foo,") == "--disable-features=Foo,HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=\"Foo,Bar\"") == "--disable-features=\"Foo,Bar,HideCursorWhileTyping\"");
}

TEST_CASE("webview2 args: HideCursorWhileTyping is a feature token, not a substring", "[WebView2Args]")
{
    CHECK(with_hide_cursor_while_typing("--disable-features=NotHideCursorWhileTyping") ==
          "--disable-features=NotHideCursorWhileTyping,HideCursorWhileTyping");
    CHECK(with_hide_cursor_while_typing("--disable-features=HideCursorWhileTypingExtra") ==
          "--disable-features=HideCursorWhileTypingExtra,HideCursorWhileTyping");
    // Enabling the feature is not the same as already disabling it.
    CHECK(with_hide_cursor_while_typing("--enable-features=HideCursorWhileTyping") ==
          "--enable-features=HideCursorWhileTyping --disable-features=HideCursorWhileTyping");
}

TEST_CASE("webview2 args: feature_in_list matches a comma token", "[WebView2Args]")
{
    CHECK(feature_in_list("HideCursorWhileTyping", FEATURE));
    CHECK(feature_in_list("Foo,HideCursorWhileTyping,Bar", FEATURE));
    CHECK(feature_in_list("Foo, HideCursorWhileTyping", FEATURE));
    CHECK_FALSE(feature_in_list("NotHideCursorWhileTyping", FEATURE));
    CHECK_FALSE(feature_in_list("HideCursorWhileTypingExtra", FEATURE));
    CHECK_FALSE(feature_in_list("", FEATURE));
}
