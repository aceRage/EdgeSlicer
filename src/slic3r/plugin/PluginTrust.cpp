#include "PluginTrust.hpp"

#include "PluginAuditManager.hpp"
#include "PluginFsUtils.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Slic3r { namespace plugin_trust {
namespace {

std::mutex& trust_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string ascii_lower(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool is_hex_chars(const std::string& value)
{
    if (value.empty() || (value.size() % 2) != 0)
        return false;
    for (unsigned char ch : value) {
        if (std::isxdigit(ch) == 0)
            return false;
    }
    return true;
}

int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

bool hex_to_bytes(const std::string& hex, std::vector<unsigned char>& out)
{
    out.clear();
    if (!is_hex_chars(hex))
        return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

std::string bytes_to_hex(const unsigned char* data, size_t len)
{
    static const char* k_digits = "0123456789abcdef";
    std::string        out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = k_digits[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = k_digits[data[i] & 0xf];
    }
    return out;
}

std::vector<std::string> normalized_types(std::vector<std::string> types)
{
    types.erase(std::remove_if(types.begin(), types.end(), [](const std::string& t) { return t.empty(); }), types.end());
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

std::string join_types(const std::vector<std::string>& types)
{
    std::string out;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i != 0)
            out += ',';
        out += types[i];
    }
    return out;
}

std::vector<std::string> normalized_deps(std::vector<std::string> deps)
{
    deps.erase(std::remove_if(deps.begin(), deps.end(), [](const std::string& d) { return d.empty(); }), deps.end());
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    return deps;
}

bool name_ends_with(const std::string& name, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

bool exclude_tree_name(const std::string& name)
{
    if (name == "__pycache__" || name == "__whl_extracted__")
        return true;
    if (name == INSTALL_STATE_FILE || name.find(INSTALL_STATE_FILE) == 0)
        return true;
    if (name_ends_with(name, k_sig_suffix))
        return true;
    return false;
}

bool path_stays_under_root(const boost::filesystem::path& root, const boost::filesystem::path& candidate)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path            root_c = fs::weakly_canonical(root, ec);
    if (ec || root_c.empty())
        return false;
    ec.clear();
    const fs::path cand_c = fs::weakly_canonical(candidate, ec);
    if (ec || cand_c.empty())
        return false;
    auto rel = cand_c.lexically_relative(root_c);
    if (rel.empty())
        return false;
    for (const auto& part : rel) {
        if (part == "..")
            return false;
    }
    return true;
}

struct TreeEntry
{
    std::string rel;
    char        kind{'f'}; // 'f' file, 'l' symlink
    std::string payload;
};

bool collect_tree_entries(const boost::filesystem::path& root, std::vector<TreeEntry>& entries, std::string& error)
{
    namespace fs = boost::filesystem;
    std::vector<fs::path> dirs{root};
    while (!dirs.empty()) {
        const fs::path dir = dirs.back();
        dirs.pop_back();
        boost::system::error_code ec;
        fs::directory_iterator    it(dir, ec);
        if (ec) {
            error = "cannot list " + dir.string() + ": " + ec.message();
            return false;
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                error = "cannot iterate " + dir.string() + ": " + ec.message();
                return false;
            }
            const fs::path    path = it->path();
            const std::string name = path.filename().string();
            if (name == "." || name == "..")
                continue;
            if (exclude_tree_name(name))
                continue;

            ec.clear();
            const fs::file_status st = fs::symlink_status(path, ec);
            if (ec) {
                error = "cannot stat " + path.string() + ": " + ec.message();
                return false;
            }

            const fs::path rel_path = path.lexically_relative(root);
            std::string    rel      = rel_path.generic_string();
            if (rel.empty() || rel == ".")
                continue;

            if (fs::is_symlink(st)) {
                ec.clear();
                const fs::path target = fs::read_symlink(path, ec);
                if (ec) {
                    error = "cannot read symlink " + path.string() + ": " + ec.message();
                    return false;
                }
                const fs::path resolved = path.parent_path() / target;
                if (!path_stays_under_root(root, resolved)) {
                    error = "symlink escapes plugin root: " + path.string();
                    return false;
                }
                entries.push_back(TreeEntry{std::move(rel), 'l', target.generic_string()});
                continue;
            }
            if (fs::is_directory(st)) {
                dirs.push_back(path);
                continue;
            }
            if (!fs::is_regular_file(st))
                continue;
            std::string       file_error;
            const std::string hash = sha256_file(path, file_error);
            if (hash.empty()) {
                error = file_error.empty() ? ("cannot hash " + path.string()) : file_error;
                return false;
            }
            entries.push_back(TreeEntry{std::move(rel), 'f', hash});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const TreeEntry& a, const TreeEntry& b) { return a.rel < b.rel; });
    return true;
}

std::string permissions_digest_of(const PluginPermissions& permissions)
{
    auto dump_list = [](std::ostringstream& out, const char* key, std::vector<std::string> values) {
        std::sort(values.begin(), values.end());
        out << key << '=';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i != 0)
                out << ',';
            out << values[i];
        }
        out << '\n';
    };
    std::ostringstream out;
    dump_list(out, "fs_read", permissions.fs_read);
    dump_list(out, "fs_readwrite", permissions.fs_readwrite);
    dump_list(out, "network_http", permissions.network_http);
    dump_list(out, "network_socket", permissions.network_socket);
    dump_list(out, "process", permissions.process);
    dump_list(out, "events", permissions.events);
    out << "threading=" << (permissions.threading ? '1' : '0') << '\n';
    out << "full_access=" << (permissions.full_access ? '1' : '0') << '\n';
    const std::string text = out.str();
    return sha256_hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

