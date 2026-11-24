#pragma once

#include "PrintConfig.hpp"

namespace Slic3r {

/// Effective support / raft / enforce / stabilizer activity of one config scope.
///
/// enable_support, raft_layers and enforce_support_layers all make the print
/// consume the support filaments. enforce_support_layers generates support for
/// the first N layers even when normal support is disabled (see its PrintConfig
/// definition), so it is part of the activity check or the per-plate gates
/// could miss a real support filament usage.
///
/// Edge also prints the support filament for side stabilizers
/// (Print::uses_support_filament) without Enable supports. The support
/// interface filament is used only when support / raft / enforce material is
/// generated (Print::has_support_material).
struct SupportActivity
{
    bool enable_support = false;
    int  raft_layers    = 0;
    int  enforce_layers = 0;
    bool stabilizers    = false;

    bool uses_support_material() const { return enable_support || raft_layers > 0 || enforce_layers > 0; }
    bool uses_support_filament() const { return uses_support_material() || stabilizers; }
    bool uses_support_interface_filament() const { return uses_support_material(); }
    // Object / range leftover support_* keys count when this scope actually
    // consumes the support filament (supports, raft, enforcers or stabilizers).
    bool active() const { return uses_support_filament(); }
};

/// Global support activity from a canonical merged config (defaults when a key is absent).
inline SupportActivity support_activity_from_config(const DynamicPrintConfig &cfg)
{
    SupportActivity out;
    if (cfg.has("enable_support"))
        out.enable_support = cfg.opt_bool("enable_support");
    if (cfg.has("raft_layers"))
        out.raft_layers = cfg.opt_int("raft_layers");
    if (cfg.has("enforce_support_layers"))
        out.enforce_layers = cfg.opt_int("enforce_support_layers");
    if (cfg.has("stabilizer_supports"))
        out.stabilizers = cfg.opt_enum<StabilizerMode>("stabilizer_supports") != smOff;
    return out;
}

/// Resolve one scope's support activity; keys absent from the scope fall back
/// to the enclosing scope's values (layer range -> object -> global), so a
/// range that enables support on its own is honored.
inline SupportActivity effective_support_activity(const ModelConfig &config, const SupportActivity &fallback)
{
    SupportActivity out;
    out.enable_support = config.has("enable_support") ?
        config.get().opt_bool("enable_support") : fallback.enable_support;
    out.raft_layers = config.has("raft_layers") ?
        config.get().opt_int("raft_layers") : fallback.raft_layers;
    out.enforce_layers = config.has("enforce_support_layers") ?
        config.get().opt_int("enforce_support_layers") : fallback.enforce_layers;
    out.stabilizers = config.has("stabilizer_supports") ?
        (config.get().opt_enum<StabilizerMode>("stabilizer_supports") != smOff) : fallback.stabilizers;
    return out;
}

} // namespace Slic3r
