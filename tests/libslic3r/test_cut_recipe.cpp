// ---------------------------------------------------------------------------
// RE-EDITABLE CUTS: the recipe's round trip, and its ability to reproduce a cut.
//
// A cut used to be destructive - the plane, the sheet and the stroke were
// session state in the gizmo and nothing described how the two halves had been
// made. A CutRecipe is that description, plus the pre-cut mesh, carried by both
// halves and written to Metadata/cut_recipe.xml (with the mesh beside it as
// Metadata/cut_recipe/<sha256>.bin).
//
// What is asserted here:
//   1. The mesh blob round trips EXACTLY - same vertices, same indexing - which
//      is what lets a re-cut reproduce the same halves rather than merely
//      similar ones.
//   2. The whole recipe survives a save/load of a real project 3MF, for each of
//      the three surface kinds, field for field.
//   3. Re-cutting from the LOADED recipe produces the same halves as the
//      original cut did, compared by volume and bounding box.
//   4. The pre-cut mesh is stored ONCE even though both halves reference it.
//   5. With no recipe on the objects, nothing is written - the opt-out path
//      behaves exactly as the tool did before this feature.
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/CutRecipe.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/DrawCut.hpp>
#include <libslic3r/CutUtils.hpp>
// translation_transform(), for composing the cut matrix the gizmo composes.
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>

#include <libslic3r/miniz_extension.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Slic3r;

// The test article: a 40 mm cube centred on the origin, so a cut plane at the
// frame's z == 0 runs through its middle. The same article the curved- and
// draw-cut suites use.
static const double RCUBE = 40.0;

static indexed_triangle_set recipe_centred_cube(double s = RCUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f &v : its.vertices)
        v -= Vec3f(float(s) * 0.5f, float(s) * 0.5f, float(s) * 0.5f);
    return its;
}

// Volume of a closed indexed triangle set, via the divergence theorem. Signed,
// so a consistently wound mesh gives a positive number.
static double recipe_mesh_volume(const indexed_triangle_set &its)
{
    double v = 0.0;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[size_t(f[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(f[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(f[2])].cast<double>();
        v += a.dot(b.cross(c));
    }
    return std::abs(v) / 6.0;
}

static double recipe_object_volume(const ModelObject *mo)
{
    double v = 0.0;
    for (const ModelVolume *vol : mo->volumes)
        if (vol->type() == ModelVolumeType::MODEL_PART) {
            TriangleMesh m = vol->mesh();
            m.transform(vol->get_matrix());
            v += recipe_mesh_volume(m.its);
        }
    return v;
}

static BoundingBoxf3 recipe_object_bbox(const ModelObject *mo)
{
    BoundingBoxf3 bb;
    for (const ModelVolume *vol : mo->volumes)
        if (vol->type() == ModelVolumeType::MODEL_PART) {
            TriangleMesh m = vol->mesh();
            m.transform(vol->get_matrix());
            bb.merge(m.bounding_box());
        }
    return bb;
}

// A curved sheet with one control point pulled up, so the surface really is a
// height field rather than a plane in disguise.
static CutRecipeSheet recipe_bent_sheet()
{
    CurvedCutSheet s;
    s.reset(5, 5);
    s.set_half_size(30.0, 30.0);
    s.at(2, 2) = 6.0;

    CutRecipeSheet out;
    out.nx          = s.nx();
    out.ny          = s.ny();
    out.half_size_u = s.half_size_u();
    out.half_size_v = s.half_size_v();
    out.values      = s.values();
    return out;
}

// A straight stroke across the top face of the cube, captured the way the gizmo
// captures one: raw samples in the cut plane's frame with a surface normal.
static CutRecipeStroke recipe_straight_stroke()
{
    CutRecipeStroke st;
    st.closed    = false;
    st.smoothing = 0.2;
    for (int i = 0; i <= 40; ++i) {
        DrawCutSample s;
        const double t = -20.0 + double(i);
        s.pos    = Vec3d(t, 0.0, 20.0);
        s.normal = Vec3d::UnitZ();
        s.facet  = size_t(i);
        st.samples.push_back(s);
    }
    return st;
}

// A fully populated recipe of the given kind, so the round trip has something to
// lose in every field rather than only in the ones a default happens to differ
// on.
static CutRecipe recipe_make(CutRecipeKind kind)
{
    CutRecipe r;
    r.version = CutRecipeVersion;
    r.kind    = kind;

    r.plane_center = Vec3d(1.5, -2.25, 3.125);
    {
        Transform3d rot = Transform3d::Identity();
        rot.rotate(Eigen::AngleAxisd(0.3, Vec3d(0.0, 1.0, 0.0)));
        r.rotation_m = rot;
    }

    r.thickness        = 0.8;
    r.thickness_offset = CutThicknessOffset::Above;

    r.keep_upper         = true;
    r.keep_lower         = true;
    r.keep_as_parts      = false;
    r.place_on_cut_upper = true;
    r.place_on_cut_lower = true;
    r.rotate_upper       = false;
    r.rotate_lower       = true;
    r.upper_visibility   = 1;
    r.lower_visibility   = 2;

    if (kind == CutRecipeKind::Curved)
        r.sheet = recipe_bent_sheet();
    if (kind == CutRecipeKind::Drawn) {
        r.stroke           = recipe_straight_stroke();
        r.draw_direction   = int(DrawCutDirection::SurfaceNormal);
        r.draw_view_dir    = Vec3d(0.1, 0.2, -0.9);
        r.draw_extension   = 7.5;
        r.draw_angle_deg   = 4.25;
        r.draw_through_all = false;
        r.draw_depth       = 12.75;
        // VERSION 4. Populated, so the round trip has a value to lose here too; the
        // UNSET state is covered on its own below, where it matters more (it is what
        // every version <= 3 file decays to).
        r.draw_ext_angle_set = true;
        r.draw_ext_angle_deg = -22.5;
    }
    if (kind == CutRecipeKind::Groove) {
        r.groove.depth            = 3.5f;
        r.groove.width            = 5.5f;
        r.groove.flaps_angle      = 0.25f;
        r.groove.angle            = 0.125f;
        r.groove.depth_init       = 3.0f;
        r.groove.width_init       = 5.0f;
        r.groove.flaps_angle_init = 0.2f;
        r.groove.angle_init       = 0.1f;
        r.groove.depth_tolerance  = 0.15f;
        r.groove.width_tolerance  = 0.2f;
    }

    // One connector of each kind the panel can place, so the connector list is
    // not trivially empty and the enum clamping on the way back in is exercised.
    {
        CutRecipeConnector c;
        c.pos              = Vec3d(4.0, -3.0, 0.0);
        c.rotation_m       = Transform3d::Identity();
        c.radius           = 2.5f;
        c.height           = 6.0f;
        c.radius_tolerance = 0.05f;
        c.height_tolerance = 0.15f;
        c.z_angle          = 0.75f;
        c.type             = int(CutConnectorType::Plug);
        c.style            = int(CutConnectorStyle::Frustum);
        c.shape            = int(CutConnectorShape::Hexagon);
        r.connectors.push_back(c);

        CutRecipeConnector d = c;
        d.pos   = Vec3d(-4.0, 3.0, 0.0);
        d.type  = int(CutConnectorType::Dowel);
        d.shape = int(CutConnectorShape::Circle);
        r.connectors.push_back(d);
    }

    r.mesh      = TriangleMesh(recipe_centred_cube());
    r.mesh_hash = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(r.mesh));
    return r;
}

// Save a model as a project 3MF and read it back, the way the curved-cut suite's
// connector round trip does: Metadata/ parts only reach a file through the
// PROJECT writer, not the generic 3MF one.
static void recipe_round_trip(Model &src, Model &dst, const std::string &file)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams        store_params;
    store_params.path     = file.c_str();
    store_params.model    = &src;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &dst, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                         LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);
}

static std::string recipe_tmp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    return (tmp_root / name).string();
}

// How many entries under Metadata/cut_recipe/ a written 3MF actually holds. The
// point of the content-addressed store is that two halves of one cut share one
// blob, and only the file itself can prove that.
static int recipe_count_mesh_blobs(const std::string &file)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, file))
        return -1;
    int n = 0;
    const mz_uint num = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        const std::string name = stat.m_filename;
        if (name.rfind("Metadata/cut_recipe/", 0) == 0)
            ++n;
    }
    close_zip_reader(&archive);
    return n;
}

