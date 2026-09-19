#include "CutRecipe.hpp"

#include "Model.hpp"
#include "ImageFill.hpp"   // image_fill_sha256_hex - the project's SHA-256 helper

#include <cstring>
#include <cmath>
#include <limits>
#include <algorithm>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Equality. Exact for the discrete fields; the floating-point ones are compared
// bit-for-bit on purpose. This is a ROUND-TRIP test predicate, not a geometric
// one: the serializer writes doubles at full precision, so a value that comes
// back changed at all means the serialization lost something.
// ---------------------------------------------------------------------------

bool CutRecipeGroove::operator==(const CutRecipeGroove& o) const
{
    return depth == o.depth && width == o.width && flaps_angle == o.flaps_angle && angle == o.angle &&
           depth_init == o.depth_init && width_init == o.width_init &&
           flaps_angle_init == o.flaps_angle_init && angle_init == o.angle_init &&
           depth_tolerance == o.depth_tolerance && width_tolerance == o.width_tolerance;
}

bool CutRecipeConnector::operator==(const CutRecipeConnector& o) const
{
    return pos == o.pos && rotation_m.matrix() == o.rotation_m.matrix() &&
           radius == o.radius && height == o.height &&
           radius_tolerance == o.radius_tolerance && height_tolerance == o.height_tolerance &&
           z_angle == o.z_angle && type == o.type && style == o.style && shape == o.shape &&
           flexi == o.flexi;
}

bool CutRecipeStroke::operator==(const CutRecipeStroke& o) const
{
    if (closed != o.closed || smoothing != o.smoothing || samples.size() != o.samples.size())
        return false;
    if (stroke_bounds != o.stroke_bounds || finished_open != o.finished_open)
        return false;
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i].pos != o.samples[i].pos || samples[i].normal != o.samples[i].normal ||
            samples[i].facet != o.samples[i].facet)
            return false;
    return true;
}

bool CutRecipeSheet::operator==(const CutRecipeSheet& o) const
{
    return nx == o.nx && ny == o.ny && half_size_u == o.half_size_u && half_size_v == o.half_size_v &&
           values == o.values;
}

bool CutRecipe::operator==(const CutRecipe& o) const
{
    if (version != o.version || kind != o.kind)
        return false;
    if (plane_center != o.plane_center || rotation_m.matrix() != o.rotation_m.matrix())
        return false;
    if (sheet != o.sheet || stroke != o.stroke || groove != o.groove)
        return false;
    if (draw_direction != o.draw_direction || draw_view_dir != o.draw_view_dir ||
        draw_extension != o.draw_extension || draw_angle_deg != o.draw_angle_deg ||
        draw_through_all != o.draw_through_all || draw_depth != o.draw_depth ||
        draw_ext_angle_set != o.draw_ext_angle_set ||
        (draw_ext_angle_set && draw_ext_angle_deg != o.draw_ext_angle_deg))
        return false;
    if (thickness != o.thickness || thickness_offset != o.thickness_offset)
        return false;
    if (keep_upper != o.keep_upper || keep_lower != o.keep_lower || keep_as_parts != o.keep_as_parts ||
        place_on_cut_upper != o.place_on_cut_upper || place_on_cut_lower != o.place_on_cut_lower ||
        rotate_upper != o.rotate_upper || rotate_lower != o.rotate_lower)
        return false;
    if (upper_visibility != o.upper_visibility || lower_visibility != o.lower_visibility)
        return false;
    if (connectors != o.connectors)
        return false;
    // The mesh is compared by its content hash, which is what the 3MF stores and
    // what makes two halves share one blob.
    return mesh_hash == o.mesh_hash;
}

bool CutRecipe::valid() const
{
    // Any version this build knows, not only the newest: a version 1 file's cut is
    // reproducible from what it carries, so refusing it would take "Edit cut" away
    // from every project saved before the chain landed.
    if (!cut_recipe_version_supported(version))
        return false;
    if (!has_mesh())
        return false;
    switch (kind) {
    case CutRecipeKind::Curved: return sheet.valid();
    // A stroke needs enough raw samples that finish() can produce a path at all.
    case CutRecipeKind::Drawn:  return stroke.samples.size() >= size_t(DrawCutStroke::MinSamples);
    case CutRecipeKind::Plane:
    case CutRecipeKind::Groove: return true;
    }
    return false;
}

