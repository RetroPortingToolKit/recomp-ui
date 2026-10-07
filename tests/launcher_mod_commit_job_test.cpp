/* Exercise the actual owned job used by the ImGui backend, without SDL/GL. */
#include "launcher_mod_commit_job.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>

struct Provider {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, returned = false;
    bool succeeds = true, throws = false;
    std::string image, error = "verified resource missing";
    std::thread::id caller, worker;
    int error_reads = 0;
};

static int commit(void* ctx, const char* image) {
    auto& p = *static_cast<Provider*>(ctx);
    std::unique_lock<std::mutex> lock(p.mutex);
    p.worker = std::this_thread::get_id();
    p.image = image;
    p.entered = true;
    p.changed.notify_all();
    p.changed.wait(lock, [&] { return p.release; });
    p.returned = true;
    if (p.throws) throw std::runtime_error("preparer exception");
    return p.succeeds ? 1 : 0;
}
static const char* error(void* ctx) {
    auto& p = *static_cast<Provider*>(ctx);
    ++p.error_reads;
    return p.error.c_str();
}
static int failures;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static bool finish(LauncherModCommitJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!job.finish() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return !job.pending();
}
static void release(Provider& p) {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.release = true;
    p.changed.notify_all();
}

int main() {
    Provider p;
    p.caller = std::this_thread::get_id();
    LauncherModCommitJob job;
    std::string image = "verified-disc.bin";
    check(job.queue(commit, error, &p, image.c_str()), "queue accepted");
    image = "later UI path";
    check(job.pending() && job.queued(), "queued request already excludes provider access");
    check(!p.entered && !job.finish(), "queue does not start work during provider drawing");
    check(!job.queue(commit, error, &p, "second"), "cannot replace borrowed context while pending");
    job.start();
    {
        std::unique_lock<std::mutex> lock(p.mutex);
        const bool entered = p.changed.wait_for(lock, std::chrono::seconds(5), [&] { return p.entered; });
        check(entered, "worker enters actual callback");
        check(p.image == "verified-disc.bin", "worker receives an owned image copy");
        check(p.worker != p.caller, "commit runs on worker");
        check(!job.finish(), "poll is nonblocking while callback is blocked");
    }
    release(p);
    check(finish(job) && job.result(), "success publishes and joins");
    check(p.returned && !job.queued() && p.error_reads == 0, "success needs no error callback");
    check(!job.finish(), "completion consumed once");

    Provider bad;
    bad.succeeds = false; bad.release = true;
    check(job.queue(commit, error, &bad, "same-disc"), "job reusable after join");
    job.start();
    check(finish(job) && !job.result(), "failed verification cannot launch");
    bad.error = "provider replaced its buffer";
    check(std::string(job.error()) == "verified resource missing" && bad.error_reads == 1,
          "failure text copied before completion and survives provider mutation");

    Provider throwing;
    throwing.throws = true; throwing.release = true;
    check(job.queue(commit, error, &throwing, "disc"), "exception case queued");
    job.start();
    check(finish(job) && !job.result() && std::string(job.error()) == "preparer exception",
          "worker exception is a joined failure");

    Provider lifetime;
    lifetime.release = true;
    {
        LauncherModCommitJob owned;
        owned.queue(commit, error, &lifetime, "disc");
        owned.start();
    }
    check(lifetime.returned, "destruction joins before borrowed provider context dies");
    check(!job.queue(nullptr, error, &p, "disc"), "missing callback rejected");
    return failures ? 1 : 0;
}
