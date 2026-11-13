#pragma once

// GUI-free lexer and checker for custom G-code templates (machine start/end G-code, filament G-code,
// layer change G-code ...): the text the PlaceholderParser expands at slice time.
//
// analyze() splits a template into styled spans for a syntax-highlighting editor and reports problems
// the PlaceholderParser would otherwise only report at slice time:
//   - unknown placeholder names, in {expressions} and in [legacy] placeholders (warnings: a name can
//     also come from a runtime value this list does not know about);
//   - a '{' without '}', a stray '}', a malformed '[...]', an unterminated string, unbalanced
//     ( ) / [ ] inside an expression, {if} without {endif}, {else}/{elsif}/{endif} without {if}.
//
// Offsets are byte offsets into the UTF-8 text (what wxStyledTextCtrl uses).
// The checker never changes the text and never blocks anything; the editor shows what it finds.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Slic3r {
namespace GCodeLint {

enum class Style : uint8_t {
    Default = 0, // plain text, whitespace
    Command,     // G1, M104, T0, or a firmware macro name at the start of a line (PRINT_START)
    Param,       // X10.5, S200, E-2, EXTRUDER= ...
    Comment,     // ; to the end of the line
    Brace,       // { } around an expression, [ ] around a legacy placeholder
    Keyword,     // if elsif else endif then and or not true false min max ... (macro language)
    Variable,    // a placeholder name
    Number,      // numeric literal (inside an expression or as a parameter value)
    String,      // "string" or /regex/ inside an expression
    Operator,    // + - * / == != < > ( ) [ ] , ? : ...
    Count
};

enum class Severity : uint8_t { Warning, Error };

enum class Code : uint8_t {
    UnknownPlaceholder, // warning; `name` holds the name
    UnclosedBrace,      // '{' without matching '}'
    StrayClosingBrace,  // '}' outside an expression
    MalformedLegacy,    // '[' that does not start [name] or [name[index]]
    UnclosedString,     // string or regex literal without its closing quote
    UnbalancedBracket,  // ( ) or [ ] mismatch inside an expression
    IfWithoutEndif,     // {if} (or an inline `if ... then`) never closed by endif
    ElseWithoutIf,      // else / elsif with no open if
    EndifWithoutIf,     // endif with no open if
};

struct Span
{
    size_t start;
    size_t length;
    Style  style;
};

struct Issue
{
    size_t      start;  // byte offset
    size_t      length; // bytes (>= 1)
    int         line;   // 1-based line number of `start`
    Severity    severity;
    Code        code;
    std::string name;   // the placeholder name for UnknownPlaceholder, the keyword for if/else codes
};

struct Result
{
    std::vector<Span>  spans;  // sorted by start, not overlapping; gaps are Style::Default
    std::vector<Issue> issues; // sorted by start
    size_t errors() const;
    size_t warnings() const;
};

// Returns true when `name` is a placeholder the template may use.
using KnownFn = std::function<bool(const std::string &name)>;

// Lex and check `text`. A null `is_known` skips the unknown-placeholder check.
Result analyze(const std::string &text, const KnownFn &is_known);

// Every placeholder name the slicer can provide to a custom G-code template, without a GUI:
// all print/filament/printer config keys, the placeholder groups the G-code editor dialog lists,
// every G-code specific placeholder, and values the G-code generator sets at runtime.
const std::unordered_set<std::string> &known_placeholder_names();

// The macro language's reserved words (if, endif, min, one_of ...).
bool is_keyword(const std::string &word);

} // namespace GCodeLint
} // namespace Slic3r