static bool recipe_has_entry(const std::string &file, const std::string &path)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, file))
        return false;
    bool found = false;
    const mz_uint num = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < num && !found; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        found = (path == stat.m_filename);
    }
    close_zip_reader(&archive);
    return found;
}

// ---------------------------------------------------------------------------
// 1. The mesh blob.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: the pre-cut mesh blob round trips exactly", "[CutRecipe]")
{
    const TriangleMesh mesh(recipe_centred_cube());
    const std::vector<uint8_t> blob = cut_recipe_mesh_to_blob(mesh);
    REQUIRE_FALSE(blob.empty());

    TriangleMesh back;
    REQUIRE(cut_recipe_mesh_from_blob(blob, back));

    // Not "close": IDENTICAL. The vertices come back bit for bit and the facet
    // indexing is unchanged, which is what lets the re-cut reproduce the same
    // halves rather than halves that merely look the same. An STL round trip
    // would fail this - it re-welds, and loses the indexing.
    REQUIRE(back.its.vertices.size() == mesh.its.vertices.size());
    REQUIRE(back.its.indices.size() == mesh.its.indices.size());
    for (size_t i = 0; i < mesh.its.vertices.size(); ++i)
        REQUIRE(back.its.vertices[i] == mesh.its.vertices[i]);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i)
        REQUIRE(back.its.indices[i] == mesh.its.indices[i]);

    // And the hash is a function of the content, so both halves of a cut name
    // the same blob.
    REQUIRE(cut_recipe_mesh_hash(blob) == cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(back)));
    REQUIRE(cut_recipe_mesh_hash(blob).size() == 64);
}

