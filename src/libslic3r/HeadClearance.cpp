#include "HeadClearance.hpp"

// EdgeSlicer: print-head clearance model loader. The representation follows PancakeSlicer's head
// envelopes (AGPL-3.0, https://github.com/ZimengXiong/PancakeSlicer); see HeadClearance.hpp.

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "PrintConfig.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"

namespace Slic3r {

double HeadClearance::nozzle_underside_at(double r) const
{
    if (this->nozzle_profile.empty())
        return std::numeric_limits<double>::infinity();
    r = std::abs(r);
    if (r <= this->nozzle_profile.front().second)
        return 0.;
    for (size_t i = 0; i + 1 < this->nozzle_profile.size(); ++i) {
        const auto &[h0, r0] = this->nozzle_profile[i];
        const auto &[h1, r1] = this->nozzle_profile[i + 1];
        if (r <= r1) {
            if (r1 - r0 < 1e-9)
                return h0;
            return h0 + (h1 - h0) * (r - r0) / (r1 - r0);
        }
    }
    return std::numeric_limits<double>::infinity();
}

double HeadClearance::lowest_box_z() const
{
    double z = std::numeric_limits<double>::infinity();
    for (const HeadClearanceBox &b : this->boxes)
        z = std::min(z, b.min.z());
    return z;
}

double HeadClearance::nozzle_flank_angle_deg(double land_tolerance) const
{
    if (this->nozzle_profile.empty())
        return 0.;
    const double r_land = this->nozzle_profile.front().second;
    double       angle  = 90.;
    for (const auto &[h, r] : this->nozzle_profile) {
        if (h <= land_tolerance)
            continue;
        const double dr = r - r_land;
        if (dr <= 0.)
            continue; // straight up from the land's edge: clears any slope below 90
        angle = std::min(angle, std::atan2(h, dr) * 180. / M_PI);
    }
    return angle;
}

bool HeadClearance::matches_printer_model(const std::string &printer_model) const
{
    return !printer_model.empty() &&
           std::find(this->printer_models.begin(), this->printer_models.end(), printer_model) != this->printer_models.end();
}

bool HeadClearance::curved_tops_verified_for(const std::string &printer_model) const
{
    if (!this->curved_tops_verified || printer_model.empty() || !this->matches_printer_model(printer_model))
        return false;
    const auto &models = this->curved_tops_verified->printer_models;
    return std::find(models.begin(), models.end(), printer_model) != models.end();
}

static bool read_vec3(const nlohmann::json &j, Vec3d &out)
{
    if (!j.is_array() || j.size() != 3)
        return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!j[i].is_number())
            return false;
        out[i] = j[i].get<double>();
    }
    return true;
}

