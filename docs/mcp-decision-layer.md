# Maa MCP decision layer (MVP)

`MaaMcp` is an optional, transport-agnostic adapter for an MCP server. It is
intentionally outside the OCR/input pipeline: an embedding process supplies
`observe` and `execute` callbacks, so existing MaaCore behavior remains
unchanged. An HTTP/WS or stdio MCP transport can map its tools to these calls.

The read-only tool is `observe_state`; `observe_roguelike_state` is a reserved,
read-only feature-detection endpoint and currently returns `not_configured`.
Mutations are limited to deploy, retreat, skill, speed and pause. Every action
must provide a hexadecimal `expected_state_hash` (compare-and-swap), a unique
`idempotency_key`, and `safety_gate: true`. Optional precondition and timeout
checks are enforced before an adapter callback is invoked. Successful keys are
remembered for the lifetime of the server and replay as `duplicate`.

This MVP does not implement a network listener, OCR, click injection, or a
policy engine. Applications should bind callbacks to the existing MaaCore C
API/HTTP+WS wrapper and expose the adapter only on a trusted local interface.