TEST_CASE("Deft: a corrupt mesh blob is refused rather than trusted", "[CutRecipe]")
{
    const TriangleMesh mesh(recipe_centred_cube());
    std::vector<uint8_t> blob = cut_recipe_mesh_to_blob(mesh);
    TriangleMesh out;

    SECTION("truncated") {
        blob.resize(blob.size() / 2);
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
    SECTION("bad magic") {
        blob[0] = 'X';
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
    SECTION("empty") {
        REQUIRE_FALSE(cut_recipe_mesh_from_blob({}, out));
    }
    SECTION("a facet index past the end of the vertex array") {
        // The last facet's first index, set to a vertex that does not exist. A
        // reader that trusted it would read out of bounds downstream.
        const size_t idx_at = blob.size() - 12;
        blob[idx_at + 0] = 0xFF;
        blob[idx_at + 1] = 0xFF;
        blob[idx_at + 2] = 0x00;
        blob[idx_at + 3] = 0x00;
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
}

// ---------------------------------------------------------------------------
// 2. The recipe's 3MF round trip, per surface kind.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a cut recipe survives a 3MF round trip for every surface kind", "[CutRecipe]")
{
    struct Case { CutRecipeKind kind; const char* name; };
    const Case cases[] = {
        { CutRecipeKind::Plane,  "plane"  },
        { CutRecipeKind::Curved, "curved" },
        { CutRecipeKind::Drawn,  "drawn"  },
        { CutRecipeKind::Groove, "groove" },
    };

    for (const Case &c : cases) {
        INFO("surface kind: " << c.name);
        const CutRecipe in = recipe_make(c.kind);
        REQUIRE(in.valid());

        // Two halves of one cut, both carrying the same recipe - which is how a
        // real cut leaves the model, and what makes the mesh sharing meaningful.
        Model src;
        for (int half = 0; half < 2; ++half) {
            ModelObject *mo = src.add_object();
            mo->name        = std::string("half_") + std::to_string(half);
            mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
            mo->cut_id.init();
            mo->cut_recipe = in;
        }
        src.add_default_instances();

        const std::string file = recipe_tmp_file(std::string("edgeslicer_cut_recipe_") + c.name + ".3mf");
        Model             back;
        recipe_round_trip(src, back, file);

        // THE SHARING. One blob, not two, even though both halves reference it.
        REQUIRE(recipe_count_mesh_blobs(file) == 1);
        REQUIRE(recipe_has_entry(file, "Metadata/cut_recipe.xml"));
        REQUIRE(recipe_has_entry(file, "Metadata/cut_recipe/" + in.mesh_hash + ".bin"));

        REQUIRE(back.objects.size() == 2);
        for (const ModelObject *mo : back.objects) {
            REQUIRE(mo->cut_recipe.has_value());
            const CutRecipe &out = *mo->cut_recipe;

            // FIELD FOR FIELD. operator== compares every stored field and the mesh
            // by its hash, so this one line is the whole schema's round trip.
            REQUIRE(out == in);

            // ... and the mesh really did come back, not merely its name.
            REQUIRE(out.has_mesh());
            REQUIRE(out.mesh.its.vertices.size() == in.mesh.its.vertices.size());
            REQUIRE(out.mesh.its.indices.size() == in.mesh.its.indices.size());
            for (size_t i = 0; i < in.mesh.its.vertices.size(); ++i)
                REQUIRE(out.mesh.its.vertices[i] == in.mesh.its.vertices[i]);

            // Spot-check the fields that a sloppy serializer loses first: the
            // rotation matrix, the sheet values, the stroke samples.
            REQUIRE(out.rotation_m.matrix() == in.rotation_m.matrix());
            REQUIRE(out.plane_center == in.plane_center);
            REQUIRE(out.thickness == in.thickness);
            REQUIRE(out.thickness_offset == in.thickness_offset);
            REQUIRE(out.connectors.size() == in.connectors.size());
            if (c.kind == CutRecipeKind::Curved) {
                REQUIRE(out.sheet.values == in.sheet.values);
                REQUIRE(out.sheet.nx == in.sheet.nx);
                REQUIRE(out.sheet.ny == in.sheet.ny);
                REQUIRE(out.sheet.half_size_u == in.sheet.half_size_u);
            }
            if (c.kind == CutRecipeKind::Drawn) {
                REQUIRE(out.stroke.samples.size() == in.stroke.samples.size());
                for (size_t i = 0; i < in.stroke.samples.size(); ++i) {
                    REQUIRE(out.stroke.samples[i].pos == in.stroke.samples[i].pos);
                    REQUIRE(out.stroke.samples[i].normal == in.stroke.samples[i].normal);
                }
                REQUIRE(out.draw_extension == in.draw_extension);
                REQUIRE(out.draw_angle_deg == in.draw_angle_deg);
                // VERSION 4: the extension angle survives, and its "unset" state is a
                // state in its own right rather than a zero.
                REQUIRE(out.draw_ext_angle_set == in.draw_ext_angle_set);
                if (in.draw_ext_angle_set)
                    REQUIRE(out.draw_ext_angle_deg == in.draw_ext_angle_deg);
                REQUIRE(out.draw_through_all == in.draw_through_all);
                REQUIRE(out.draw_depth == in.draw_depth);
            }
            if (c.kind == CutRecipeKind::Groove)
                REQUIRE(out.groove == in.groove);
        }

        if (!std::getenv("SNORCA_RECIPE_KEEP"))
            boost::filesystem::remove(file);
    }
}

// ---------------------------------------------------------------------------
// 3. Re-cutting from the loaded recipe reproduces the same halves.
//
// This is the feature's actual promise. Everything above only shows the bytes
// survive; this shows the bytes are ENOUGH.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a cut re-performed from a loaded recipe reproduces the same halves", "[CutRecipe]")
{
    struct Case { CutRecipeKind kind; const char* name; };
    const Case cases[] = {
        { CutRecipeKind::Plane,  "plane"  },
        { CutRecipeKind::Curved, "curved" },
    };

    for (const Case &c : cases) {
        INFO("surface kind: " << c.name);

        // A recipe with NO connectors and no kerf: this test is about the surface
        // reproducing, and a connector would add its own volumes to both sides
        // and blur what is being compared.
        CutRecipe in = recipe_make(c.kind);
        in.connectors.clear();
        in.thickness          = 0.0;
        in.thickness_offset   = CutThicknessOffset::Centred;
        in.plane_center       = Vec3d::Zero();
        in.rotation_m         = Transform3d::Identity();
        in.place_on_cut_upper = false;
        in.place_on_cut_lower = false;
        in.rotate_upper       = false;
        in.rotate_lower       = false;
        in.mesh_hash          = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(in.mesh));

        // THE ORIGINAL CUT, performed from the recipe as the gizmo would.
        auto perform = [](const CutRecipe &r) {
            Model        m;
            ModelObject *mo = m.add_object();
            mo->name        = "src";
            mo->add_volume(r.mesh)->name = "part";
            m.add_default_instances();

            const ModelObjectCutAttributes attrs =
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
            const Transform3d cut_matrix = Geometry::translation_transform(r.plane_center) * r.rotation_m;

            Cut cut(mo, 0, cut_matrix, attrs);
            ModelObjectPtrs res;
            if (r.kind == CutRecipeKind::Curved)
                res = cut.perform_with_curved_sheet(r.curved_sheet(), r.thickness, r.thickness_offset);
            else
                res = cut.perform_with_plane(r.thickness, r.thickness_offset);

            std::vector<std::pair<double, BoundingBoxf3>> out;
            for (const ModelObject *o : res)
                out.emplace_back(recipe_object_volume(o), recipe_object_bbox(o));
            return out;
        };

        const auto before = perform(in);
        REQUIRE(before.size() == 2);
        // Both halves have material, or the comparison below would be vacuous.
        REQUIRE(before[0].first > 1.0);
        REQUIRE(before[1].first > 1.0);

        // ... now put the recipe through a real 3MF and cut again from what came
        // back.
        Model src;
        {
            ModelObject *mo = src.add_object();
            mo->name        = "half";
            mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
            mo->cut_id.init();
            mo->cut_recipe = in;
            src.add_default_instances();
        }
        const std::string file = recipe_tmp_file(std::string("edgeslicer_cut_recut_") + c.name + ".3mf");
        Model             back;
        recipe_round_trip(src, back, file);
        REQUIRE(back.objects.size() == 1);
        REQUIRE(back.objects.front()->cut_recipe.has_value());

        const auto after = perform(*back.objects.front()->cut_recipe);
        REQUIRE(after.size() == before.size());

        // The halves come back the same. Compared by volume and bounding box
        // within a tolerance, because the boolean is floating point and an exact
        // mesh comparison would be testing the arithmetic rather than the recipe.
        for (size_t i = 0; i < before.size(); ++i) {
            INFO("half " << i);
            REQUIRE(after[i].first == Approx(before[i].first).epsilon(1e-6));
            REQUIRE(after[i].second.min.isApprox(before[i].second.min, 1e-6));
            REQUIRE(after[i].second.max.isApprox(before[i].second.max, 1e-6));
        }

        if (!std::getenv("SNORCA_RECIPE_KEEP"))
            boost::filesystem::remove(file);
    }
}

// ---------------------------------------------------------------------------
// 4. The opt-out.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: with no recipe nothing is written, and the file loads as before", "[CutRecipe]")
{
    // Exactly what "Keep cut editable" OFF leaves in the model: cut halves with
    // a cut_id and no recipe. Nothing about the 3MF should change.
    Model src;
    for (int half = 0; half < 2; ++half) {
        ModelObject *mo = src.add_object();
        mo->name        = std::string("half_") + std::to_string(half);
        mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
        mo->cut_id.init();
    }
    src.add_default_instances();

    const std::string file = recipe_tmp_file("edgeslicer_cut_recipe_none.3mf");
    Model             back;
    recipe_round_trip(src, back, file);

    REQUIRE_FALSE(recipe_has_entry(file, "Metadata/cut_recipe.xml"));
    REQUIRE(recipe_count_mesh_blobs(file) == 0);

    REQUIRE(back.objects.size() == 2);
    for (const ModelObject *mo : back.objects) {
        REQUIRE_FALSE(mo->cut_recipe.has_value());
        REQUIRE_FALSE(mo->has_cut_recipe());
        // The cut itself is untouched: this is still a cut object, exactly as it
        // was before the feature existed.
        REQUIRE(mo->is_cut());
    }

    if (!std::getenv("SNORCA_RECIPE_KEEP"))
        boost::filesystem::remove(file);
}

// ---------------------------------------------------------------------------
// 5. A recipe whose mesh blob is missing is dropped, not half-applied.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a recipe with no stored mesh is not offered for editing", "[CutRecipe]")
{
    // valid() is what the menu item and arm_reedit() gate on. A recipe with no
    // mesh cannot reproduce the cut, so it must not claim it can.
    CutRecipe r = recipe_make(CutRecipeKind::Plane);
    REQUIRE(r.valid());

    r.mesh = TriangleMesh();
    REQUIRE_FALSE(r.has_mesh());
    REQUIRE_FALSE(r.valid());

    // Same for a schema this build does not know.
    CutRecipe future = recipe_make(CutRecipeKind::Plane);
    future.version   = CutRecipeVersion + 1;
    REQUIRE_FALSE(future.valid());

    // ... and for a curved recipe whose sheet does not add up, which is what a
    // truncated value list would produce.
    CutRecipe bent = recipe_make(CutRecipeKind::Curved);
    REQUIRE(bent.valid());
    bent.sheet.values.pop_back();
    REQUIRE_FALSE(bent.sheet.valid());
    REQUIRE_FALSE(bent.valid());
}
// ===========================================================================
// THE CHAIN (recipe version 2, 2026-09-12). The drawn line became a chain of
// strokes; the recipe grew the per-stroke ranges, and a version 1 recipe still
// loads as a chain of one stroke.
// ===========================================================================

TEST_CASE("Cut recipe: a chain round-trips through the stored stroke", "[CutRecipe]")
{
    // Build a chain the way the gizmo does: three strokes, one of them onto the FRONT
    // (which is where the stored order and the drawn order part company).
    auto run = [](const Vec3d& a, const Vec3d& b, int n) {
        std::vector<DrawCutSample> out;
        for (int i = 0; i < n; ++ i) {
            DrawCutSample s;
            s.pos    = a + (double(i) / double(n - 1)) * (b - a);
            s.normal = Vec3d::UnitZ();
            s.facet  = size_t(i);
            out.push_back(s);
        }
        return out;
    };

    const double r = 2.0;
    DrawCutChain chain;
    REQUIRE(chain.append(run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), 11), r) == DrawChainEnd::Back);
    REQUIRE(chain.append(run(Vec3d(10, 0, 0), Vec3d(10, 10, 0), 11), r) == DrawChainEnd::Back);
    REQUIRE(chain.append(run(Vec3d(0, 0, 0), Vec3d(0, -10, 0), 11), r) == DrawChainEnd::Front);
    REQUIRE(chain.size() == 31);
    REQUIRE(chain.stroke_count() == 3);

    const CutRecipeStroke stored = cut_recipe_stroke_from_chain(chain, 0.35);
    REQUIRE(stored.samples.size() == 31);
    REQUIRE(stored.smoothing == Approx(0.35));
    REQUIRE(stored.stroke_bounds.size() == 3);
    // SORTED BY POSITION, because that is the order the replay walks the sample list
    // in - the drawn order (which put the front append last) cannot be replayed.
    REQUIRE(stored.stroke_bounds[0].first == 0);
    for (size_t i = 1; i < stored.stroke_bounds.size(); ++ i)
        REQUIRE(stored.stroke_bounds[i].first == stored.stroke_bounds[i - 1].second);
    REQUIRE(stored.stroke_bounds.back().second == 31);

    DrawCutChain back;
    cut_recipe_stroke_to_chain(stored, back);
    // THE SAMPLES AND THEIR ORDER SURVIVE EXACTLY, which is what the cut is made from.
    REQUIRE(back.size() == chain.size());
    for (size_t i = 0; i < back.size(); ++ i)
        REQUIRE(back.samples()[i].pos.isApprox(chain.samples()[i].pos));
    REQUIRE(back.front_pos().isApprox(chain.front_pos()));
    REQUIRE(back.back_pos().isApprox(chain.back_pos()));
    // And so does the stroke split, so undo still works per stroke after a reopen.
    REQUIRE(back.stroke_count() == 3);
    REQUIRE(back.undo_last_stroke());
    REQUIRE(back.size() < chain.size());
}

TEST_CASE("Cut recipe: a version 1 stroke loads as a chain of one stroke", "[CutRecipe]")
{
    // A version 1 recipe carries samples and the closed flag and NOTHING about strokes
    // - which is exactly what an empty stroke_bounds means. It has to load, because the
    // samples were always the whole description of the line.
    CutRecipeStroke v1 = recipe_straight_stroke();
    REQUIRE(v1.stroke_bounds.empty());

    DrawCutChain chain;
    cut_recipe_stroke_to_chain(v1, chain);
    REQUIRE(chain.size() == v1.samples.size());
    REQUIRE(chain.stroke_count() == 1);
    REQUIRE_FALSE(chain.is_closed());
    for (size_t i = 0; i < chain.size(); ++ i)
        REQUIRE(chain.samples()[i].pos.isApprox(v1.samples[i].pos));

    // A version 1 recipe is still `valid()`, so "Edit cut" stays available on every
    // project saved before the chain landed.
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.version   = 1;
    r.stroke    = v1;
    REQUIRE(cut_recipe_version_supported(1));
    REQUIRE(cut_recipe_version_supported(CutRecipeVersion));
    REQUIRE_FALSE(cut_recipe_version_supported(CutRecipeVersion + 1));
    REQUIRE(r.valid());
}

