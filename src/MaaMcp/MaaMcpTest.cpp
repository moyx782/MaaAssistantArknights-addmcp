#include <cstdio>
#include "MaaMcp.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <atomic>

struct Fixture { uint64_t revision = 1; std::atomic<int> calls{0}; };
static int observe(void* p, MaaMcpState* s) {
    auto* f = static_cast<Fixture*>(p); *s = MaaMcpState{}; s->revision = f->revision; s->stage_id = 7; return 0;
}
static int execute(void* p, MaaMcpActionKind, const char*, uint32_t, int32_t* code) {
    auto* f = static_cast<Fixture*>(p); ++f->calls; std::this_thread::sleep_for(std::chrono::milliseconds(5)); *code = 0; return 0;
}
int main() {
    Fixture f; MaaMcpConfig cfg{&f, observe, execute, nullptr, 1000, 1};
    MaaMcpServer* s = MaaMcpCreate(&cfg); assert(s);
    MaaMcpState state{}; assert(MaaMcpObserveState(s, &state) == MaaMcpOk);
    char hash[32]; std::snprintf(hash, sizeof(hash), "%llx", (unsigned long long)state.state_hash);
    MaaMcpActionRequest req{MaaMcpActionPause, "{}", hash, "test-1", 1000, 1};
    MaaMcpActionResult result{}; assert(MaaMcpExecute(s, &req, &result) == MaaMcpOk); assert(f.calls == 1);
    assert(MaaMcpExecute(s, &req, &result) == MaaMcpDuplicate); assert(f.calls == 1);
    req.idempotency_key = "test-2"; req.expected_state_hash = "deadbeef";
    assert(MaaMcpExecute(s, &req, &result) == MaaMcpConflict);
    // Concurrent callers with one CAS token must dispatch at most once.
    req.expected_state_hash = hash; req.idempotency_key = "race";
    MaaMcpResultCode codes[2];
    std::thread t1([&] { codes[0] = MaaMcpExecute(s, &req, &result); });
    std::thread t2([&] { MaaMcpActionResult other{}; codes[1] = MaaMcpExecute(s, &req, &other); });
    t1.join(); t2.join();
    assert(f.calls == 2); // test-1 and race, never a second race dispatch
    assert((codes[0] == MaaMcpOk && codes[1] == MaaMcpDuplicate) ||
           (codes[1] == MaaMcpOk && codes[0] == MaaMcpDuplicate));
    req.idempotency_key = "race"; req.payload = "{\"changed\":true}";
    assert(MaaMcpExecute(s, &req, &result) == MaaMcpConflict);
    MaaMcpDestroy(s); return 0;
}