bool user_sidecar_version_ok(const nlohmann::json& user)
{
    const auto it = user.find("version");
    if (it == user.end() || !it->is_number_integer())
        return false;
    return it->get<int>() == 1;
}

bool read_file_bytes(const boost::filesystem::path& path, std::string& out, std::string& error)
{
    boost::nowide::ifstream in(path.string(), std::ios::in | std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

// Same atomic replace as the install-state sidecar: unique temp in the target directory, then
// POSIX rename / MoveFileExW REPLACE. A failure leaves the previous file untouched.
bool atomic_replace_file(const boost::filesystem::path& target, const std::string& contents, std::string& error)
{
    namespace fs = boost::filesystem;

    static std::atomic<unsigned> counter{0};
    const fs::path tmp_path = target.parent_path() / (target.filename().string() + "." + std::to_string(get_current_pid()) + "." +
                                                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp");

    const auto fail = [&tmp_path, &error](const std::string& reason) {
        boost::system::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        error = reason;
        return false;
    };

    boost::nowide::ofstream out(tmp_path.string(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out)
        return fail("cannot open temporary for writing");

    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) {
        out.close();
        return fail("write or flush failed");
    }
    out.close();
    if (!out)
        return fail("close failed");

#ifdef _WIN32
    {
        const std::wstring wide_tmp = tmp_path.wstring();
        HANDLE handle = ::CreateFileW(wide_tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return fail("CreateFileW for fsync failed");
        const BOOL flushed = ::FlushFileBuffers(handle);
        ::CloseHandle(handle);
        if (!flushed)
            return fail("FlushFileBuffers failed");
    }
#else
    {
        const int fd = ::open(tmp_path.string().c_str(), O_RDONLY);
        if (fd < 0)
            return fail("open for fsync failed");
        const int synced = ::fsync(fd);
        const int saved  = errno;
        ::close(fd);
        if (synced != 0)
            return fail(std::error_code(saved, std::generic_category()).message());
    }
#endif

#ifdef _WIN32
    const std::wstring wide_tmp     = tmp_path.wstring();
    const std::wstring wide_target  = target.wstring();
    constexpr unsigned max_attempts = 200;
    for (unsigned attempt = 0;; ++attempt) {
        if (::MoveFileExW(wide_tmp.c_str(), wide_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            break;
        const DWORD last_error = ::GetLastError();
        const bool  transient  = last_error == ERROR_SHARING_VIOLATION || last_error == ERROR_LOCK_VIOLATION ||
                               last_error == ERROR_ACCESS_DENIED;
        if (!transient || attempt + 1 == max_attempts)
            return fail("MoveFileExW failed with error " + std::to_string(static_cast<unsigned long>(last_error)));
        ::Sleep(10);
    }
#else
    if (boost::nowide::rename(tmp_path.string().c_str(), target.string().c_str()) != 0)
        return fail(std::error_code(errno, std::generic_category()).message());
#endif
    return true;
}

Publisher publisher_from_json(const nlohmann::json& entry, const std::string& source)
{
    Publisher pub;
    if (!entry.is_object())
        return pub;
    pub.name       = entry.value("name", std::string());
    pub.public_key = ascii_lower(entry.value("public_key", std::string()));
    pub.source     = source;
    if (pub.public_key.size() != 64 || !is_hex_chars(pub.public_key)) {
        pub.public_key.clear();
        pub.name.clear();
    }
    return pub;
}

std::vector<Publisher> publishers_from_json(const nlohmann::json& root, const std::string& source)
{
    std::vector<Publisher> out;
    const auto             it = root.find("publishers");
    if (it == root.end() || !it->is_array())
        return out;
    for (const nlohmann::json& entry : *it) {
        Publisher pub = publisher_from_json(entry, source);
        if (!pub.public_key.empty())
            out.push_back(std::move(pub));
    }
    return out;
}

std::vector<Pin> pins_from_json(const nlohmann::json& root)
{
    std::vector<Pin> out;
    const auto       it = root.find("pins");
    if (it == root.end() || !it->is_array())
        return out;
    for (const nlohmann::json& entry : *it) {
        if (!entry.is_object())
            continue;
        Pin pin;
        pin.sha256     = ascii_lower(entry.value("sha256", std::string()));
        pin.plugin_key = entry.value("plugin_key", std::string());
        if (pin.plugin_key.empty())
            continue;
        if (pin.sha256.size() == 64 && is_hex_chars(pin.sha256))
            out.push_back(std::move(pin));
    }
    return out;
}

nlohmann::json load_json_object_file(const boost::filesystem::path& path, bool& corrupt)
{
    corrupt = false;
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec) || ec)
        return nlohmann::json{};
    std::string bytes;
    std::string error;
    if (!read_file_bytes(path, bytes, error)) {
        corrupt = true;
        return nlohmann::json{};
    }
    try {
        nlohmann::json parsed = nlohmann::json::parse(bytes);
        if (!parsed.is_object()) {
            corrupt = true;
            return nlohmann::json{};
        }
        return parsed;
    } catch (...) {
        corrupt = true;
        return nlohmann::json{};
    }
}

const Publisher* find_publisher(const TrustStore& store, const std::string& public_key)
{
    const std::string key = ascii_lower(public_key);
    for (const Publisher& pub : store.shipped_publishers) {
        if (pub.public_key == key)
            return &pub;
    }
    for (const Publisher& pub : store.user_publishers) {
        if (pub.public_key == key)
            return &pub;
    }
    return nullptr;
}

bool sidecar_resolves_into_ultranet(const boost::filesystem::path& path)
{
    if (path.empty())
        return true;
    if (path_is_ultranet(path) || path_is_ultranet(path.parent_path()))
        return true;
    boost::system::error_code     ec;
    const boost::filesystem::path canonical = boost::filesystem::weakly_canonical(path, ec);
    if (!ec && !canonical.empty() && canonical != path && (path_is_ultranet(canonical) || path_is_ultranet(canonical.parent_path())))
        return true;
    return false;
}

} // namespace

boost::filesystem::path trust_sidecar_path() { return boost::filesystem::path(data_dir()) / "orca_plugins" / k_sidecar_name; }

boost::filesystem::path shipped_keys_path() { return boost::filesystem::path(resources_dir()) / k_shipped_keys_rel; }

boost::filesystem::path signature_path(const boost::filesystem::path& package)
{
    return boost::filesystem::path(package.string() + k_sig_suffix);
}

bool path_is_ultranet(const boost::filesystem::path& path)
{
    if (path.empty())
        return false;
    return PluginAuditManager::instance().is_ultranet_plugins_path(path);
}

std::string sha256_hex(const unsigned char* data, size_t len)
{
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    unsigned int  digest_len                   = SHA256_DIGEST_LENGTH;
    if (EVP_Digest(data, len, digest, &digest_len, EVP_sha256(), nullptr) != 1)
        return {};
    return bytes_to_hex(digest, digest_len);
}

std::string sha256_file(const boost::filesystem::path& path, std::string& error)
{
    error.clear();
    std::string bytes;
    if (!read_file_bytes(path, bytes, error))
        return {};
    return sha256_hex(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

std::string sha256_plugin_tree(const boost::filesystem::path& root, std::string& error)
{
    namespace fs = boost::filesystem;
    error.clear();
    if (root.empty()) {
        error = "plugin root is empty";
        return {};
    }
    boost::system::error_code ec;
    const fs::file_status     st = fs::symlink_status(root, ec);
    if (ec) {
        error = "cannot stat plugin root: " + ec.message();
        return {};
    }
    if (fs::is_symlink(st)) {
        error = "plugin root must not be a symlink";
        return {};
    }

    std::vector<TreeEntry> entries;
    if (fs::is_regular_file(st)) {
        const std::string hash = sha256_file(root, error);
        if (hash.empty())
            return {};
        entries.push_back(TreeEntry{root.filename().generic_string(), 'f', hash});
    } else if (fs::is_directory(st)) {
        if (!collect_tree_entries(root, entries, error))
            return {};
    } else {
        error = "plugin root is not a file or directory";
        return {};
    }

    std::ostringstream out;
    out << "edge-plugin-tree-v1\n";
    for (const TreeEntry& entry : entries)
        out << entry.kind << '\t' << entry.rel << '\t' << entry.payload << '\n';
    const std::string manifest = out.str();
    return sha256_hex(reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size());
}

std::string canonical_payload(const std::string& sha256, const SignedMetadata& metadata)
{
    const std::vector<std::string> types       = normalized_types(metadata.types);
    const std::vector<std::string> deps        = normalized_deps(metadata.dependencies);
    const std::string              deps_text   = join_types(deps);
    const std::string              deps_digest = sha256_hex(reinterpret_cast<const unsigned char*>(deps_text.data()), deps_text.size());
    std::ostringstream             out;
    out << k_payload_version << '\n'
        << "sha256=" << ascii_lower(sha256) << '\n'
        << "name=" << metadata.name << '\n'
        << "version=" << metadata.version << '\n'
        << "types=" << join_types(types) << '\n'
        << "min_orca_version=" << metadata.min_orca_version << '\n'
        << "plugin_key=" << metadata.plugin_key << '\n'
        << "deps=" << deps_text << '\n'
        << "deps_digest=" << deps_digest << '\n'
        << "permissions_digest=" << ascii_lower(metadata.permissions_digest) << '\n';
    return out.str();
}

SignedMetadata metadata_from_descriptor(const PluginDescriptor& descriptor)
{
    SignedMetadata meta;
    meta.name               = descriptor.name;
    meta.version            = descriptor.installed_version.empty() ? descriptor.version : descriptor.installed_version;
    meta.types              = normalized_types(descriptor.display_types);
    meta.min_orca_version   = descriptor.required_orca_api;
    meta.plugin_key         = descriptor.plugin_key;
    meta.dependencies       = normalized_deps(descriptor.dependencies);
    meta.permissions_digest = permissions_digest_of(PluginPermissions{});
    return meta;
}

bool metadata_matches(const SignedMetadata& signed_meta, const PluginDescriptor& descriptor)
{
    const SignedMetadata live = metadata_from_descriptor(descriptor);
    return signed_meta.name == live.name && signed_meta.version == live.version && normalized_types(signed_meta.types) == live.types &&
           signed_meta.min_orca_version == live.min_orca_version && signed_meta.plugin_key == live.plugin_key &&
           normalized_deps(signed_meta.dependencies) == live.dependencies &&
           ascii_lower(signed_meta.permissions_digest) == live.permissions_digest;
}

bool public_key_from_seed(const std::string& seed_hex, std::string& public_hex, std::string& error)
{
    public_hex.clear();
    error.clear();
    std::vector<unsigned char> seed;
    if (!hex_to_bytes(ascii_lower(seed_hex), seed) || seed.size() != 32) {
        error = "Ed25519 seed must be 32 bytes (64 hex chars)";
        return false;
    }
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    OPENSSL_cleanse(seed.data(), seed.size());
    if (!pkey) {
        error = "OpenSSL rejected the Ed25519 seed";
        return false;
    }
    unsigned char pub[32] = {};
    size_t        publen  = sizeof(pub);
    const bool    ok      = EVP_PKEY_get_raw_public_key(pkey, pub, &publen) == 1 && publen == sizeof(pub);
    EVP_PKEY_free(pkey);
    if (!ok) {
        error = "OpenSSL could not export the Ed25519 public key";
        return false;
    }
    public_hex = bytes_to_hex(pub, publen);
    return true;
}

bool sign(const std::string& seed_hex, const std::string& message, std::string& signature_hex, std::string& error)
{
    signature_hex.clear();
    error.clear();
    std::vector<unsigned char> seed;
    if (!hex_to_bytes(ascii_lower(seed_hex), seed) || seed.size() != 32) {
        error = "Ed25519 seed must be 32 bytes (64 hex chars)";
        return false;
    }
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    OPENSSL_cleanse(seed.data(), seed.size());
    if (!pkey) {
        error = "OpenSSL rejected the Ed25519 seed";
        return false;
    }
    unsigned char sig[64] = {};
    size_t        siglen  = sizeof(sig);
    EVP_MD_CTX*   md      = EVP_MD_CTX_new();
    const bool    ok      = md && EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
                    EVP_DigestSign(md, sig, &siglen, reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1 &&
                    siglen == sizeof(sig);
    if (md)
        EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!ok) {
        error = "Ed25519 sign failed";
        return false;
    }
    signature_hex = bytes_to_hex(sig, siglen);
    return true;
}

bool verify(const std::string& public_hex, const std::string& message, const std::string& signature_hex)
{
    std::vector<unsigned char> pub;
    std::vector<unsigned char> sig;
    if (!hex_to_bytes(ascii_lower(public_hex), pub) || pub.size() != 32)
        return false;
    if (!hex_to_bytes(ascii_lower(signature_hex), sig) || sig.size() != 64)
        return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    if (!pkey)
        return false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    const bool  ok = md && EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
                    EVP_DigestVerify(md, sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size()) ==
                        1;
    if (md)
        EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

bool write_signature_file(const boost::filesystem::path& package, const Signature& sig, std::string& error)
{
    error.clear();
    nlohmann::json meta = {
        {"name", sig.metadata.name},
        {"version", sig.metadata.version},
        {"types", normalized_types(sig.metadata.types)},
        {"min_orca_version", sig.metadata.min_orca_version},
        {"plugin_key", sig.metadata.plugin_key},
        {"dependencies", normalized_deps(sig.metadata.dependencies)},
        {"permissions_digest", ascii_lower(sig.metadata.permissions_digest)},
    };
    nlohmann::json root = {
        {"version", 1},
        {"algorithm", "ed25519"},
        {"public_key", ascii_lower(sig.public_key)},
        {"publisher", sig.publisher},
        {"signature", ascii_lower(sig.signature)},
        {"hash", ascii_lower(sig.hash)},
        {"metadata", std::move(meta)},
    };
    const boost::filesystem::path path = signature_path(package);
    boost::system::error_code     ec;
    boost::filesystem::create_directories(path.parent_path(), ec);
    if (!atomic_replace_file(path, root.dump(2) + "\n", error))
        return false;
    return true;
}

bool read_signature_file(const boost::filesystem::path& package, Signature& sig, std::string& error)
{
    error.clear();
    sig = {};
    std::string bytes;
    if (!read_file_bytes(signature_path(package), bytes, error))
        return false;
    try {
        const nlohmann::json root = nlohmann::json::parse(bytes);
        if (!root.is_object()) {
            error = "signature sidecar is not a JSON object";
            return false;
        }
        sig.public_key     = ascii_lower(root.value("public_key", std::string()));
        sig.signature      = ascii_lower(root.value("signature", std::string()));
        sig.hash           = ascii_lower(root.value("hash", std::string()));
        sig.publisher      = root.value("publisher", std::string());
        const auto meta_it = root.find("metadata");
        if (meta_it != root.end() && meta_it->is_object()) {
            sig.metadata.name               = meta_it->value("name", std::string());
            sig.metadata.version            = meta_it->value("version", std::string());
            sig.metadata.min_orca_version   = meta_it->value("min_orca_version", std::string());
            sig.metadata.plugin_key         = meta_it->value("plugin_key", std::string());
            sig.metadata.permissions_digest = ascii_lower(meta_it->value("permissions_digest", std::string()));
            const auto types_it             = meta_it->find("types");
            if (types_it != meta_it->end() && types_it->is_array()) {
                for (const nlohmann::json& t : *types_it)
                    if (t.is_string())
                        sig.metadata.types.push_back(t.get<std::string>());
            }
            sig.metadata.types = normalized_types(sig.metadata.types);
            const auto deps_it = meta_it->find("dependencies");
            if (deps_it != meta_it->end() && deps_it->is_array()) {
                for (const nlohmann::json& d : *deps_it)
                    if (d.is_string())
                        sig.metadata.dependencies.push_back(d.get<std::string>());
            }
            sig.metadata.dependencies = normalized_deps(sig.metadata.dependencies);
        }
        if (root.value("algorithm", std::string()) != "ed25519") {
            error = "unsupported signature algorithm";
            return false;
        }
        return true;
    } catch (...) {
        error = "signature sidecar is not valid JSON";
        return false;
    }
}

TrustStore load_trust_store()
{
    TrustStore store;

    bool shipped_corrupt     = false;
    store.shipped_publishers = publishers_from_json(load_json_object_file(shipped_keys_path(), shipped_corrupt), "shipped");
    if (shipped_corrupt)
        store.shipped_publishers.clear();
    for (Publisher& pub : store.shipped_publishers)
        pub.source = "shipped";

    bool                          user_corrupt = false;
    const boost::filesystem::path sidecar      = trust_sidecar_path();
    boost::system::error_code     exists_ec;
    const bool                    sidecar_exists = boost::filesystem::exists(sidecar, exists_ec) && !exists_ec;
    const nlohmann::json          user           = load_json_object_file(sidecar, user_corrupt);
    store.user_file_corrupt                      = user_corrupt;
    if (!user_corrupt && sidecar_exists && !user_sidecar_version_ok(user))
        store.user_file_corrupt = true;
    if (store.user_file_corrupt) {
        // Fail closed: a corrupt sidecar contributes no pins and no user-added keys.
        store.user_publishers.clear();
        store.pins.clear();
        return store;
    }
    store.user_publishers = publishers_from_json(user, "user");
    for (Publisher& pub : store.user_publishers)
        pub.source = "user";
    store.pins = pins_from_json(user);
    return store;
}

bool save_trust_store(const TrustStore& store, std::string& error)
{
    error.clear();
    if (store.user_file_corrupt) {
        error = "trust.json is corrupt; reset it before saving";
        return false;
    }
    const boost::filesystem::path path = trust_sidecar_path();
    if (sidecar_resolves_into_ultranet(path)) {
        error = "refusing to write plugin trust.json under UltraNet {data_dir}/plugins";
        BOOST_LOG_TRIVIAL(error) << error << " path=" << path.string();
        return false;
    }

    boost::system::error_code ec;
    boost::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    if (sidecar_resolves_into_ultranet(path)) {
        error = "refusing to write plugin trust.json under UltraNet {data_dir}/plugins";
        return false;
    }

    nlohmann::json publishers = nlohmann::json::array();
    for (const Publisher& pub : store.user_publishers) {
        if (pub.public_key.size() != 64 || !is_hex_chars(pub.public_key))
            continue;
        publishers.push_back({
            {"name", pub.name},
            {"public_key", ascii_lower(pub.public_key)},
            {"source", "user"},
        });
    }
    nlohmann::json pins = nlohmann::json::array();
    for (const Pin& pin : store.pins) {
        if (pin.plugin_key.empty() || pin.sha256.size() != 64 || !is_hex_chars(pin.sha256))
            continue;
        nlohmann::json entry = {{"sha256", ascii_lower(pin.sha256)}, {"plugin_key", pin.plugin_key}};
        pins.push_back(std::move(entry));
    }
    const nlohmann::json root = {
        {"version", 1},
        {"publishers", std::move(publishers)},
        {"pins", std::move(pins)},
    };

    if (!atomic_replace_file(path, root.dump(2) + "\n", error))
        return false;
    return true;
}

bool add_user_publisher(const Publisher& publisher, std::string& error)
{
    error.clear();
    const std::string key = ascii_lower(publisher.public_key);
    if (key.size() != 64 || !is_hex_chars(key)) {
        error = "publisher public key must be 32 bytes (64 hex chars)";
        return false;
    }
    std::lock_guard<std::mutex> lock(trust_mutex());
    TrustStore                  store = load_trust_store();
    if (store.user_file_corrupt) {
        error = "trust.json is corrupt; reset it before saving";
        return false;
    }
    for (Publisher& pub : store.user_publishers) {
        if (pub.public_key == key) {
            pub.name = publisher.name;
            return save_trust_store(store, error);
        }
    }
    Publisher added  = publisher;
    added.public_key = key;
    added.source     = "user";
    store.user_publishers.push_back(std::move(added));
    return save_trust_store(store, error);
}

bool pin_hash(const std::string& plugin_key, const std::string& sha256, std::string& error)
{
    error.clear();
    if (plugin_key.empty()) {
        error = "content pin requires a plugin_key";
        return false;
    }
    const std::string hash = ascii_lower(sha256);
    if (hash.size() != 64 || !is_hex_chars(hash)) {
        error = "content pin must be a SHA-256 hex digest";
        return false;
    }
    std::lock_guard<std::mutex> lock(trust_mutex());
    TrustStore                  store = load_trust_store();
    if (store.user_file_corrupt) {
        error = "trust.json is corrupt; reset it before saving";
        return false;
    }
    bool replaced = false;
    for (Pin& pin : store.pins) {
        if (pin.plugin_key == plugin_key) {
            pin.sha256 = hash;
            replaced   = true;
        }
    }
    if (!replaced)
        store.pins.push_back(Pin{hash, plugin_key});
    return save_trust_store(store, error);
}

bool unpin_plugin(const std::string& plugin_key, std::string& error)
{
    error.clear();
    std::lock_guard<std::mutex> lock(trust_mutex());
    TrustStore                  store = load_trust_store();
    if (store.user_file_corrupt) {
        error = "trust.json is corrupt; reset it before saving";
        return false;
    }
    store.pins.erase(std::remove_if(store.pins.begin(), store.pins.end(),
                                    [&plugin_key](const Pin& pin) { return pin.plugin_key == plugin_key; }),
                     store.pins.end());
    return save_trust_store(store, error);
}

bool is_hash_pinned(const TrustStore& store, const std::string& plugin_key, const std::string& sha256)
{
    const std::string hash = ascii_lower(sha256);
    for (const Pin& pin : store.pins) {
        if (pin.sha256 != hash)
            continue;
        if (!pin.plugin_key.empty() && pin.plugin_key == plugin_key)
            return true;
    }
    return false;
}

bool plugin_key_has_different_pin(const TrustStore& store, const std::string& plugin_key, const std::string& sha256)
{
    if (plugin_key.empty())
        return false;
    const std::string hash = ascii_lower(sha256);
    for (const Pin& pin : store.pins) {
        if (pin.plugin_key == plugin_key && pin.sha256 != hash)
            return true;
    }
    return false;
}

bool local_install_needs_prompt(const std::string& plugin_key, const std::string& sha256)
{
    const TrustStore store = load_trust_store();
    return !is_hash_pinned(store, plugin_key, sha256);
}

bool reset_corrupt_trust_store(std::string& error)
{
    error.clear();
    std::lock_guard<std::mutex> lock(trust_mutex());
    TrustStore                  empty;
    empty.user_file_corrupt = false;
    return save_trust_store(empty, error);
}

std::string refuse_untrusted_message()
{
    return _u8L(
        "This plugin is untrusted. Approve it when installing from a local file, or install a package signed by a trusted publisher.");
}

std::string trust_label(const TrustState& state)
{
    switch (state.kind) {
    case TrustKind::Signed:
        return GUI::format(_u8L("Signed by %1%"), state.publisher.empty() ? _u8L("unknown publisher") : state.publisher);
    case TrustKind::ApprovedLocally: return _u8L("Approved locally");
    case TrustKind::Untrusted:
    default: return _u8L("Untrusted");
    }
}

TrustState evaluate_package(const boost::filesystem::path& package, const PluginDescriptor& descriptor)
{
    TrustState state;
    state.label = trust_label(state);
    if (package.empty()) {
        state.error = refuse_untrusted_message();
        return state;
    }

    std::string                   hash_error;
    const boost::filesystem::path content_root = descriptor.plugin_root.empty() ? package : boost::filesystem::path(descriptor.plugin_root);
    state.sha256                               = sha256_plugin_tree(content_root, hash_error);
    if (state.sha256.empty()) {
        state.error = hash_error.empty() ? refuse_untrusted_message() : hash_error;
        state.label = trust_label(state);
        return state;
    }

    const TrustStore store = load_trust_store();

    Signature   sig;
    std::string sig_error;
    if (read_signature_file(package, sig, sig_error) && sig.hash == state.sha256 && metadata_matches(sig.metadata, descriptor)) {
        const std::string payload = canonical_payload(sig.hash, sig.metadata);
        if (verify(sig.public_key, payload, sig.signature)) {
            if (const Publisher* pub = find_publisher(store, sig.public_key)) {
                state.kind      = TrustKind::Signed;
                state.publisher = pub->name;
                state.label     = trust_label(state);
                return state;
            }
        }
    }

    if (is_hash_pinned(store, descriptor.plugin_key, state.sha256)) {
        state.kind  = TrustKind::ApprovedLocally;
        state.label = trust_label(state);
        return state;
    }

    state.error = refuse_untrusted_message();
    state.label = trust_label(state);
    return state;
}

TrustState evaluate(const PluginDescriptor& descriptor)
{
    return evaluate_package(boost::filesystem::path(descriptor.entry_path), descriptor);
}

}} // namespace Slic3r::plugin_trust