// ---------------------------------------------------------------------------
// VERSION 3 (2026-09-13): the drawn cut's surface model changed, and with it the
// MEANING of two stored numbers. A version 2 recipe still loads, but it cannot
// re-cut to the same halves, so migrate_draw_v2() maps it onto the nearest
// phase-3 cut rather than leaving numbers that mean something else.
// ---------------------------------------------------------------------------

TEST_CASE("Cut recipe: a version 2 drawn recipe migrates to the flat-core model", "[CutRecipe]")
{
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.version          = 2;
    r.draw_angle_deg   = 0.0;    // a v2 draft of 0: the ruling straight down the normal
    r.draw_depth       = 40.0;   // a v2 reach through the part
    r.draw_through_all = true;   // the v2 default, and the broken case

    REQUIRE(cut_recipe_version_supported(2));
    r.migrate_draw_v2();

    // Stamped forward, so nothing downstream sees two meanings.
    REQUIRE(r.version == CutRecipeVersion);
    // The migration is a version <= 2 story and stamps whatever the current version
    // is; it does not pin that number. (It was pinned to 3 while 3 was current, and
    // the version-4 bump - the extension angle - is a pure ADDITION that no v2 recipe
    // needs migrating for, which is exactly why this assertion had to go.)
    REQUIRE(CutRecipeVersion >= 3);

    // A v2 draft of 0 was a straight-down ruling, which IS a 90-degree straight wall
    // in the new model.
    REQUIRE(r.draw_angle_deg == Approx(90.0));
    // The old reach is meaningless as a band travel; the phase-3 default replaces it.
    REQUIRE(r.draw_depth == Approx(3.0));
    // And through-all is turned off, or the migrated cut would faithfully reproduce
    // the "projects through the object at random angles" cut this release fixes.
    REQUIRE_FALSE(r.draw_through_all);

    // The migrated recipe is still re-cuttable.
    REQUIRE(r.valid());

    // The parameters it hands DrawCut agree with what it stored.
    const DrawCutParams p = r.draw_params();
    REQUIRE(p.angle_deg == Approx(90.0));
    REQUIRE(p.depth == Approx(3.0));
    REQUIRE_FALSE(p.through_all);
}

TEST_CASE("Cut recipe: migration maps a v2 draft onto the nearest lip angle", "[CutRecipe]")
{
    // |draft| is how far the old ruling leaned OFF the surface normal, so the closest
    // phase-3 surface is a lip of 90 - |draft|. The sign is dropped because the lip
    // angle is unsigned - a v2 flare and a v2 undercut of the same size both become
    // the same lip, which is the best a one-way map can do.
    auto migrated_angle = [](double v2_angle) {
        CutRecipe r = recipe_make(CutRecipeKind::Drawn);
        r.version        = 2;
        r.draw_angle_deg = v2_angle;
        r.migrate_draw_v2();
        return r.draw_angle_deg;
    };

    REQUIRE(migrated_angle(  0.0) == Approx(90.0));
    REQUIRE(migrated_angle( 30.0) == Approx(60.0));
    REQUIRE(migrated_angle(-30.0) == Approx(60.0));
    REQUIRE(migrated_angle( 60.0) == Approx(30.0));
    // Clamped into the lip angle's own range whatever the old value was.
    REQUIRE(migrated_angle(120.0) >= 0.0);
    REQUIRE(migrated_angle(120.0) <= 90.0);
}

TEST_CASE("Cut recipe: migration leaves a flat or curved recipe alone", "[CutRecipe]")
{
    // Only the DRAWN cut's meanings changed. A flat or curved recipe is carried
    // forward untouched and just gets the new stamp - migrating its numbers would
    // corrupt a cut that was perfectly well described.
    for (CutRecipeKind kind : { CutRecipeKind::Plane, CutRecipeKind::Curved }) {
        CutRecipe r = recipe_make(kind);
        r.version          = 2;
        r.draw_angle_deg   = 12.5;
        r.draw_depth       = 40.0;
        r.draw_through_all = true;

        r.migrate_draw_v2();

        REQUIRE(r.version == CutRecipeVersion);
        REQUIRE(r.draw_angle_deg == Approx(12.5));
        REQUIRE(r.draw_depth == Approx(40.0));
        REQUIRE(r.draw_through_all);
    }
}

TEST_CASE("Cut recipe: migrating an already-current recipe is a no-op", "[CutRecipe]")
{
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.version          = CutRecipeVersion;
    r.draw_angle_deg   = 45.0;
    r.draw_depth       = 5.0;
    r.draw_through_all = true;

    r.migrate_draw_v2();

    REQUIRE(r.version == CutRecipeVersion);
    REQUIRE(r.draw_angle_deg == Approx(45.0));
    REQUIRE(r.draw_depth == Approx(5.0));
    REQUIRE(r.draw_through_all);
}

// ---------------------------------------------------------------------------
// VERSION 4 (2026-09-15): the extension angle. A pure ADDITION - no stored number
// changed meaning - so a version <= 3 recipe re-cuts to exactly the same halves,
// and the only thing to prove is that "unset" survives as unset rather than
// decaying to a zero that would aim the skirt somewhere the old cut never did.
// ---------------------------------------------------------------------------

TEST_CASE("Cut recipe: an old recipe's extension angle is unset, not zero", "[CutRecipe]")
{
    // ZERO IS A REAL EXTENSION ANGLE - it lays the skirt flat, level with the middle
    // of the cut - so "the user never set one" cannot be spelt as zero. A version 3
    // recipe loaded by this build has to come back with the flag OFF, which
    // draw_params() then turns into an EMPTY optional, which DrawCut reads as
    // "continue the band": the skirt the cut was actually made with.
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.version            = 3;
    r.draw_angle_deg     = 35.0;
    r.draw_ext_angle_set = false;
    r.draw_ext_angle_deg = 0.0;

    REQUIRE(cut_recipe_version_supported(3));

    const DrawCutParams p = r.draw_params();
    REQUIRE_FALSE(p.extension_angle_deg.has_value());
    // And "continue the band" resolves to the band's own angle, so moving Angle moves
    // the skirt with it - which a latched number would not do.
    REQUIRE(draw_cut_extension_angle(p) == Approx(35.0));

    // Setting one makes it independent.
    r.draw_ext_angle_set = true;
    r.draw_ext_angle_deg = 0.0;
    const DrawCutParams q = r.draw_params();
    REQUIRE(q.extension_angle_deg.has_value());
    REQUIRE(draw_cut_extension_angle(q) == Approx(0.0));

    // The two are DIFFERENT recipes, which operator== has to see - otherwise an
    // undo step could swap one for the other silently.
    CutRecipe unset = r;
    unset.draw_ext_angle_set = false;
    REQUIRE(r != unset);
}

TEST_CASE("Cut recipe: version 4 is current and a version 3 recipe still loads", "[CutRecipe]")
{
    REQUIRE(CutRecipeVersion == 4);
    REQUIRE(cut_recipe_version_supported(3));
    REQUIRE(cut_recipe_version_supported(4));
    REQUIRE_FALSE(cut_recipe_version_supported(5));

    // migrate_draw_v2() stamps a version 3 recipe forward WITHOUT touching its
    // numbers: version 4 changed no meanings, only added a field, so the angle, the
    // depth and through-all all survive a migration that only bumps the stamp.
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.version          = 3;
    r.draw_angle_deg   = 35.0;
    r.draw_depth       = 4.5;
    r.draw_through_all = true;
    r.migrate_draw_v2();
    REQUIRE(r.version == 4);
    REQUIRE(r.draw_angle_deg == Approx(35.0));
    REQUIRE(r.draw_depth == Approx(4.5));
    REQUIRE(r.draw_through_all);
}

