#include "MaaMcp.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
uint64_t hash_state(const MaaMcpState& s) {
    // FNV-1a over the stable, observable fields.  This is a consistency token,
    // not a security primitive.
    uint64_t h = 1469598103934665603ULL;
    const uint64_t values[] = {s.revision, s.stage_id, s.wave, s.operator_count, s.flags};
    for (uint64_t v : values) {
        for (unsigned i = 0; i < sizeof(v); ++i) {
            h ^= static_cast<uint8_t>(v >> (i * 8));
            h *= 1099511628211ULL;
        }
    }
    return h;
}

bool parse_hash(const char* text, uint64_t& value) {
    if (!text || !*text) return false;
    const auto length = std::strlen(text);
    if (length > 16) return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtoull(text, &end, 16);
    return errno == 0 && end && *end == '\0';
}

bool valid_kind(MaaMcpActionKind kind) {
    return kind >= MaaMcpActionDeploy && kind <= MaaMcpActionPause;
}
}

struct MaaMcpServer {
    MaaMcpConfig config{};
    std::mutex mutex;
    struct Completed { std::string fingerprint; MaaMcpActionResult result; };
    std::unordered_map<std::string, Completed> completed;
};

extern "C" MaaMcpServer* MaaMcpCreate(const MaaMcpConfig* config) {
    if (!config) return nullptr;
    try {
        auto* server = new MaaMcpServer();
        server->config = *config;
        if (server->config.default_timeout_ms == 0) server->config.default_timeout_ms = 5000;
        return server;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void MaaMcpDestroy(MaaMcpServer* server) { delete server; }

extern "C" MaaMcpResultCode MaaMcpObserveState(MaaMcpServer* server, MaaMcpState* out_state) {
    if (!server || !out_state) return MaaMcpInvalidArgument;
    if (!server->config.observe) return MaaMcpNotConfigured;
    try {
        MaaMcpState state{};
        if (server->config.observe(server->config.user, &state) != 0) return MaaMcpBackendError;
        state.state_hash = hash_state(state);
        *out_state = state;
        return MaaMcpOk;
    } catch (...) {
        return MaaMcpBackendError;
    }
}

extern "C" MaaMcpResultCode MaaMcpObserveRoguelikeState(MaaMcpServer* server,
                                                          MaaMcpState* out_state) {
    // Deliberately read-only and unimplemented until a backend can provide a
    // reliable schema.  Keeping the endpoint lets clients feature-detect it.
    if (!server || !out_state) return MaaMcpInvalidArgument;
    (void)server;
    std::memset(out_state, 0, sizeof(*out_state));
    return MaaMcpNotConfigured;
}

extern "C" MaaMcpResultCode MaaMcpExecute(MaaMcpServer* server,
                                             const MaaMcpActionRequest* request,
                                             MaaMcpActionResult* out_result) {
    try {
    if (!server || !request || !out_result || !request->payload || !request->expected_state_hash ||
        !request->idempotency_key || !*request->idempotency_key)
        return MaaMcpInvalidArgument;
    *out_result = MaaMcpActionResult{MaaMcpBackendError, 0, 0};
    if (!server->config.enable_mutations || !server->config.execute) {
        out_result->code = MaaMcpNotConfigured;
        return out_result->code;
    }
    if (!request->safety_gate) {
        out_result->code = MaaMcpSafetyDenied;
        return out_result->code;
    }
    if (!valid_kind(request->kind)) {
        out_result->code = MaaMcpInvalidArgument;
        return out_result->code;
    }

    // Serialize observe/CAS/dispatch as one transaction.  This prevents two
    // callers from both observing the same state and dispatching actions.
    std::unique_lock transaction(server->mutex);
    const std::string key(request->idempotency_key);
    const std::string fingerprint = std::to_string(static_cast<int>(request->kind)) + "\n" +
                                    request->expected_state_hash + "\n" + request->payload;
    auto it = server->completed.find(key);
    if (it != server->completed.end()) {
        if (it->second.fingerprint != fingerprint) {
            out_result->code = MaaMcpConflict;
            return out_result->code;
        }
        *out_result = it->second.result;
        out_result->code = MaaMcpDuplicate;
        return out_result->code;
    }

    MaaMcpState current{};
    auto observed = MaaMcpObserveState(server, &current);
    if (observed != MaaMcpOk) {
        out_result->code = observed;
        return observed;
    }
    uint64_t expected = 0;
    if (!parse_hash(request->expected_state_hash, expected) || expected != current.state_hash) {
        out_result->code = MaaMcpConflict;
        out_result->resulting_state_hash = current.state_hash;
        return out_result->code;
    }
    try {
        if (server->config.precondition &&
            server->config.precondition(server->config.user, request->kind, request->payload) != 0) {
            out_result->code = MaaMcpPreconditionFailed;
            out_result->resulting_state_hash = current.state_hash;
            return out_result->code;
        }
    } catch (...) {
        out_result->code = MaaMcpBackendError;
        out_result->resulting_state_hash = current.state_hash;
        return out_result->code;
    }

    const uint32_t timeout = request->timeout_ms ? request->timeout_ms : server->config.default_timeout_ms;
    const auto started = std::chrono::steady_clock::now();
    int32_t backend_code = 0;
    int rc = -1;
    try {
        rc = server->config.execute(server->config.user, request->kind, request->payload,
                                    timeout, &backend_code);
    } catch (...) {
        rc = -1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    out_result->backend_code = backend_code;
    if (elapsed > timeout) out_result->code = MaaMcpTimeout;
    else if (rc != 0) out_result->code = MaaMcpBackendError;
    else out_result->code = MaaMcpOk;

    MaaMcpState after{};
    if (MaaMcpObserveState(server, &after) == MaaMcpOk) out_result->resulting_state_hash = after.state_hash;
    // Record every dispatched outcome, including timeout/backend error: a
    // caller must not retry an operation whose side effects are indeterminate.
    server->completed.emplace(key, MaaMcpServer::Completed{fingerprint, *out_result});
    return out_result->code;
    } catch (...) {
        if (out_result) out_result->code = MaaMcpBackendError;
        return MaaMcpBackendError;
    }
}

extern "C" const char* MaaMcpResultCodeName(MaaMcpResultCode code) {
    switch (code) {
    case MaaMcpOk: return "ok";
    case MaaMcpInvalidArgument: return "invalid_argument";
    case MaaMcpNotConfigured: return "not_configured";
    case MaaMcpSafetyDenied: return "safety_denied";
    case MaaMcpPreconditionFailed: return "precondition_failed";
    case MaaMcpConflict: return "conflict";
    case MaaMcpTimeout: return "timeout";
    case MaaMcpBackendError: return "backend_error";
    case MaaMcpDuplicate: return "duplicate";
    }
    return "unknown";
}
