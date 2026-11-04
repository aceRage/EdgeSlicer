#include <catch2/catch.hpp>
#include <libslic3r/TacJoint.hpp>
#include <libslic3r/MeshBoolean.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>

using namespace Slic3r;

namespace {
Vec3d dimensions(const indexed_triangle_set& mesh)
{
    BoundingBoxf3 box;
    for (const Vec3f& v : mesh.vertices)
        box.merge(v.cast<double>());
    return box.size();
}
} // namespace

// Catches reconstruction, separate centering, incorrect length axis and dropped source facets.
TEST_CASE("TAC Joint defaults preserve both reference meshes in one rigid frame", "[TACJoint]")
{
    const Transform3d transform = tac_joint_source_transform();
    REQUIRE(transform.linear().determinant() == Approx(1.0).margin(1e-10));
    REQUIRE((transform.linear().transpose() * transform.linear() - Eigen::Matrix3d::Identity()).norm() < 1e-10);
    const auto connector = tac_joint_connector_mesh({});
    const auto cutter    = tac_joint_cutter_mesh({});
    for (const auto& entry : {std::make_pair("connector.stl", &connector), std::make_pair("connector cutter.stl", &cutter)}) {
        const std::string path = std::string(TEST_DATA_DIR) + "/../../resources/cut_connectors/tac_joint/" + entry.first;
        std::ifstream     source(path, std::ios::binary);
        REQUIRE(source.good());
        source.seekg(80);
        uint32_t count = 0;
        source.read(reinterpret_cast<char*>(&count), sizeof(count));
        REQUIRE(count == entry.second->indices.size());
        // Read the source's binary-STL triangle records directly. Comparing all
        // ordered vertices catches reconstruction or loss of relative alignment.
        for (size_t i = 0; i < count; ++i) {
            float    triangle[12];
            uint16_t attributes;
            source.read(reinterpret_cast<char*>(triangle), sizeof(triangle));
            source.read(reinterpret_cast<char*>(&attributes), sizeof(attributes));
            REQUIRE(source.good());
            for (int j = 0; j < 3; ++j) {
                const Vec3d expected = transform * Vec3d(triangle[3 + 3 * j], triangle[4 + 3 * j], triangle[5 + 3 * j]);
                const Vec3d actual   = entry.second->vertices[entry.second->indices[i][j]].cast<double>();
                REQUIRE((expected - actual).norm() < 2e-6);
            }
        }
    }
    const Vec3d size = dimensions(connector);
    REQUIRE(size.x() == Approx(4.09).margin(2e-5));
    REQUIRE(size.y() == Approx(3.34).margin(2e-5));
    REQUIRE(size.z() == Approx(11.0).margin(2e-5));
    REQUIRE(tac_joint_validate({}).valid);
}

// Catches multiplying physical clearance by scale or adding generic connector tolerance twice.
TEST_CASE("TAC Joint physical clearance is independent of connector scale", "[TACJoint]")
{
    TacJointParams p;
    const Vec3d    original = dimensions(tac_joint_cutter_mesh(p));
    p.scale                 = 2;
    const Vec3d larger      = dimensions(tac_joint_cutter_mesh(p));
    REQUIRE(larger.x() == Approx(2 * original.x() - 0.24).margin(3e-6));
    REQUIRE(larger.y() == Approx(2 * original.y() - 0.24).margin(3e-6));
    REQUIRE(larger.z() == Approx(2 * original.z() - 0.60).margin(3e-6));
    REQUIRE(dimensions(tac_joint_connector_mesh(p)).z() == Approx(22).margin(3e-5));
    REQUIRE(tac_joint_validate(p).valid);
    p.clearance       = .22f;
    p.end_clearance   = .5f;
    const Vec3d loose = dimensions(tac_joint_cutter_mesh(p));
    REQUIRE(loose.x() - larger.x() == Approx(.2).margin(3e-6));
    REQUIRE(loose.y() - larger.y() == Approx(.2).margin(3e-6));
    REQUIRE(loose.z() - larger.z() == Approx(.4).margin(3e-6));
}

// The rounded reference profile can interfere even while all three cutter bounds are larger.
TEST_CASE("TAC Joint validation rejects actual rounded-profile interference", "[TACJoint]")
{
    TacJointParams p;
    p.clearance = 0;
    REQUIRE_FALSE(tac_joint_validate(p).valid);
    REQUIRE_FALSE(tac_joint_cutter_mesh(p).indices.empty());
    p.clearance          = .001f;
    const auto connector = tac_joint_connector_mesh(p);
    // A small positive outer bounding-box allowance is not a containment proof.
    REQUIRE((dimensions(tac_joint_cutter_mesh(p)) - dimensions(connector)).minCoeff() > 0);
    REQUIRE_FALSE(tac_joint_validate(p).valid);
    p.clearance = .12f;
    REQUIRE(tac_joint_validate(p).valid);
}