TEST_CASE("Cut recipe: a negative lip angle is storable and survives", "[CutRecipe]")
{
    // OWNER ITEM 5: the lip angle is signed now, so a recipe has to be able to carry
    // the mirrored band. The old range was 0..90 and a clamp anywhere in the chain
    // would quietly turn the user's -40 into 0.
    CutRecipe r = recipe_make(CutRecipeKind::Drawn);
    r.draw_angle_deg = -40.0;

    const DrawCutParams p = r.draw_params();
    REQUIRE(p.angle_deg == Approx(-40.0));

    // And the band really leans the other way at that value.
    const Vec3d inward = Vec3d::UnitX();
    const Vec3d n      = Vec3d::UnitZ();
    const Vec3d d      = draw_cut_band_dir(inward, n, p.angle_deg);
    REQUIRE(d.dot(n) > 0.0);   // UP, towards the side the loop was drawn on
    REQUIRE(draw_cut_band_dir(inward, n, 40.0).dot(n) < 0.0);   // and DOWN at +40
}

TEST_CASE("Cut recipe: bounds that do not tile the samples fall back to one stroke", "[CutRecipe]")
{
    // A chain whose ranges disagree with its samples would corrupt undo in a way the
    // user cannot see coming, so a malformed set is DISCARDED rather than half-applied.
    CutRecipeStroke st = recipe_straight_stroke();
    const size_t n = st.samples.size();

    auto loads_as_one = [&](std::vector<std::pair<uint32_t, uint32_t>> bounds) {
        st.stroke_bounds = std::move(bounds);
        DrawCutChain c;
        cut_recipe_stroke_to_chain(st, c);
        REQUIRE(c.size() == n);
        return c.stroke_count() == 1;
    };

    REQUIRE(loads_as_one({ { 0, 5 }, { 7, uint32_t(n) } }));            // a gap
    REQUIRE(loads_as_one({ { 0, 10 }, { 5, uint32_t(n) } }));           // an overlap
    REQUIRE(loads_as_one({ { 3, uint32_t(n) } }));                      // not starting at 0
    REQUIRE(loads_as_one({ { 0, uint32_t(n) + 5 } }));                  // past the end
    REQUIRE(loads_as_one({ { 0, 10 } }));                               // short of the end
    // A well-formed pair really does keep both strokes, so the fallback is not simply
    // firing every time.
    st.stroke_bounds = { { 0, 10 }, { 10, uint32_t(n) } };
    DrawCutChain ok;
    cut_recipe_stroke_to_chain(st, ok);
    REQUIRE(ok.size() == n);
    REQUIRE(ok.stroke_count() == 2);
}
TEST_CASE("Cut recipe: the open-line verdict round-trips, and version 1 keeps its cut", "[CutRecipe]")
{
    auto run = [](const Vec3d& a, const Vec3d& b, int n) {
        std::vector<DrawCutSample> out;
        for (int i = 0; i < n; ++ i) {
            DrawCutSample s;
            s.pos    = a + (double(i) / double(n - 1)) * (b - a);
            s.normal = Vec3d::UnitZ();
            out.push_back(s);
        }
        return out;
    };

    DrawCutChain chain;
    REQUIRE(chain.append(run(Vec3d(-20, 0, 20), Vec3d(20, 0, 20), 41), 2.0) == DrawChainEnd::Back);
    REQUIRE(chain.finish_open());

    const CutRecipeStroke stored = cut_recipe_stroke_from_chain(chain, 0.2);
    REQUIRE(stored.finished_open);
    REQUIRE_FALSE(stored.closed);

    DrawCutChain back;
    cut_recipe_stroke_to_chain(stored, back);
    REQUIRE(back.is_finished_open());
    REQUIRE_FALSE(back.is_closed());
    // Which is what makes a reopened open cut reproduce a stroke at all.
    DrawCutStroke st;
    REQUIRE(back.finish(st, 1.0, 0.0) == DrawCutError::None);
    REQUIRE_FALSE(st.is_closed());

    // A VERSION 1 recipe has no flag, and its line WAS cut with - so an unclosed one is
    // finished-open whether or not the flag says so. Without this a version 1 open cut
    // would reopen with no surface and no way to tell why.
    CutRecipeStroke v1 = recipe_straight_stroke();
    REQUIRE_FALSE(v1.finished_open);
    REQUIRE_FALSE(v1.closed);
    DrawCutChain from_v1;
    cut_recipe_stroke_to_chain(v1, from_v1);
    REQUIRE(from_v1.is_finished_open());
    DrawCutStroke st_v1;
    REQUIRE(from_v1.finish(st_v1, 1.0, 0.2) == DrawCutError::None);
    REQUIRE(st_v1.valid());

    // A CLOSED stored stroke is not touched by any of that.
    CutRecipeStroke closed = stored;
    closed.closed        = true;
    closed.finished_open = false;
    DrawCutChain c2;
    cut_recipe_stroke_to_chain(closed, c2);
    REQUIRE(c2.is_closed());
    REQUIRE_FALSE(c2.is_finished_open());
}

// ===========================================================================
// MIRRORING A RECIPE.
//
// Step 1 of the "Copy cut to..." + Mirror-buttons feature: the pure function the
// panel's Mirror on X / Y / Z buttons and the copy-cut arm path both go through.
// The contract it has to hold, and what each of these pins:
//
//   * mirroring twice on the same axis about the same pivot is the IDENTITY, for
//     every CutRecipeKind - which is what makes the buttons as freely reversible
//     as "Flip cut plane" is;
//   * rotation_m stays a PROPER rotation (orthonormal, determinant +1) - the
//     handedness trap, and the reason the reflected frame is never stored;
//   * the plane NORMAL is genuinely reflected, so the mirrored plane is the
//     mirror of the original and upper/lower does not secretly swap;
//   * a connector on +X lands on -X, and its type NEVER changes;
//   * the sheet grid and the stroke samples take the one fixed plane-local
//     re-indexing, on a NON-SQUARE grid where getting nx and ny the wrong way
//     round would read off the end.
// ===========================================================================

using Catch::Matchers::WithinAbs;

static const Vec3d MPIVOT = Vec3d(10.0, -5.0, 2.5);

static bool mirror_rot_is_proper(const Transform3d& t)
{
    const Matrix3d R = t.linear();
    if (std::abs(R.determinant() - 1.0) > 1e-9)
        return false;
    const Matrix3d I = R * R.transpose();
    return (I - Matrix3d::Identity()).cwiseAbs().maxCoeff() < 1e-9;
}

// An oblique frame: not axis aligned on any axis, so the orthonormality and
// determinant checks have something real to fail on.
static Transform3d mirror_oblique_rotation()
{
    Transform3d rot = Transform3d::Identity();
    rot.rotate(Eigen::AngleAxisd(0.7, Vec3d(0.3, -0.8, 0.5).normalized()));
    rot.rotate(Eigen::AngleAxisd(-0.4, Vec3d::UnitX()));
    return rot;
}

// A 7 x 3 grid, deliberately NOT square and deliberately not symmetric in v, so
// the row mirroring is observable and an nx/ny mix-up is out of bounds rather
// than invisible. Value at (i, j) encodes both indices.
static CutRecipeSheet mirror_rect_sheet()
{
    CutRecipeSheet s;
    s.nx = 7;
    s.ny = 3;
    s.half_size_u = 30.0;
    s.half_size_v = 12.0;
    s.values.assign(size_t(s.nx) * size_t(s.ny), 0.0);
    for (int j = 0; j < s.ny; ++ j)
        for (int i = 0; i < s.nx; ++ i)
            s.values[size_t(j) * size_t(s.nx) + size_t(i)] = double(i) + 100.0 * double(j);
    return s;
}