std::optional<HeadClearance> parse_head_clearance_json(const std::string &json_text, std::string *error)
{
    auto fail = [error](const std::string &msg) -> std::optional<HeadClearance> {
        if (error != nullptr)
            *error = msg;
        return std::nullopt;
    };

    const nlohmann::json root = nlohmann::json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return fail("not a JSON object");
    if (root.value("schema", std::string()) != HeadClearance::SCHEMA)
        return fail(std::string("schema is not ") + HeadClearance::SCHEMA);

    HeadClearance head;
    head.id     = root.value("id", std::string());
    head.name   = root.value("name", head.id);
    head.source = root.value("source", std::string());
    if (head.id.empty())
        return fail("missing id");
    if (root.contains("calibrated") && root["calibrated"].is_boolean())
        head.calibrated = root["calibrated"].get<bool>();

    if (!root.contains("printer_models") || !root["printer_models"].is_array())
        return fail(head.id + ": missing printer_models");
    for (const nlohmann::json &m : root["printer_models"])
        if (m.is_string() && !m.get<std::string>().empty())
            head.printer_models.push_back(m.get<std::string>());

    if (!root.contains("nozzle") || !root["nozzle"].is_object())
        return fail(head.id + ": missing nozzle");
    const nlohmann::json &nozzle = root["nozzle"];
    if (nozzle.contains("estimated") && nozzle["estimated"].is_boolean())
        head.nozzle_estimated = nozzle["estimated"].get<bool>();
    if (!nozzle.contains("profile") || !nozzle["profile"].is_array() || nozzle["profile"].empty())
        return fail(head.id + ": nozzle.profile must be a non-empty array of [height, radius]");
    for (const nlohmann::json &p : nozzle["profile"]) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
            return fail(head.id + ": nozzle.profile entries must be [height, radius]");
        const double h = p[0].get<double>();
        const double r = p[1].get<double>();
        if (!(r > 0.) || !(h >= 0.))
            return fail(head.id + ": nozzle.profile needs height >= 0 and radius > 0");
        if (!head.nozzle_profile.empty()) {
            if (h <= head.nozzle_profile.back().first)
                return fail(head.id + ": nozzle.profile heights must increase");
            if (r < head.nozzle_profile.back().second)
                return fail(head.id + ": nozzle.profile radii must not decrease");
        } else if (h != 0.)
            return fail(head.id + ": nozzle.profile must start at height 0 (the tip)");
        head.nozzle_profile.emplace_back(h, r);
    }

    if (root.contains("boxes")) {
        if (!root["boxes"].is_array())
            return fail(head.id + ": boxes must be an array");
        for (const nlohmann::json &jb : root["boxes"]) {
            HeadClearanceBox b;
            if (!jb.is_object() || !read_vec3(jb.value("min", nlohmann::json()), b.min) ||
                !read_vec3(jb.value("max", nlohmann::json()), b.max))
                return fail(head.id + ": every box needs min and max as [x, y, z]");
            b.name = jb.value("name", std::string("box"));
            if (jb.contains("spans_x") && jb["spans_x"].is_boolean())
                b.spans_x = jb["spans_x"].get<bool>();
            if (jb.contains("estimated") && jb["estimated"].is_boolean())
                b.estimated = jb["estimated"].get<bool>();
            if (b.min.z() <= 0.)
                return fail(head.id + ": box '" + b.name + "' reaches the tip plane; the nozzle profile owns z <= 0");
            for (int i = 0; i < 3; ++i) {
                // A spans_x box ignores its X numbers, so only Y and Z must be a real extent.
                if (i == 0 && b.spans_x)
                    continue;
                if (!(b.max[i] > b.min[i]))
                    return fail(head.id + ": box '" + b.name + "' has an empty extent");
            }
            head.boxes.push_back(std::move(b));
        }
    }

    // Optional: curved tops printed with this head.
    if (root.contains("curved_tops")) {
        const nlohmann::json &ct = root["curved_tops"];
        if (!ct.is_object())
            return fail(head.id + ": curved_tops must be an object");
        HeadClearanceCurvedTopsVerified v;
        if (ct.contains("verified_printer_models")) {
            if (!ct["verified_printer_models"].is_array())
                return fail(head.id + ": curved_tops.verified_printer_models must be an array");
            for (const nlohmann::json &m : ct["verified_printer_models"])
                if (m.is_string() && !m.get<std::string>().empty())
                    v.printer_models.push_back(m.get<std::string>());
        }
        if (ct.contains("verified_rise_mm") && ct["verified_rise_mm"].is_number())
            v.rise_mm = ct["verified_rise_mm"].get<double>();
        if (ct.contains("verified_angle_deg") && ct["verified_angle_deg"].is_number())
            v.angle_deg = ct["verified_angle_deg"].get<double>();
        if (ct.contains("verified_nozzle_contact_mm") && ct["verified_nozzle_contact_mm"].is_number())
            v.nozzle_contact_mm = std::max(0., ct["verified_nozzle_contact_mm"].get<double>());
        v.on   = ct.value("verified_on", std::string());
        v.note = ct.value("verified_note", std::string());
        if (!(v.rise_mm > 0.) || !(v.angle_deg > 0.) || v.printer_models.empty())
            return fail(head.id + ": curved_tops needs verified_printer_models, verified_rise_mm > 0 and verified_angle_deg > 0");
        for (const std::string &m : v.printer_models)
            if (!head.matches_printer_model(m))
                return fail(head.id + ": curved_tops.verified_printer_models lists '" + m + "', which printer_models does not");
        head.curved_tops_verified = std::move(v);
    }
    return head;
}

std::vector<HeadClearance> load_head_clearances(const std::string &dir, std::vector<std::string> *errors)
{
    namespace fs = boost::filesystem;
    std::vector<HeadClearance> out;
    boost::system::error_code  ec;
    if (!fs::is_directory(dir, ec))
        return out;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->path(), ec) && boost::iequals(it->path().extension().string(), ".json"))
            files.push_back(it->path());
    // Deterministic order, so a model listed by two files always resolves the same way.
    std::sort(files.begin(), files.end());
    for (const fs::path &p : files) {
        boost::nowide::ifstream f(p.string(), std::ios::binary);
        std::stringstream       ss;
        ss << f.rdbuf();
        std::string err;
        if (auto head = parse_head_clearance_json(ss.str(), &err))
            out.push_back(std::move(*head));
        else if (errors != nullptr)
            errors->push_back(p.filename().string() + ": " + err);
    }
    return out;
}

HeadClearancePtrs shipped_head_clearances()
{
    static std::mutex        mutex;
    static bool              loaded = false;
    static std::string       loaded_from;
    static HeadClearancePtrs heads;

    std::lock_guard<std::mutex> lock(mutex);
    const std::string           dir = (boost::filesystem::path(resources_dir()) / "head_clearance").string();
    if (!loaded || loaded_from != dir) {
        std::vector<std::string> errors;
        heads.clear();
        for (HeadClearance &h : load_head_clearances(dir, &errors))
            heads.push_back(std::make_shared<const HeadClearance>(std::move(h)));
        for (const std::string &e : errors)
            BOOST_LOG_TRIVIAL(warning) << "Head clearance: skipped " << e;
        loaded      = true;
        loaded_from = dir;
    }
    return heads;
}

HeadClearancePtr resolve_head_clearance(const std::string &printer_model, const HeadClearancePtrs &heads)
{
    // Source 1 (printer preset values) arrives with P2. Source 2: the first file listing the model.
    for (const HeadClearancePtr &h : heads)
        if (h && h->matches_printer_model(printer_model))
            return h;
    // Source 3: nothing. Deliberately no generic default.
    return nullptr;
}

HeadClearancePtr resolve_head_clearance(const std::string &printer_model)
{
    return resolve_head_clearance(printer_model, shipped_head_clearances());
}

HeadClearancePtr resolve_head_clearance(const PrintConfig &config)
{
    return resolve_head_clearance(config.printer_model.value);
}

std::shared_ptr<const HeadClearance> head_clearance_for_printer_model(const std::string &printer_model)
{
    return resolve_head_clearance(printer_model);
}

} // namespace Slic3r