DrawCutParams CutRecipe::draw_params() const
{
    DrawCutParams p;
    p.direction        = (draw_direction >= int(DrawCutDirection::SurfaceNormal) &&
                          draw_direction <= int(DrawCutDirection::AxisZ)) ?
                         DrawCutDirection(draw_direction) : DrawCutDirection::SurfaceNormal;
    p.view_dir         = draw_view_dir;
    p.extension        = draw_extension;
    p.angle_deg        = draw_angle_deg;
    p.through_all      = draw_through_all;
    p.depth            = draw_depth;
    // "Continue the band" is an EMPTY optional, not a number: it has to keep tracking
    // Angle after the recipe is loaded and the user moves Angle.
    if (draw_ext_angle_set)
        p.extension_angle_deg = draw_ext_angle_deg;
    else
        p.extension_angle_deg.reset();
    p.thickness        = thickness;
    p.thickness_offset = thickness_offset;
    return p;
}

void CutRecipe::migrate_draw_v2()
{
    if (version >= CutRecipeVersion)
        return;
    // ONLY A VERSION <= 2 RECIPE HAS ANYTHING TO REWRITE, and saying so explicitly
    // is what version 4 made necessary. This used to be guarded only by "not already
    // current", which was the same thing while 3 was current - and stopped being it
    // the moment 4 arrived: a version 3 recipe is not current any more, so it fell
    // into the v2 rewrite below and had its lip angle mapped through 90 - |a|. A
    // stored 35 came back as 55, silently changing a cut the user had made.
    //
    // Version 3 -> 4 added a field and changed no meanings, so the whole migration
    // for it is the stamp at the bottom.
    if (version <= 2 && kind == CutRecipeKind::Drawn) {
        // THE ANGLE. A v2 draft was signed about zero and meant "tilt the ruling
        // away from the loop's outside"; the phase-3 lip angle is unsigned and means
        // "how steeply the band leans in towards the core". They are not the same
        // quantity, so no formula preserves the old cut - what there is, is a nearest
        // intent. |draft| is how far the old ruling leaned OFF the surface normal, and
        // the phase-3 surface closest to that is a lip of 90 - |draft|: a v2 angle of
        // 0 (the ruling straight down the normal) becomes a 90-degree straight wall,
        // which is exactly what that cut looked like on a flat face.
        //
        // CLAMPED INTO THE POSITIVE HALF, not into the whole signed range. The lip
        // angle became signed in version 4 (the sign is which side the band leans),
        // and a v2 draft carries no such information - it was unsigned in this
        // direction by construction, |draft| being how far the ruling leaned OFF the
        // normal. 90 - |draft| can go negative for a draft past 90, and letting it
        // would migrate an old cut into a MIRRORED one, which is a different surface
        // rather than a nearest intent. 0 is the floor, as it was before the range
        // widened.
        const double draft = std::abs(draw_angle_deg);
        draw_angle_deg = std::clamp(90.0 - draft, 0.0, DrawCutMaxLipAngleDeg);

        // THE DEPTH. A v2 depth was a reach through the part, meaningless as a band
        // travel: a stored 10 or 40 would inset the core past the loop's own radius
        // and leave no flat at all. Take the phase-3 default, which is the number the
        // panel would offer a new user.
        draw_depth = 3.0;

        // THROUGH ALL. A v2 recipe almost always had this on, because it was the
        // default; carrying it forward would reproduce exactly the "projects through
        // the object at random angles" cut the owner reported. Turn it off so the
        // migrated cut gets the flat core that is the point of phase 3.
        draw_through_all = false;
    }
    version = CutRecipeVersion;
}

CurvedCutSheet CutRecipe::curved_sheet() const
{
    CurvedCutSheet s;
    if (!sheet.valid())
        return s;
    s.reset(sheet.nx, sheet.ny);
    s.set_half_size(sheet.half_size_u, sheet.half_size_v);
    s.set_values(sheet.values);
    return s;
}

// ---------------------------------------------------------------------------
// Connector conversion.
// ---------------------------------------------------------------------------