TEST_CASE("Cut recipe: the mirror is its own inverse, for every kind", "[CutRecipe][CutMirror]")
{
    const CutMirrorAxis axes[3] = { CutMirrorAxis::X, CutMirrorAxis::Y, CutMirrorAxis::Z };
    const char*         names[3] = { "X", "Y", "Z" };
    const CutRecipeKind kinds[4] = { CutRecipeKind::Plane, CutRecipeKind::Curved,
                                     CutRecipeKind::Drawn, CutRecipeKind::Groove };
    const char*         kind_names[4] = { "Plane", "Curved", "Drawn", "Groove" };

    for (int k = 0; k < 4; ++ k) {
        for (int a = 0; a < 3; ++ a) {
            DYNAMIC_SECTION("kind " << kind_names[k] << " axis " << names[a]) {
                CutRecipe src = recipe_make(kinds[k]);
                // The oblique frame, so this is not merely the axis-aligned case.
                src.rotation_m = mirror_oblique_rotation();
                if (kinds[k] == CutRecipeKind::Curved)
                    src.sheet = mirror_rect_sheet();

                const CutRecipe once  = cut_recipe_mirrored(src,  axes[a], MPIVOT);
                const CutRecipe twice = cut_recipe_mirrored(once, axes[a], MPIVOT);

                // The frame comes back to where it started. Not bit-exact - the
                // rotation goes through a Gram-Schmidt each way - so a tolerance,
                // but a tight one.
                REQUIRE_THAT((twice.plane_center - src.plane_center).norm(), WithinAbs(0.0, 1e-9));
                REQUIRE_THAT((twice.rotation_m.linear() - src.rotation_m.linear()).cwiseAbs().maxCoeff(),
                             WithinAbs(0.0, 1e-9));

                // The surface comes back exactly: the sheet re-indexing and the
                // sample sign flips are both exact arithmetic, no evaluation.
                REQUIRE(twice.sheet.values == src.sheet.values);
                REQUIRE(twice.sheet.nx == src.sheet.nx);
                REQUIRE(twice.sheet.ny == src.sheet.ny);
                REQUIRE(twice.stroke == src.stroke);
                REQUIRE(twice.groove == src.groove);
                REQUIRE(twice.draw_view_dir == src.draw_view_dir);

                // The connectors, including the z_angle double negation.
                REQUIRE(twice.connectors.size() == src.connectors.size());
                for (size_t i = 0; i < src.connectors.size(); ++ i) {
                    REQUIRE_THAT((twice.connectors[i].pos - src.connectors[i].pos).norm(),
                                 WithinAbs(0.0, 1e-9));
                    REQUIRE(twice.connectors[i].z_angle == src.connectors[i].z_angle);
                    REQUIRE(twice.connectors[i].type  == src.connectors[i].type);
                    REQUIRE(twice.connectors[i].shape == src.connectors[i].shape);
                }

                // The after-cut attributes are never touched by a mirror at all.
                REQUIRE(twice.keep_upper == src.keep_upper);
                REQUIRE(twice.keep_lower == src.keep_lower);
                REQUIRE(twice.upper_visibility == src.upper_visibility);
                REQUIRE(twice.lower_visibility == src.lower_visibility);
            }
        }
    }
}

TEST_CASE("Cut recipe: a mirrored rotation is still a rotation", "[CutRecipe][CutMirror]")
{
    // THE HANDEDNESS TRAP. Naively reflecting the 3x3 would give determinant -1,
    // and every its_transform() and Transformation() downstream would silently
    // produce inside-out geometry. Pinned on an oblique frame, because the
    // axis-aligned case can pass by accident.
    const CutMirrorAxis axes[3] = { CutMirrorAxis::X, CutMirrorAxis::Y, CutMirrorAxis::Z };
    const char*         names[3] = { "X", "Y", "Z" };

    for (int a = 0; a < 3; ++ a) {
        DYNAMIC_SECTION("axis " << names[a]) {
            const Transform3d src = mirror_oblique_rotation();
            REQUIRE(mirror_rot_is_proper(src));

            const Transform3d out = cut_mirror_rotation(src, axes[a]);
            REQUIRE(mirror_rot_is_proper(out));

            // THE NORMAL IS GENUINELY REFLECTED. This is what says the mirrored
            // plane is the mirror of the original plane rather than merely some
            // other plane through the mirrored centre - and it is also why no
            // upper/lower swap is needed: the side that was upper still is.
            const Vec3d n_src = src.linear() * Vec3d::UnitZ();
            const Vec3d n_out = out.linear() * Vec3d::UnitZ();
            const Vec3d want  = n_src.cwiseProduct(cut_mirror_vector(axes[a]));
            REQUIRE_THAT((n_out - want).norm(), WithinAbs(0.0, 1e-9));

            // THE PLANE-LOCAL RESIDUAL IS diag(1, -1, 1). The whole (u,v)
            // re-indexing question turns on this: because the residual is always
            // this one map, the sheet always mirrors its ROWS and the stroke always
            // negates local y, for every axis and every starting rotation. If this
            // ever fails, the sheet and stroke handling in cut_recipe_mirrored()
            // is wrong and needs the case analysis this construction avoids.
            Matrix3d M = Matrix3d::Identity();
            M.diagonal() = cut_mirror_vector(axes[a]);
            const Matrix3d Q = out.linear().transpose() * M * src.linear();
            Matrix3d want_Q = Matrix3d::Identity();
            want_Q(1, 1) = -1.0;
            REQUIRE_THAT((Q - want_Q).cwiseAbs().maxCoeff(), WithinAbs(0.0, 1e-9));
        }
    }

    // A degenerate frame must not come back non-orthonormal. The gizmo should
    // never hand one over, but "should never" is not "cannot".
    Transform3d degenerate = Transform3d::Identity();
    degenerate.matrix().block(0, 0, 3, 3) = Matrix3d::Zero();
    REQUIRE(mirror_rot_is_proper(cut_mirror_rotation(degenerate, CutMirrorAxis::X)));
}

TEST_CASE("Cut recipe: a connector on +X lands on -X and keeps its type", "[CutRecipe][CutMirror]")
{
    CutRecipe src = recipe_make(CutRecipeKind::Plane);
    src.connectors.clear();

    // Mirroring about the ORIGIN here, so the arithmetic is readable: +X goes to
    // -X and nothing else moves.
    CutRecipeConnector plug;
    plug.pos     = Vec3d(12.0, 3.0, -1.0);
    plug.z_angle = 0.4f;
    plug.type    = int(CutConnectorType::Plug);
    plug.shape   = int(CutConnectorShape::Triangle);
    src.connectors.push_back(plug);

    CutRecipeConnector dowel = plug;
    dowel.pos   = Vec3d(-8.0, 1.0, 2.0);
    dowel.type  = int(CutConnectorType::Dowel);
    dowel.shape = int(CutConnectorShape::Circle);
    src.connectors.push_back(dowel);

    CutRecipeConnector flexi = plug;
    flexi.pos   = Vec3d(0.0, 6.0, 0.0);
    flexi.type  = int(CutConnectorType::FlexiJoint);
    flexi.shape = int(CutConnectorShape::Circle);
    src.connectors.push_back(flexi);

    const CutRecipe out = cut_recipe_mirrored(src, CutMirrorAxis::X, Vec3d::Zero());

    REQUIRE_THAT(out.connectors[0].pos.x(), WithinAbs(-12.0, 1e-12));
    REQUIRE_THAT(out.connectors[0].pos.y(), WithinAbs(  3.0, 1e-12));
    REQUIRE_THAT(out.connectors[0].pos.z(), WithinAbs( -1.0, 1e-12));
    REQUIRE_THAT(out.connectors[1].pos.x(), WithinAbs(  8.0, 1e-12));

    // TYPE IS A MATING ROLE, NEVER A CHIRALITY. A mirrored assembly still needs
    // one plug and one dowel to mate - which is precisely the use case the owner
    // wants a mirrored connector layout for - so a mirror must never toggle it.
    REQUIRE(out.connectors[0].type == int(CutConnectorType::Plug));
    REQUIRE(out.connectors[1].type == int(CutConnectorType::Dowel));
    REQUIRE(out.connectors[2].type == int(CutConnectorType::FlexiJoint));
    // Nor the style, the shape, the radii or the tolerances.
    REQUIRE(out.connectors[0].shape  == int(CutConnectorShape::Triangle));
    REQUIRE(out.connectors[0].radius == src.connectors[0].radius);
    REQUIRE(out.connectors[0].height == src.connectors[0].height);

    // z_angle: negated for a shape that HAS an orientation, untouched for a
    // Circle, where it means nothing.
    REQUIRE_THAT(double(out.connectors[0].z_angle), WithinAbs(-0.4, 1e-7));
    REQUIRE_THAT(double(out.connectors[1].z_angle), WithinAbs( 0.4, 1e-7));

    // Mirroring about a non-zero pivot moves the point the other way about it.
    const CutRecipe off = cut_recipe_mirrored(src, CutMirrorAxis::X, Vec3d(10.0, 0.0, 0.0));
    REQUIRE_THAT(off.connectors[0].pos.x(), WithinAbs(8.0, 1e-12));   // 10 - (12 - 10)
}