// An independent solid-boolean oracle checks the fit validator against the
// rounded STL surfaces, including the case that passes a bounding-box test.
TEST_CASE("TAC Joint containment agrees with solid mesh subtraction", "[TACJoint]")
{
    TacJointParams            p;
    std::vector<TriangleMesh> residual;
    REQUIRE(MeshBoolean::mfd::make_boolean(TriangleMesh(tac_joint_connector_mesh(p)), TriangleMesh(tac_joint_cutter_mesh(p)), residual,
                                           "A_NOT_B"));
    REQUIRE(residual.empty());
    p.clearance = .001f;
    REQUIRE(MeshBoolean::mfd::make_boolean(TriangleMesh(tac_joint_connector_mesh(p)), TriangleMesh(tac_joint_cutter_mesh(p)), residual,
                                           "A_NOT_B"));
    REQUIRE_FALSE(residual.empty());
    double outside_volume = 0;
    for (TriangleMesh& piece : residual)
        outside_volume += piece.volume();
    REQUIRE(outside_volume > 1e-6);
}

TEST_CASE("TAC Joint rejects unsafe parameters and insufficient engagement", "[TACJoint]")
{
    TacJointParams p;
    for (float bad :
         {0.f, -1.f, std::numeric_limits<float>::min(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        p.scale = bad;
        REQUIRE_FALSE(tac_joint_validate(p).valid);
        REQUIRE_THROWS(tac_joint_connector_mesh(p));
        REQUIRE_THROWS(tac_joint_cutter_mesh(p));
    }
    for (float TacJointParams::*field : {&TacJointParams::clearance, &TacJointParams::end_clearance, &TacJointParams::offset})
        for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            p        = {};
            p.*field = bad;
            REQUIRE_FALSE(tac_joint_validate(p).valid);
            REQUIRE_THROWS(tac_joint_connector_mesh(p));
            REQUIRE_THROWS(tac_joint_cutter_mesh(p));
        }
    p         = {};
    p.version = 999;
    REQUIRE_FALSE(tac_joint_validate(p).valid);
    p        = {};
    p.offset = 5.5f;
    REQUIRE_FALSE(tac_joint_validate(p).valid);
    p.offset = 4.5f;
    REQUIRE(tac_joint_validate(p).valid);
    REQUIRE_FALSE(tac_joint_validate(p, -1.1, 1.1).valid);
    REQUIRE_FALSE(tac_joint_validate(p, 1, -1).valid);
    REQUIRE_FALSE(tac_joint_validate(p, std::numeric_limits<double>::quiet_NaN(), 0).valid);
    REQUIRE_FALSE(tac_joint_validate(p, 0, std::numeric_limits<double>::infinity()).valid);
    p.offset = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(tac_joint_validate(p).valid);
    p               = {};
    p.end_clearance = -1;
    REQUIRE_FALSE(tac_joint_validate(p).valid);
}

TEST_CASE("TAC Joint offset shifts both meshes along the same local normal", "[TACJoint]")
{
    TacJointParams p;
    const auto     connector   = tac_joint_connector_mesh(p);
    const auto     cutter      = tac_joint_cutter_mesh(p);
    p.offset                   = 1.25f;
    const auto moved_connector = tac_joint_connector_mesh(p);
    const auto moved_cutter    = tac_joint_cutter_mesh(p);
    for (const auto& pair : {std::make_pair(&connector, &moved_connector), std::make_pair(&cutter, &moved_cutter)})
        for (size_t i = 0; i < pair.first->vertices.size(); ++i)
            REQUIRE((pair.second->vertices[i] - pair.first->vertices[i] - Vec3f(0, 0, 1.25f)).norm() < 1e-6f);
}

TEST_CASE("TAC Joint attributes roundtrip and reject malformed versions", "[TACJoint]")
{
    TacJointParams p;
    p.scale         = 1.5f;
    p.clearance     = .24f;
    p.end_clearance = .45f;
    p.offset        = -1;
    std::map<std::string, double> attributes;
    for (const auto& attribute : tac_joint_attributes(p))
        attributes.insert(attribute);
    const auto get = [&](const std::string& key) -> std::optional<double> {
        const auto it = attributes.find(key);
        return it == attributes.end() ? std::nullopt : std::optional<double>(it->second);
    };
    REQUIRE(tac_joint_from_attributes(get) == p);
    attributes["tac_joint_version"] = 1.5;
    REQUIRE_FALSE(tac_joint_validate(tac_joint_from_attributes(get)).valid);
    attributes["tac_joint_version"] = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(tac_joint_validate(tac_joint_from_attributes(get)).valid);
    attributes.clear();
    REQUIRE(tac_joint_from_attributes(get) == TacJointParams{});
}
