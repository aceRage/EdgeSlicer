#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r { namespace plugin_trust {

// Allow-list sidecar under {data_dir}/orca_plugins. Never {data_dir}/plugins (UltraNet).
inline constexpr const char* k_sidecar_name     = "trust.json";
inline constexpr const char* k_sig_suffix       = ".sig";
inline constexpr const char* k_payload_version  = "edge-plugin-sig-v1";
inline constexpr const char* k_shipped_keys_rel = "python_plugins/trust_keys.json";

// Existing installs from before phase 5 have no pin and no signature, so they come up Untrusted
// until the user re-approves (local pin) or installs a package signed by a trusted publisher.

struct Publisher
{
    std::string name;
    std::string public_key; // 32-byte Ed25519 public key, 64 lowercase hex chars
    std::string source;     // "shipped" or "user"
};

struct Pin
{
    std::string sha256;     // SHA-256 of the plugin tree (sorted relative paths + content)
    std::string plugin_key; // required; local sideload of a named package
};

struct TrustStore
{
    std::vector<Publisher> shipped_publishers;
    std::vector<Publisher> user_publishers;
    std::vector<Pin>       pins;
    bool                   user_file_corrupt{false};
};

struct SignedMetadata
{
    std::string              name;
    std::string              version;
    std::vector<std::string> types;
    std::string              min_orca_version;
    std::string              plugin_key;
    std::vector<std::string> dependencies;
    std::string              permissions_digest; // SHA-256 of the canonical requested-permissions form
};

struct Signature
{
    std::string    public_key;
    std::string    signature; // 64-byte Ed25519 signature, 128 lowercase hex chars
    std::string    hash;
    std::string    publisher;
    SignedMetadata metadata;
};

enum class TrustKind { Untrusted, ApprovedLocally, Signed };

struct TrustState
{
    TrustKind   kind{TrustKind::Untrusted};
    std::string publisher;
    std::string sha256;
    std::string label;
    std::string error;

    bool allowed() const { return kind != TrustKind::Untrusted; }
};

boost::filesystem::path trust_sidecar_path();
boost::filesystem::path shipped_keys_path();
boost::filesystem::path signature_path(const boost::filesystem::path& package);

// True when `path` is UltraNet's {data_dir}/plugins or a file inside it (case-insensitive,
// NFC/NFD, dangling-symlink, and equivalent() rules from PluginAuditManager).
bool path_is_ultranet(const boost::filesystem::path& path);

std::string sha256_hex(const unsigned char* data, size_t len);
std::string sha256_file(const boost::filesystem::path& path, std::string& error);
// Canonical tree hash of a plugin file or plugin root: sorted relative paths plus content.
// Skips .sig files, __pycache__, __whl_extracted__, and .install_state.json. Does not follow
// symlinks out of the root (an escaping symlink fails the hash). uv-installed deps are not
// part of this tree.
std::string sha256_plugin_tree(const boost::filesystem::path& root, std::string& error);

std::string    canonical_payload(const std::string& sha256, const SignedMetadata& metadata);
SignedMetadata metadata_from_descriptor(const PluginDescriptor& descriptor);
bool           metadata_matches(const SignedMetadata& signed_meta, const PluginDescriptor& descriptor);

// Ed25519 via OpenSSL EVP. seed_hex is a 32-byte private scalar (64 hex chars). Used by tests
// and by an offline signer; the application never ships a private key.
bool public_key_from_seed(const std::string& seed_hex, std::string& public_hex, std::string& error);
bool sign(const std::string& seed_hex, const std::string& message, std::string& signature_hex, std::string& error);
bool verify(const std::string& public_hex, const std::string& message, const std::string& signature_hex);

bool write_signature_file(const boost::filesystem::path& package, const Signature& sig, std::string& error);
bool read_signature_file(const boost::filesystem::path& package, Signature& sig, std::string& error);

// Never throws. A missing sidecar is empty-trust. A corrupt or unparseable file, a missing
// version, or any version other than 1 is empty-trust (no user pins, no user publishers) and
// never crashes; shipped keys still apply.
TrustStore load_trust_store();
// Atomic replace. Refuses to write if the sidecar path resolves into UltraNet {data_dir}/plugins
// or if store.user_file_corrupt is set (would wipe existing pins). Call reset_corrupt_trust_store
// for an explicit empty rewrite.
bool save_trust_store(const TrustStore& store, std::string& error);
bool reset_corrupt_trust_store(std::string& error);

bool add_user_publisher(const Publisher& publisher, std::string& error);
bool pin_hash(const std::string& plugin_key, const std::string& sha256, std::string& error);
bool unpin_plugin(const std::string& plugin_key, std::string& error);

bool is_hash_pinned(const TrustStore& store, const std::string& plugin_key, const std::string& sha256);
bool plugin_key_has_different_pin(const TrustStore& store, const std::string& plugin_key, const std::string& sha256);

// A local file install prompts unless this plugin_key is already pinned to this exact hash.
bool local_install_needs_prompt(const std::string& plugin_key, const std::string& sha256);

TrustState evaluate(const PluginDescriptor& descriptor);
TrustState evaluate_package(const boost::filesystem::path& package, const PluginDescriptor& descriptor);

std::string refuse_untrusted_message();
std::string trust_label(const TrustState& state);

}} // namespace Slic3r::plugin_trust