TEST_CASE("Cut recipe: the sheet grid mirrors its rows, not its values", "[CutRecipe][CutMirror]")
{
    CutRecipe src = recipe_make(CutRecipeKind::Curved);
    src.sheet = mirror_rect_sheet();   // 7 x 3, value(i,j) = i + 100*j

    const CutRecipe out = cut_recipe_mirrored(src, CutMirrorAxis::Y, MPIVOT);

    REQUIRE(out.sheet.nx == 7);
    REQUIRE(out.sheet.ny == 3);
    // The extent does not move: the grid is symmetric in v about its own centre,
    // so mirrored indices land exactly on grid points.
    REQUIRE(out.sheet.half_size_u == src.sheet.half_size_u);
    REQUIRE(out.sheet.half_size_v == src.sheet.half_size_v);

    // Row j came from row (ny - 1 - j), and the VALUES ARE NOT NEGATED. That last
    // point is the one that separates this from CurvedCutSheet::flip_about_u(),
    // which is the 180-degree frame TURN and does negate them: local z is untouched
    // by a mirror's plane-local residual, so the heights are unchanged.
    for (int j = 0; j < 3; ++ j)
        for (int i = 0; i < 7; ++ i)
            REQUIRE_THAT(out.sheet.values[size_t(j) * 7 + size_t(i)],
                         WithinAbs(double(i) + 100.0 * double(2 - j), 1e-12));

    // Not a value negation anywhere: every entry is still non-negative, as every
    // source entry was.
    for (double v : out.sheet.values)
        REQUIRE(v >= 0.0);

    // The same 7 x 3 sheet through the OTHER two axes gives the same answer - the
    // residual does not depend on the axis, which is the whole point of the fixed
    // column choice. An nx/ny mix-up would be out of bounds on this grid.
    for (CutMirrorAxis a : { CutMirrorAxis::X, CutMirrorAxis::Z }) {
        const CutRecipe o = cut_recipe_mirrored(src, a, MPIVOT);
        REQUIRE(o.sheet.values == out.sheet.values);
    }
}

TEST_CASE("Cut recipe: stroke samples reflect in the plane frame", "[CutRecipe][CutMirror]")
{
    CutRecipe src = recipe_make(CutRecipeKind::Drawn);
    src.draw_view_dir = Vec3d(0.2, 0.6, -0.8);

    const CutRecipe out = cut_recipe_mirrored(src, CutMirrorAxis::Z, MPIVOT);

    REQUIRE(out.stroke.samples.size() == src.stroke.samples.size());
    for (size_t i = 0; i < src.stroke.samples.size(); ++ i) {
        const DrawCutSample& a = src.stroke.samples[i];
        const DrawCutSample& b = out.stroke.samples[i];
        // Local y negates; local x and local z do not. The samples are stored in
        // the CUT PLANE's frame (draw_view_dir_in_plane() takes even the camera
        // direction into that frame before storing it), so they see the residual
        // and never the world reflection.
        REQUIRE_THAT(b.pos.x(), WithinAbs( a.pos.x(), 1e-12));
        REQUIRE_THAT(b.pos.y(), WithinAbs(-a.pos.y(), 1e-12));
        REQUIRE_THAT(b.pos.z(), WithinAbs( a.pos.z(), 1e-12));
        REQUIRE_THAT(b.normal.y(), WithinAbs(-a.normal.y(), 1e-12));
        // The sample ORDER is untouched, so the stored stroke ranges stay valid.
        REQUIRE(b.facet == a.facet);
    }
    REQUIRE(out.stroke.stroke_bounds == src.stroke.stroke_bounds);
    REQUIRE(out.stroke.closed == src.stroke.closed);
    REQUIRE(out.stroke.smoothing == src.stroke.smoothing);

    // The view direction is a direction in that same frame.
    REQUIRE_THAT(out.draw_view_dir.x(), WithinAbs( 0.2, 1e-12));
    REQUIRE_THAT(out.draw_view_dir.y(), WithinAbs(-0.6, 1e-12));
    REQUIRE_THAT(out.draw_view_dir.z(), WithinAbs(-0.8, 1e-12));

    // The sweep parameters are scalars and pass through.
    REQUIRE(out.draw_extension == src.draw_extension);
    REQUIRE(out.draw_angle_deg == src.draw_angle_deg);
    REQUIRE(out.draw_depth == src.draw_depth);
    REQUIRE(out.draw_ext_angle_set == src.draw_ext_angle_set);
    REQUIRE(out.draw_direction == src.draw_direction);
}

TEST_CASE("Cut recipe: groove parameters are scalars and survive a mirror", "[CutRecipe][CutMirror]")
{
    const CutRecipe src = recipe_make(CutRecipeKind::Groove);

    for (CutMirrorAxis a : { CutMirrorAxis::X, CutMirrorAxis::Y, CutMirrorAxis::Z }) {
        const CutRecipe out = cut_recipe_mirrored(src, a, MPIVOT);
        // Not one field of CutRecipeGroove encodes a direction in the plane, so a
        // mirror moves the frame the groove sits on and nothing else.
        REQUIRE(out.groove == src.groove);
        REQUIRE(out.kind == CutRecipeKind::Groove);
        // ... and the frame really did move.
        REQUIRE(out.plane_center != src.plane_center);
    }
}

TEST_CASE("Cut recipe: the mirrored plane really is the mirror of the plane", "[CutRecipe][CutMirror]")
{
    // A point on the source plane, reflected, must lie on the mirrored plane. This
    // is the geometric statement the whole construction exists to make true, tested
    // independently of how rotation_m is built.
    CutRecipe src = recipe_make(CutRecipeKind::Plane);
    src.rotation_m   = mirror_oblique_rotation();
    src.plane_center = Vec3d(3.0, 7.0, -2.0);

    for (CutMirrorAxis a : { CutMirrorAxis::X, CutMirrorAxis::Y, CutMirrorAxis::Z }) {
        const CutRecipe out = cut_recipe_mirrored(src, a, MPIVOT);
        const Vec3d     n   = out.rotation_m.linear() * Vec3d::UnitZ();

        // Several points spread over the source plane, not just its centre.
        for (double u : { -20.0, 0.0, 13.5 })
            for (double v : { -9.0, 4.0 }) {
                const Vec3d on_src = src.plane_center + src.rotation_m.linear() * Vec3d(u, v, 0.0);
                const Vec3d want   = cut_mirror_point(on_src, a, MPIVOT);
                // Distance from the mirrored plane.
                REQUIRE_THAT(n.dot(want - out.plane_center), WithinAbs(0.0, 1e-9));
            }
    }
}

// ---------------------------------------------------------------------------
// "COPY CUT TO..." - how a target object is NAMED in the submenu.
//
// The owner cut two notches off a part of an assembled object and opened "Copy
// cut to...". The submenu listed "1. non clicker core.stl, 2. button non
// clicker.stl, 3-17. Assembly, 18. R Ring, 19-23. Assembly ..." - a dozen and a
// half entries all called "Assembly", because that is the name the assemble
// action gives everything it makes, and nothing in the list identified anything.
//
// cut_target_label() is the fix: the object's name, plus the names of the PARTS
// it is made of, which is what actually tells two assemblies apart. The target is
// still the whole object (the Cut gizmo works on a full instance), so the parts
// are a legend rather than a set of targets - which is why this returns one
// string per object.
// ---------------------------------------------------------------------------
namespace {
// An object with the given name and one model-part volume per part name.
ModelObject* cut_label_object(Model& model, const std::string& name, const std::vector<std::string>& parts)
{
    ModelObject* mo = model.add_object();
    mo->name        = name;
    for (const std::string& p : parts)
        mo->add_volume(TriangleMesh(recipe_centred_cube(10.0)))->name = p;
    return mo;
}
} // namespace

TEST_CASE("Copy cut: an assembly is named by its parts, not just \"Assembly\"", "[CutRecipe][CutCopyTarget]")
{
    Model model;

    // The owner's own case: several objects the assemble action called "Assembly",
    // which the old label made indistinguishable.
    ModelObject* a = cut_label_object(model, "Assembly", { "left ear", "right ear", "body" });
    ModelObject* b = cut_label_object(model, "Assembly", { "left foot", "right foot" });

    const std::string la = cut_target_label(a);
    const std::string lb = cut_target_label(b);

    // The whole point: two objects with the SAME name get different labels.
    REQUIRE(la != lb);
    // The object's own name is still there - it is what the object list shows.
    REQUIRE(la.find("Assembly") != std::string::npos);
    // ... and so are the parts that identify it.
    REQUIRE(la.find("left ear") != std::string::npos);
    REQUIRE(la.find("right ear") != std::string::npos);
    REQUIRE(la.find("body") != std::string::npos);
    REQUIRE(lb.find("left foot") != std::string::npos);
}

