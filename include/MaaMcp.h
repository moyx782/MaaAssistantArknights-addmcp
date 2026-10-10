#pragma once

// A deliberately small, transport-agnostic MCP decision layer.  The layer
// does not perform OCR or input injection; callers provide those operations
// through callbacks (normally backed by MaaCore's existing C API).
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaaMcpServer MaaMcpServer;

typedef enum MaaMcpActionKind {
    MaaMcpActionDeploy = 1,
    MaaMcpActionRetreat = 2,
    MaaMcpActionSkill = 3,
    MaaMcpActionSpeed = 4,
    MaaMcpActionPause = 5,
} MaaMcpActionKind;

typedef struct MaaMcpState {
    uint64_t revision;
    uint64_t state_hash;
    uint32_t stage_id;
    uint32_t wave;
    uint32_t operator_count;
    uint32_t flags;
} MaaMcpState;

typedef struct MaaMcpActionRequest {
    MaaMcpActionKind kind;
    const char* payload;          // UTF-8 JSON/object, interpreted by adapter
    const char* expected_state_hash; // required hexadecimal hash (CAS)
    const char* idempotency_key; // required for mutating actions
    uint32_t timeout_ms;          // zero uses the configured default
    uint8_t safety_gate;          // must be non-zero for every mutating action
} MaaMcpActionRequest;

typedef enum MaaMcpResultCode {
    MaaMcpOk = 0,
    MaaMcpInvalidArgument = 1,
    MaaMcpNotConfigured = 2,
    MaaMcpSafetyDenied = 3,
    MaaMcpPreconditionFailed = 4,
    MaaMcpConflict = 5,
    MaaMcpTimeout = 6,
    MaaMcpBackendError = 7,
    MaaMcpDuplicate = 8,
} MaaMcpResultCode;

typedef struct MaaMcpActionResult {
    MaaMcpResultCode code;
    uint64_t resulting_state_hash;
    int32_t backend_code;
} MaaMcpActionResult;

typedef int (*MaaMcpObserveFn)(void* user, MaaMcpState* out_state);
typedef int (*MaaMcpExecuteFn)(void* user, MaaMcpActionKind kind, const char* payload,
                               uint32_t timeout_ms, int32_t* backend_code);
typedef int (*MaaMcpPreconditionFn)(void* user, MaaMcpActionKind kind, const char* payload);

typedef struct MaaMcpConfig {
    void* user;
    MaaMcpObserveFn observe;
    MaaMcpExecuteFn execute;
    MaaMcpPreconditionFn precondition;
    uint32_t default_timeout_ms;
    uint8_t enable_mutations;
} MaaMcpConfig;

// Callbacks must be non-reentrant: do not call MaaMcpExecute from observe,
// precondition, or execute. The decision layer serializes CAS/dispatch and
// holds its transaction lock while invoking callbacks.

MaaMcpServer* MaaMcpCreate(const MaaMcpConfig* config);
void MaaMcpDestroy(MaaMcpServer* server);
MaaMcpResultCode MaaMcpObserveState(MaaMcpServer* server, MaaMcpState* out_state);
MaaMcpResultCode MaaMcpObserveRoguelikeState(MaaMcpServer* server, MaaMcpState* out_state);
MaaMcpResultCode MaaMcpExecute(MaaMcpServer* server, const MaaMcpActionRequest* request,
                               MaaMcpActionResult* out_result);
const char* MaaMcpResultCodeName(MaaMcpResultCode code);

#ifdef __cplusplus
}
#endif
