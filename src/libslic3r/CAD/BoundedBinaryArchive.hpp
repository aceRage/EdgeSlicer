#ifndef slic3r_CAD_BoundedBinaryArchive_hpp_
#define slic3r_CAD_BoundedBinaryArchive_hpp_

// A cereal binary INPUT archive that knows how many bytes its stream holds, and refuses any
// size tag (std::string, std::vector, std::map, ...) that claims more elements than there are
// bytes left. Every element of every container in a recipe costs at least one byte on the wire,
// so "size <= remaining bytes" is a bound no valid recipe can break, and it is checked BEFORE
// the container resizes - a corrupt length can no longer ask for terabytes.
//
// Byte-for-byte the same format as cereal::BinaryInputArchive, so it reads everything
// cereal::BinaryOutputArchive writes.

#include <cereal/cereal.hpp>
#include <cereal/archives/binary.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Slic3r {

// Thrown when a length or count in a recipe cannot be satisfied by the bytes that are left.
// It is a CORRUPTION signal: the recipe reader lets it fail the whole load instead of treating
// it like the end of an older, shorter record.
struct RecipeLengthError : std::runtime_error
{
    explicit RecipeLengthError(const char* what = "length field exceeds the remaining data")
        : std::runtime_error(what) {}
};

// Hard ceiling on the number of features in one recipe, far above any real design.
constexpr uint64_t kMaxRecipeFeatures = 100000;
// Lower bound on the bytes of one serialized feature. A feature writes well over a hundred
// fields (a default one is several hundred bytes; a test pins that), so 64 is conservative for
// every recipe version, and it lets a count be checked against the bytes actually present.
constexpr uint64_t kMinFeatureRecordBytes = 64;

class BoundedBinaryInputArchive
    : public cereal::InputArchive<BoundedBinaryInputArchive, cereal::AllowEmptyClassElision>
{
public:
    // parse_only: the framed/v4 feature loaders still read the Import BRep *string*, but they
    // skip BRepTools::Read. validate_recipe_bytes uses that so untrusted 3MF bytes cannot drive
    // OCCT. deserialize_recipe constructs archives with parse_only=false and restores shapes.
    BoundedBinaryInputArchive(std::istream& stream, uint64_t total_bytes, bool parse_only = false)
        : cereal::InputArchive<BoundedBinaryInputArchive, cereal::AllowEmptyClassElision>(this)
        , m_stream(stream), m_total(total_bytes), m_parse_only(parse_only) {}

    void loadBinary(void* const data, std::streamsize size)
    {
        const auto read = m_stream.rdbuf()->sgetn(reinterpret_cast<char*>(data), size);
        if (read > 0)
            m_pos += uint64_t(read);
        if (read != size)
            throw cereal::Exception("Failed to read " + std::to_string(size) + " bytes from input stream! Read " +
                                    std::to_string(read));
    }

    uint64_t remaining() const { return m_pos >= m_total ? 0 : m_total - m_pos; }

    // Throws RecipeLengthError unless n elements of at least min_bytes_each fit in what is left.
    void check_len(uint64_t n, uint64_t min_bytes_each = 1) const
    {
        if (n > remaining() / min_bytes_each)
            throw RecipeLengthError();
    }

    bool parse_only() const { return m_parse_only; }

private:
    std::istream& m_stream;
    uint64_t      m_total;
    uint64_t      m_pos{0};
    bool          m_parse_only{false};
};

template<class T>
inline typename std::enable_if<std::is_arithmetic<T>::value, void>::type
CEREAL_LOAD_FUNCTION_NAME(BoundedBinaryInputArchive& ar, T& t)
{
    ar.loadBinary(std::addressof(t), sizeof(t));
}

template<class T>
inline void CEREAL_SERIALIZE_FUNCTION_NAME(BoundedBinaryInputArchive& ar, cereal::NameValuePair<T>& t)
{
    ar(t.value);
}

// The one place every container learns its size: check it before the container acts on it.
// This is the 1-byte-per-element floor; it does not know the element type. std::vector, the one
// container here that allocates all its elements before reading them, gets its own load below,
// which accounts for sizeof(T).
template<class T>
inline void CEREAL_SERIALIZE_FUNCTION_NAME(BoundedBinaryInputArchive& ar, cereal::SizeTag<T>& t)
{
    ar(t.size);
    ar.check_len(static_cast<uint64_t>(t.size));
}

// std::vector for this archive (more specialised than cereal's generic vector loads, so it wins
// overload resolution; same wire format). cereal resizes to the declared count first, i.e.
// allocates count * sizeof(T) while the size tag was only checked at one byte per element: a
// corrupt count could ask for 8x (double) to 24x (Vec3d) the bytes the blob holds.
//  - arithmetic T: every element is exactly sizeof(T) bytes on the wire, so the count is checked
//    at sizeof(T) each before the resize;
//  - any other T (strings, structs): the wire size of an element is not known up front, so the
//    vector grows as elements actually arrive and a lying count fails at the end of the data,
//    having allocated only for what was really there.
template<class T, class A>
inline typename std::enable_if<!std::is_same<T, bool>::value, void>::type
CEREAL_LOAD_FUNCTION_NAME(BoundedBinaryInputArchive& ar, std::vector<T, A>& vec)
{
    cereal::size_type n = 0;
    ar(cereal::make_size_tag(n));
    if constexpr (std::is_arithmetic<T>::value) {
        ar.check_len(static_cast<uint64_t>(n), sizeof(T));
        vec.resize(static_cast<std::size_t>(n));
        ar(cereal::binary_data(vec.data(), static_cast<std::size_t>(n) * sizeof(T)));
    } else {
        vec.clear();
        vec.reserve(static_cast<std::size_t>(std::min<uint64_t>(static_cast<uint64_t>(n), 1024)));
        for (cereal::size_type i = 0; i < n; ++i) {
            vec.emplace_back();
            ar(vec.back());
        }
    }
}

template<class T>
inline void CEREAL_LOAD_FUNCTION_NAME(BoundedBinaryInputArchive& ar, cereal::BinaryData<T>& t)
{
    ar.loadBinary(t.data, static_cast<std::streamsize>(t.size));
}

} // namespace Slic3r

// load_minimal (used by cereal for enums) finds the matching save_minimal through the output
// archive paired with this input archive.
namespace cereal { namespace traits { namespace detail {
template<> struct get_output_from_input<Slic3r::BoundedBinaryInputArchive> { using type = cereal::BinaryOutputArchive; };
}}}

#endif // slic3r_CAD_BoundedBinaryArchive_hpp_