TEST_CASE("Copy cut: a single-part object is named by itself", "[CutRecipe][CutCopyTarget]")
{
    Model model;
    // "18. R Ring" in the owner's list - already unambiguous, and repeating the one
    // part's name after the object's would only add noise.
    ModelObject* mo = cut_label_object(model, "R Ring", { "R Ring" });
    REQUIRE(cut_target_label(mo) == "R Ring");

    // An object with no volumes at all is still pickable rather than blank.
    ModelObject* empty = cut_label_object(model, "lonely", {});
    REQUIRE(cut_target_label(empty) == "lonely");
}

TEST_CASE("Copy cut: a long part list is truncated rather than unreadable", "[CutRecipe][CutCopyTarget]")
{
    Model model;
    ModelObject* mo = cut_label_object(model, "Assembly",
                                       { "p1", "p2", "p3", "p4", "p5", "p6", "p7" });

    const std::string label = cut_target_label(mo, 3);
    // The first three are named ...
    REQUIRE(label.find("p1") != std::string::npos);
    REQUIRE(label.find("p3") != std::string::npos);
    // ... the rest are counted, not listed.
    REQUIRE(label.find("p4") == std::string::npos);
    REQUIRE(label.find("+4 more") != std::string::npos);
}

TEST_CASE("Copy cut: only model parts name an object", "[CutRecipe][CutCopyTarget]")
{
    Model        model;
    ModelObject* mo = model.add_object();
    mo->name        = "Assembly";
    mo->add_volume(TriangleMesh(recipe_centred_cube(10.0)))->name = "shell";
    mo->add_volume(TriangleMesh(recipe_centred_cube(5.0)))->name  = "pocket";
    // A negative volume is not what identifies an object to the eye, and listing it
    // would crowd out the names that do.
    mo->volumes.back()->set_type(ModelVolumeType::NEGATIVE_VOLUME);

    const std::string label = cut_target_label(mo);
    REQUIRE(label.find("pocket") == std::string::npos);
    // One model part left, so the object is named by itself.
    REQUIRE(label == "Assembly");
}

TEST_CASE("Copy cut: an unnamed object is still pickable", "[CutRecipe][CutCopyTarget]")
{
    Model        model;
    ModelObject* mo = cut_label_object(model, "", { "a", "b" });
    const std::string label = cut_target_label(mo);
    REQUIRE(!label.empty());
    REQUIRE(label.find("a") != std::string::npos);
}

TEST_CASE("Copy cut: a null object yields an empty label rather than a crash", "[CutRecipe][CutCopyTarget]")
{
    REQUIRE(cut_target_label(nullptr).empty());
}

// ---------------------------------------------------------------------------
// "EDIT CUT..." - the index alignment the re-edit's stand-in has to preserve.
//
// THE CRASH. GLGizmoCut3D::begin_reedit() swaps the cut halves for a stand-in
// carrying the pre-cut mesh. It used to ADD the stand-in to the model first and
// only then delete the halves, removing each from the model by index and from the
// object LIST by the same index. But Model::add_object() adds to the model alone -
// the list knows nothing about it - so between the two the model was one object
// LONGER than the list, and every delete_object_from_list(i) then took out the
// wrong row. On an object that was not the last on the plate the list and the
// model ended up disagreeing about what every later index meant, and the next
// thing to resolve a selection against them (ObjectList::part_selection_changed(),
// which indexes (*m_objects)[obj_idx] unguarded) took the slicer down.
//
// The fix is an ordering: delete the halves from model and list together FIRST,
// while the two still agree, then append the stand-in to BOTH.
//
// The object list is GUI and cannot be built here, but what the ordering has to
// guarantee is a statement about the MODEL alone: performing the deletions before
// the append leaves the surviving objects at the indices a parallel list would
// have reached by applying the same deletions, and the stand-in last. That is
// what this pins - the invariant the crash broke, in the form that can be tested
// without a gizmo.
// ---------------------------------------------------------------------------
TEST_CASE("Edit cut: the stand-in is appended after the halves are removed", "[CutRecipe][CutReedit]")
{
    Model model;
    // A plate where the cut object is NOT last - the arrangement that crashed. The
    // owner's project had the cut assembly at index 23 of 24, with objects after it
    // in the list order the halves were removed from.
    for (int i = 0; i < 5; ++ i) {
        ModelObject* mo = model.add_object();
        mo->name        = std::string("obj_") + std::to_string(i);
        mo->add_volume(TriangleMesh(recipe_centred_cube(10.0)))->name = "part";
    }
    // The halves of the cut being re-edited: indices 1 and 2.
    const ObjectID keep0 = model.objects[0]->id();
    const ObjectID keep3 = model.objects[3]->id();
    const ObjectID keep4 = model.objects[4]->id();

    std::vector<int> idxs{ 1, 2 };

    // What a parallel object list would do, given the same deletions: the same
    // indices, removed descending.
    std::vector<std::string> list_names;
    for (const ModelObject* o : model.objects)
        list_names.push_back(o->name);

    // THE ORDER THE FIX ESTABLISHES: delete first ...
    std::sort(idxs.begin(), idxs.end(), std::greater<int>());
    for (int i : idxs) {
        model.delete_object(size_t(i));
        list_names.erase(list_names.begin() + i);
    }
    // ... then append the stand-in.
    ModelObject* proxy = model.add_object();
    proxy->name        = "proxy";
    proxy->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
    list_names.push_back(proxy->name);

    // Model and the parallel list agree, index for index - the whole invariant.
    // Under the OLD order the stand-in went into the model BEFORE the deletions and
    // never into the list at all, so the two were already a different length when
    // the deletions ran and the list ended up permanently one short, with every
    // index past the deletions naming a different object in each.
    REQUIRE(model.objects.size() == list_names.size());
    for (size_t i = 0; i < model.objects.size(); ++ i)
        REQUIRE(model.objects[i]->name == list_names[i]);

    // The survivors are the ones that should have survived, by identity and not
    // merely by count.
    REQUIRE(model.objects[0]->id() == keep0);
    REQUIRE(model.objects[1]->id() == keep3);
    REQUIRE(model.objects[2]->id() == keep4);
    // ... and the stand-in is last, which is the index begin_reedit() selects it at.
    REQUIRE(model.objects.back() == proxy);
    REQUIRE(model.objects.size() == 4);
}

TEST_CASE("Edit cut: a stashed half is a CLONE with new ids, so the transform is copied out", "[CutRecipe][CutReedit]")
{
    // begin_reedit() places the stand-in at the first half's instance transform, and
    // the half does not survive the deletion that precedes the stand-in. The obvious
    // repair - look the half up in m_reedit_stash afterwards, by ObjectID - DOES NOT
    // WORK, and this pins why: Model::add_object(const ModelObject&) goes through
    // new_clone(), whose assign_clone() calls assign_new_unique_ids_recursive() and
    // asserts the result's id DIFFERS from the original's. A stashed half therefore
    // shares no id with the object it was taken from, and an id lookup would silently
    // find nothing and place the stand-in at the wrong transform (or the origin).
    //
    // So begin_reedit() copies the TRANSFORMATION out as plain data before deleting,
    // which is what this asserts is both necessary and sufficient.
    Model source;
    ModelObject* half = source.add_object();
    half->name        = "half_0";
    half->add_volume(TriangleMesh(recipe_centred_cube(10.0)))->name = "part";
    half->add_instance();
    half->instances.front()->set_offset(Vec3d(11.0, -4.0, 2.5));
    const ObjectID half_id = half->id();

    // The data begin_reedit() captures BEFORE the deletion.
    const Geometry::Transformation trafo = half->instances.front()->get_transformation();

    Model stash;
    ModelObject* copy = stash.add_object(*half);

    // NOT the same id - the trap the id lookup would have fallen into.
    REQUIRE(copy->id() != half_id);
    // The geometry does survive the clone, which is what makes the stash a valid
    // thing for Cancel to put back.
    REQUIRE(!copy->instances.empty());
    REQUIRE(copy->instances.front()->get_offset().isApprox(Vec3d(11.0, -4.0, 2.5)));

    // And the captured transformation reproduces the placement on a fresh instance,
    // which is exactly what the stand-in is given.
    Model        rebuilt;
    ModelObject* proxy = rebuilt.add_object();
    proxy->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
    ModelInstance* inst = proxy->add_instance();
    inst->set_transformation(trafo);
    REQUIRE(inst->get_offset().isApprox(Vec3d(11.0, -4.0, 2.5)));
}