void cut_recipe_connectors_from_model(const std::vector<CutConnector>& in, std::vector<CutRecipeConnector>& out)
{
    out.clear();
    out.reserve(in.size());
    for (const CutConnector& c : in) {
        CutRecipeConnector r;
        r.pos              = c.pos;
        r.rotation_m       = c.rotation_m;
        r.radius           = c.radius;
        r.height           = c.height;
        r.radius_tolerance = c.radius_tolerance;
        r.height_tolerance = c.height_tolerance;
        r.z_angle          = c.z_angle;
        r.type             = int(c.attribs.type);
        r.style            = int(c.attribs.style);
        r.shape            = int(c.attribs.shape);
        r.flexi            = c.flexi;
        out.emplace_back(r);
    }
}

void cut_recipe_connectors_to_model(const std::vector<CutRecipeConnector>& in, std::vector<CutConnector>& out)
{
    out.clear();
    out.reserve(in.size());
    for (const CutRecipeConnector& r : in) {
        // The recipe may have come from a file: clamp every enum before casting,
        // the same discipline the cut_information.xml reader follows.
        const int type  = (r.type  >= 0 && r.type  <= int(CutConnectorType::FlexiJoint)) ? r.type  : int(CutConnectorType::Plug);
        const int style = (r.style >= 0 && r.style <= int(CutConnectorStyle::Undef))     ? r.style : int(CutConnectorStyle::Prism);
        const int shape = (r.shape >= 0 && r.shape <= int(CutConnectorShape::Undef))     ? r.shape : int(CutConnectorShape::Circle);
        CutConnector c(r.pos, r.rotation_m, r.radius, r.height, r.radius_tolerance, r.height_tolerance, r.z_angle,
                       CutConnectorAttributes(CutConnectorType(type), CutConnectorStyle(style), CutConnectorShape(shape)));
        c.flexi = r.flexi;
        out.emplace_back(c);
    }
}

// ---------------------------------------------------------------------------
// The mesh blob.
//
// Deliberately NOT STL. An STL round trip would lose the vertex indexing (every
// facet carries its own three vertices) and would re-weld on the way back, so
// the mesh that came out would not be the mesh that went in - and the whole
// point of storing it is that re-cutting reproduces the SAME halves. This keeps
// the indexed triangle set exactly as it stands.
// ---------------------------------------------------------------------------

static const char   CutRecipeMeshMagic[8] = { 'E', 'S', 'C', 'U', 'T', 'M', 'S', 'H' };
static const uint32_t CutRecipeMeshBlobVersion = 1;

// A blob bigger than this is not a mesh we wrote; refuse it rather than trying
// to allocate whatever a corrupt header claims. 200M triangles is far past any
// printable model and still far short of overflowing the arithmetic below.
static const uint64_t CutRecipeMeshMaxFacets   = 200ull * 1000ull * 1000ull;
static const uint64_t CutRecipeMeshMaxVertices = 200ull * 1000ull * 1000ull;

static void blob_put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(uint8_t( x        & 0xFF));
    v.push_back(uint8_t((x >>  8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 24) & 0xFF));
}

static uint32_t blob_get_u32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void blob_put_f32(std::vector<uint8_t>& v, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    blob_put_u32(v, bits);
}

static float blob_get_f32(const uint8_t* p)
{
    const uint32_t bits = blob_get_u32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::vector<uint8_t> cut_recipe_mesh_to_blob(const TriangleMesh& mesh)
{
    const indexed_triangle_set& its = mesh.its;
    std::vector<uint8_t> blob;
    if (its.vertices.size() > CutRecipeMeshMaxVertices || its.indices.size() > CutRecipeMeshMaxFacets)
        return blob;
    blob.reserve(16 + its.vertices.size() * 12 + its.indices.size() * 12);
    blob.insert(blob.end(), CutRecipeMeshMagic, CutRecipeMeshMagic + sizeof(CutRecipeMeshMagic));
    blob_put_u32(blob, CutRecipeMeshBlobVersion);
    blob_put_u32(blob, uint32_t(its.vertices.size()));
    blob_put_u32(blob, uint32_t(its.indices.size()));
    for (const Vec3f& v : its.vertices) {
        blob_put_f32(blob, v.x());
        blob_put_f32(blob, v.y());
        blob_put_f32(blob, v.z());
    }
    for (const Vec3i32& f : its.indices) {
        blob_put_u32(blob, uint32_t(f[0]));
        blob_put_u32(blob, uint32_t(f[1]));
        blob_put_u32(blob, uint32_t(f[2]));
    }
    return blob;
}

bool cut_recipe_mesh_from_blob(const std::vector<uint8_t>& blob, TriangleMesh& out)
{
    out = TriangleMesh();
    // Untrusted file content throughout: every length is checked against what is
    // actually there before a single byte is read past the header.
    if (blob.size() < 20)
        return false;
    if (std::memcmp(blob.data(), CutRecipeMeshMagic, sizeof(CutRecipeMeshMagic)) != 0)
        return false;
    const uint32_t version = blob_get_u32(blob.data() + 8);
    if (version != CutRecipeMeshBlobVersion)
        return false;
    const uint64_t n_vert  = blob_get_u32(blob.data() + 12);
    const uint64_t n_facet = blob_get_u32(blob.data() + 16);
    if (n_vert > CutRecipeMeshMaxVertices || n_facet > CutRecipeMeshMaxFacets)
        return false;
    const uint64_t need = 20ull + n_vert * 12ull + n_facet * 12ull;
    if (uint64_t(blob.size()) != need)
        return false;

    indexed_triangle_set its;
    its.vertices.resize(size_t(n_vert));
    its.indices.resize(size_t(n_facet));
    const uint8_t* p = blob.data() + 20;
    for (size_t i = 0; i < size_t(n_vert); ++i, p += 12)
        its.vertices[i] = Vec3f(blob_get_f32(p), blob_get_f32(p + 4), blob_get_f32(p + 8));
    for (size_t i = 0; i < size_t(n_facet); ++i, p += 12) {
        const uint32_t a = blob_get_u32(p), b = blob_get_u32(p + 4), c = blob_get_u32(p + 8);
        // An index past the vertex array would be read out of bounds by every
        // consumer of the mesh downstream.
        if (a >= n_vert || b >= n_vert || c >= n_vert)
            return false;
        its.indices[i] = Vec3i32(int(a), int(b), int(c));
    }
    out = TriangleMesh(std::move(its));
    return true;
}

std::string cut_recipe_mesh_hash(const std::vector<uint8_t>& blob)
{
    return image_fill_sha256_hex(blob);
}

// ---------------------------------------------------------------------------
// The stroke <-> chain conversion. 2026-09-12.
// ---------------------------------------------------------------------------

void cut_recipe_stroke_to_chain(const CutRecipeStroke& in, DrawCutChain& out)
{
    out.clear();
    if (in.samples.empty())
        return;

    // Do the bounds TILE the sample list exactly? Anything else - a gap, an overlap,
    // a range past the end, a first range not starting at 0 - and they are discarded
    // in favour of the one-stroke fallback. A chain whose ranges disagree with its
    // samples corrupts undo silently, which is worse than losing the stroke split.
    bool tiles = !in.stroke_bounds.empty();
    size_t expect = 0;
    for (const std::pair<uint32_t, uint32_t>& b : in.stroke_bounds) {
        if (size_t(b.first) != expect || b.second <= b.first || size_t(b.second) > in.samples.size()) {
            tiles = false;
            break;
        }
        expect = size_t(b.second);
    }
    if (tiles && expect != in.samples.size())
        tiles = false;

    // A line that was CUT WITH and is not a loop was, by definition, finished - so an
    // unclosed stored stroke is finished-open whether or not the flag says so. That is
    // what makes a version 1 recipe (which has no flag) re-cut to the same halves.
    const bool fin_open = !in.closed && (in.finished_open || in.samples.size() >= DrawCutChain::MinChainSamples);

    if (!tiles) {
        out.set_samples(in.samples, in.closed, fin_open);
        return;
    }

    // Rebuild the chain by replaying the appends. Every one is a BACK append, which
    // is what the stored ranges describe: they are ranges in the final sample order,
    // and replaying them in order reproduces exactly that order whether the strokes
    // were originally drawn onto the front or the back.
    //
    // THE SNAP RADIUS HERE MUST BE TINY, NOT HUGE, and the reason is worth stating
    // because the obvious choice is exactly backwards. A huge radius does make every
    // append's START test pass - but the same radius is what append() measures the
    // CLOSURE with, so the very first replayed stroke would be judged to have closed
    // the chain on its far endpoint, and a closed chain refuses every later append.
    // The whole replay would then fall back to one stroke, silently.
    //
    // The joins are EXACT by construction (each range begins where the last ended, and
    // the join sample is re-inserted below), so an epsilon radius passes every start
    // test and fails every closure test - which is what is wanted, since the closure is
    // restored explicitly at the end from the stored flag.
    const double any = 1e-9;
    for (const std::pair<uint32_t, uint32_t>& b : in.stroke_bounds) {
        std::vector<DrawCutSample> stroke(in.samples.begin() + int(b.first), in.samples.begin() + int(b.second));
        if (stroke.size() < 2) {
            // A one-sample stroke cannot be appended (append() needs two to have a
            // direction), so fall back rather than dropping a sample.
            out.set_samples(in.samples, in.closed, fin_open);
            return;
        }
        // append() drops a first sample duplicating the join, so re-insert the join
        // sample at the front of each continuation: the stored ranges do NOT repeat it
        // (cut_recipe_stroke_from_chain writes the chain's own flat list), and without
        // it append() would see a stroke starting a whole span away from the endpoint.
        if (!out.empty())
            stroke.insert(stroke.begin(), out.samples()[out.size() - 1]);
        if (out.append(stroke, any) == DrawChainEnd::None) {
            out.set_samples(in.samples, in.closed, fin_open);
            return;
        }
    }
    if (out.size() != in.samples.size()) {
        out.set_samples(in.samples, in.closed, fin_open);
        return;
    }
    // Every append cleared the flag, so the verdict is restored last, from what was
    // stored rather than from what the replay happened to produce.
    if (in.closed)
        out.force_close();
    else if (fin_open)
        out.finish_open();
}

CutRecipeStroke cut_recipe_stroke_from_chain(const DrawCutChain& chain, double smoothing)
{
    CutRecipeStroke out;
    out.samples       = chain.samples();
    out.closed        = chain.is_closed();
    out.finished_open = chain.is_finished_open();
    out.smoothing     = smoothing;
    // The chain's own ranges, SORTED BY POSITION rather than in append order.
    //
    // DrawCutChain::stroke_bounds() is in APPEND order, because that is the order undo
    // consumes it in - and a front append puts its samples at index 0 while its range
    // goes to the end of the list. The recipe cannot store that order, because the
    // replay in cut_recipe_stroke_to_chain() rebuilds the chain by walking the sample
    // list forwards: it is the only replay that reproduces the stored sample ORDER,
    // which is what the cut is made from.
    //
    // What is lost is the sequence the strokes were drawn in, so a reopened cut's
    // Ctrl+Z takes back the LAST stroke along the line rather than the last one drawn.
    // That is a fair trade: nobody remembers the drawing order of a line from a
    // previous session, and the alternative - storing both orders - would let the two
    // disagree about the same line.
    out.stroke_bounds.reserve(chain.stroke_count());
    for (const std::pair<size_t, size_t>& b : chain.stroke_bounds())
        out.stroke_bounds.emplace_back(uint32_t(b.first), uint32_t(b.second));
    std::sort(out.stroke_bounds.begin(), out.stroke_bounds.end());
    return out;
}


// ---------------------------------------------------------------------------
// MIRRORING.
//
// The contract, the frames and the handedness argument are all in CutRecipe.hpp
// above cut_recipe_mirrored(); this is only the arithmetic.
// ---------------------------------------------------------------------------

Vec3d cut_mirror_vector(CutMirrorAxis axis)
{
    switch (axis) {
    case CutMirrorAxis::X: return Vec3d(-1.0,  1.0,  1.0);
    case CutMirrorAxis::Y: return Vec3d( 1.0, -1.0,  1.0);
    case CutMirrorAxis::Z: return Vec3d( 1.0,  1.0, -1.0);
    }
    return Vec3d::Ones();
}

Vec3d cut_mirror_point(const Vec3d& p, CutMirrorAxis axis, const Vec3d& pivot)
{
    const Vec3d m = cut_mirror_vector(axis);
    return pivot + (p - pivot).cwiseProduct(m);
}

Transform3d cut_mirror_rotation(const Transform3d& rotation_m, CutMirrorAxis axis)
{
    const Vec3d    m = cut_mirror_vector(axis);
    const Matrix3d R = rotation_m.linear();

    // M * R, column by column. M is diagonal, so this is a row scaling.
    Matrix3d out;
    for (int c = 0; c < 3; ++ c)
        out.col(c) = R.col(c).cwiseProduct(m);

    // The one negated column, which is what turns the det -1 reflected frame back
    // into a det +1 rotation. IT MUST BE THE SECOND (the frame's Y / the sheet's v)
    // and not "whichever looks convenient": the whole point of fixing it is that
    // the plane-LOCAL residual R'^T * M * R then comes out as diag(1, -1, 1) for
    // every axis and every starting rotation, so the sheet grid and the stroke
    // samples always take the same one fixed re-indexing. Negating column 1 or 3
    // would work just as well for the determinant and would make that residual a
    // u-mirror or a z-mirror instead - i.e. it would move the plane normal or swap
    // which in-plane axis is re-indexed, and the (u,v) bookkeeping would become a
    // case analysis. Column 3 in particular is the plane NORMAL; negating it would
    // flip which half counts as upper, and then the keep_/place_on_cut_/rotate_
    // pairs really would have to be swapped.
    out.col(1) = -out.col(1);

    // Re-orthonormalize. M * R is exactly orthonormal in theory and the column
    // negation does not change that, but rotation_m has been through a long chain
    // of products by the time it reaches here and may have drifted; a frame that
    // is only nearly orthonormal is the kind of thing that shows up much later as
    // a connector pocket that does not quite line up. Gram-Schmidt off the NORMAL
    // (column 3), because that is the axis that must be exact - it is the cut.
    Vec3d z = out.col(2);
    if (z.norm() < 1e-12)
        z = Vec3d::UnitZ();
    z.normalize();
    Vec3d x = out.col(0) - z * z.dot(out.col(0));
    if (x.norm() < 1e-12) {
        // Degenerate input frame: pick any axis perpendicular to z rather than
        // returning something non-orthogonal.
        x = std::abs(z.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
        x = x - z * z.dot(x);
    }
    x.normalize();
    const Vec3d y = z.cross(x);   // right-handed by construction, so det == +1

    Transform3d t = Transform3d::Identity();
    t.matrix().block(0, 0, 3, 3) << x, y, z;
    return t;
}

// The plane-LOCAL residual of the mirror: negate local y. See the header - this
// is diag(1, -1, 1) for every axis, by the column choice in cut_mirror_rotation().
static inline Vec3d cut_mirror_local(const Vec3d& v) { return Vec3d(v.x(), -v.y(), v.z()); }

CutRecipe cut_recipe_mirrored(const CutRecipe& src, CutMirrorAxis axis, const Vec3d& pivot)
{
    CutRecipe r = src;

    // --- the cut frame ----------------------------------------------------
    r.plane_center = cut_mirror_point(src.plane_center, axis, pivot);
    r.rotation_m   = cut_mirror_rotation(src.rotation_m, axis);

    // --- the surface ------------------------------------------------------
    // The sheet: mirror the ROWS (the v index, which runs over ny with stride nx)
    // and leave the values alone. f'(u, -v) = f(u, v) is the whole change, because
    // local z is untouched by the residual.
    //
    // THE AXIS TRAP CurvedCutSheet::flip_about_u() documents applies here word for
    // word: the v index runs over NY and the stride is NX. On a square grid getting
    // those the wrong way round is invisible; on a 10 x 2 sheet it reads off the
    // end. The [CutMirror] tests use a non-square grid for exactly that reason.
    if (r.kind == CutRecipeKind::Curved && src.sheet.valid()) {
        const int nx = src.sheet.nx, ny = src.sheet.ny;
        std::vector<double> nz(src.sheet.values.size(), 0.0);
        for (int j = 0; j < ny; ++ j)
            for (int i = 0; i < nx; ++ i)
                nz[size_t(j) * size_t(nx) + size_t(i)] =
                    src.sheet.values[size_t(ny - 1 - j) * size_t(nx) + size_t(i)];
        r.sheet.values = std::move(nz);
        // nx / ny / half_size_u / half_size_v are unchanged: the grid is symmetric
        // in v about its own centre, so mirroring the indices lands exactly on grid
        // points and the extent does not move. No evaluation, no re-sampling,
        // nothing lost - and the operation is its own inverse.
    }

    // The stroke: the samples and their normals are directions and points in the
    // plane frame, so both take the residual. `facet` is an index into the pre-cut
    // mesh and means nothing after a mirror of the FRAME - but it means nothing to
    // the cut either (finish() uses pos and normal), and the flip-plane path leaves
    // it alone for the same reason, so it rides through untouched.
    if (r.kind == CutRecipeKind::Drawn) {
        for (DrawCutSample& s : r.stroke.samples) {
            s.pos    = cut_mirror_local(s.pos);
            s.normal = cut_mirror_local(s.normal);
        }
        // The sample ORDER is untouched, so stroke_bounds stays valid as it stands -
        // the same reason flip_cut_plane() rebuilds the chain from its own ranges
        // rather than calling set_samples(). A mirror reverses the winding of a
        // closed loop, which DrawCut's own finish() handles (it derives binormals
        // from the path either way); reversing the list here would instead invalidate
        // every stored range.
    }
    // draw_view_dir is a direction in the plane frame too, and it is stored on every
    // recipe rather than only on a drawn one, so it is mirrored unconditionally - a
    // recipe that later becomes a drawn cut should not carry a stale view direction.
    r.draw_view_dir = cut_mirror_local(src.draw_view_dir);

    // The groove: every field is a scalar magnitude (depth, width, the two angles
    // and their _init / _tolerance partners). None of them encodes a direction in
    // the plane, so a mirror passes through them unchanged and only the frame they
    // sit on moves. Nothing to do - r.groove is already the copy.

    // --- connectors -------------------------------------------------------
    for (size_t i = 0; i < r.connectors.size(); ++ i) {
        const CutRecipeConnector& c = src.connectors[i];
        CutRecipeConnector&       o = r.connectors[i];
        // pos is in the OBJECT frame, the same frame plane_center is in, so it takes
        // the same reflection about the same pivot.
        o.pos        = cut_mirror_point(c.pos, axis, pivot);
        o.rotation_m = cut_mirror_rotation(c.rotation_m, axis);
        // z_angle turns the connector's own polygon about its own axis. A mirror
        // presents that polygon the other way round, so for a shape that HAS an
        // orientation the angle negates; for a Circle it means nothing and negating
        // it would only make "mirror twice" fail to be a bit-exact identity for no
        // benefit.
        if (o.shape != int(CutConnectorShape::Circle))
            o.z_angle = -c.z_angle;
        // type, style, shape, the radii, the tolerances and the flexi parameters are
        // all untouched. type especially: Plug / Dowel / Snap / FlexiJoint is a
        // mating ROLE, not a chirality. A mirrored half still needs the mating
        // partner it had, and silently turning a plug into a dowel would break the
        // one use case this feature exists for.
    }

    // The after-cut attributes (keep_upper / keep_lower, place_on_cut_*, rotate_*,
    // the visibilities) are copied straight through and NOT swapped. They would have
    // to be swapped if the mirror inverted the plane normal - but it does not: the
    // column choice in cut_mirror_rotation() maps the normal e3 to M*e3 exactly, so
    // the side that was upper is still upper. See the header.

    // The mesh and its hash ride through untouched. The recipe's mesh is the PRE-CUT
    // shape, and mirroring a cut does not change the object it is being made in -
    // whether it is the same object (the panel's Mirror buttons) or a different one
    // (Copy cut to..., which re-derives the mesh from the target at commit time).

    return r;
}

// The label "Copy cut to..." lists an object under. See the header for why the
// parts appear in it and why the target is still the whole object.
std::string cut_target_label(const ModelObject* object, size_t max_parts)
{
    if (object == nullptr)
        return std::string();

    // An object with no name of its own still needs to be pickable. "Object" is
    // what the rest of the UI calls one.
    std::string label = object->name.empty() ? std::string("Object") : object->name;

    // Only MODEL PARTS. A negative volume, a modifier or a support blocker is not
    // what identifies an object to the eye, and listing them would crowd out the
    // names that do.
    std::vector<std::string> parts;
    for (const ModelVolume* v : object->volumes) {
        if (v == nullptr || !v->is_model_part())
            continue;
        parts.push_back(v->name.empty() ? std::string("part") : v->name);
    }

    // One part identifies nothing the object's own name does not already say, and
    // for a single-part object the two names are usually the same string.
    if (parts.size() < 2)
        return label;

    label += " (";
    const size_t shown = std::min(parts.size(), max_parts == 0 ? parts.size() : max_parts);
    for (size_t i = 0; i < shown; ++ i) {
        if (i > 0)
            label += ", ";
        label += parts[i];
    }
    if (shown < parts.size())
        label += ", +" + std::to_string(parts.size() - shown) + " more";
    label += ")";
    return label;
}
} // namespace Slic3r
