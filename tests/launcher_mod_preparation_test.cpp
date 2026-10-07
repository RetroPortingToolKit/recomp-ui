/* Exercise the backend's real preparation/launch policy and joined worker. */
#include "launcher_mod_preparation.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

struct Provider {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = true, busy = false;
    bool succeeds = true, ready = false;
    unsigned long long revision = 0;
    int commits = 0, probes = 0, revision_reads = 0, error_reads = 0;
    std::string prepared_image, committed_image;
    std::string error = "Resource verification failed";
    std::thread::id owner = std::this_thread::get_id(), worker;
};
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static int commit(void* ctx, const char* image) {
    auto& p = *static_cast<Provider*>(ctx);
    std::unique_lock<std::mutex> lock(p.mutex);
    ++p.commits;
    p.entered = p.busy = true;
    p.worker = std::this_thread::get_id();
    p.committed_image = image;
    p.changed.notify_all();
    p.changed.wait(lock, [&] { return p.release; });
    p.busy = false;
    p.ready = p.succeeds;
    if (p.ready) p.prepared_image = image;
    return p.succeeds ? 1 : 0;
}
static int probe(void* ctx, const char* image) {
    auto& p = *static_cast<Provider*>(ctx);
    std::lock_guard<std::mutex> lock(p.mutex);
    check(!p.busy && std::this_thread::get_id() == p.owner, "probe must not overlap worker");
    ++p.probes;
    return p.ready && p.prepared_image == image ? 1 : 0;
}
static int failed_probe(void*, const char*) { return -1; }
static unsigned long long revision(void* ctx) {
    auto& p = *static_cast<Provider*>(ctx);
    std::lock_guard<std::mutex> lock(p.mutex);
    check(!p.busy && std::this_thread::get_id() == p.owner, "revision must not overlap worker");
    ++p.revision_reads;
    return p.revision;
}
static const char* error(void* ctx) {
    auto& p = *static_cast<Provider*>(ctx);
    ++p.error_reads;
    return p.error.c_str();
}
static RecompLauncherCModProvider callbacks(Provider& p) {
    RecompLauncherCModProvider result{};
    result.ctx = &p; result.commit = commit; result.last_error = error;
    result.commit_worker_safe = 1;
    result.try_commit = probe; result.preparation_revision = revision;
    return result;
}
using Policy = LauncherModPreparation;
static Policy::Completion finish(Policy& policy, bool close = false) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Policy::Completion done = Policy::Completion::None;
    while (done == Policy::Completion::None && std::chrono::steady_clock::now() < until) {
        done = policy.finish(close);
        if (done == Policy::Completion::None) std::this_thread::yield();
    }
    check(done != Policy::Completion::None && !policy.job.pending(), "completion must join worker");
    return done;
}
int main() {
    Provider p;
    auto api = callbacks(p);
    Policy policy;
    std::string selected = "disc-a.cue";
    check(policy.prepare_if_changed(&api, false, selected.c_str()) == Policy::Request::Queued,
          "initial selected disc prepares without PLAY");
    check(policy.job.queued() && p.commits == 0, "queue waits for drawing to finish");
    selected = "an unselected roster slot";
    policy.job.start();
    check(finish(policy) == Policy::Completion::Prepared, "prepare-only success never launches");
    check(p.committed_image == "disc-a.cue" && p.worker != p.owner, "owned selected path reaches worker");
    check(policy.prepare_if_changed(&api, false, "disc-a.cue") == Policy::Request::None,
          "unchanged selected path does not prepare after unselected-slot bookkeeping");
    const int prepared = p.commits;
    check(policy.launch(&api, false, "disc-a.cue") == Policy::Request::Ready && !policy.job.pending(),
          "authoritative warm PLAY launches without job or progress view");
    check(p.commits == prepared && p.probes == 2, "warm PLAY probes readiness, never repeats preparation");

    ++p.revision; p.ready = false;
    check(policy.prepare_if_changed(&api, false, "disc-a.cue") == Policy::Request::Queued,
          "source/options/catalog revision invalidates preparation for the same disc");
    policy.job.start();
    check(finish(policy) == Policy::Completion::Prepared, "changed inputs prepare without launching");
    check(policy.prepare_if_changed(&api, false, "disc-b.cue") == Policy::Request::Queued,
          "selected disc change prepares even when provider revision is unchanged");
    policy.job.start(); finish(policy);
    p.ready = false; // External invalidation need not change the UI's observed key.
    check(policy.launch(&api, false, "disc-b.cue") == Policy::Request::Queued,
          "PLAY never treats observed path/revision as authoritative readiness");
    policy.job.start();
    check(finish(policy) == Policy::Completion::Launch, "explicit cold PLAY launches only after success");

    Provider bad;
    bad.succeeds = false;
    auto bad_api = callbacks(bad);
    Policy failure;
    failure.prepare_if_changed(&bad_api, false, "bad.cue"); failure.job.start();
    check(finish(failure) == Policy::Completion::Failed, "failed preparation cannot launch");
    bad.error = "later provider error";
    check(std::string(failure.error()) == "Resource verification failed", "worker error remains owned");
    check(failure.prepare_if_changed(&bad_api, false, "bad.cue") == Policy::Request::None && bad.commits == 1,
          "failed unchanged preparation does not retry every frame");
    bad_api.try_commit = failed_probe;
    check(failure.launch(&bad_api, false, "bad.cue") == Policy::Request::Failed && !failure.job.pending(),
          "authoritative fast-path failure blocks launch without a worker");

    Provider closing;
    closing.release = false;
    auto close_api = callbacks(closing);
    Policy close;
    /* Even an assertion/exception in this blocked fixture must release the
     * provider before the policy destructor joins, so failures cannot hang. */
    struct ReleaseOnExit {
        Provider& provider;
        ~ReleaseOnExit() {
            std::lock_guard<std::mutex> lock(provider.mutex);
            provider.release = true; provider.changed.notify_all();
        }
    } release_on_exit{closing};
    close.prepare_if_changed(&close_api, false, "close.cue"); close.job.start();
    {
        std::unique_lock<std::mutex> lock(closing.mutex);
        check(closing.changed.wait_for(lock, std::chrono::seconds(5), [&] { return closing.entered; }),
              "close fixture worker entered");
    }
    const int probes = closing.probes, revisions = closing.revision_reads;
    check(close.prepare_if_changed(&close_api, false, "other.cue") == Policy::Request::None &&
          close.launch(&close_api, false, "other.cue") == Policy::Request::Queued &&
          closing.probes == probes && closing.revision_reads == revisions,
          "pending job excludes provider probes and replacement requests");
    check(close.finish(true) == Policy::Completion::None, "close does not abandon an active worker");
    {
        std::lock_guard<std::mutex> lock(closing.mutex);
        closing.release = true; closing.changed.notify_all();
    }
    check(finish(close, true) == Policy::Completion::Closed, "close wins successful preparation after join");

    Provider legacy;
    auto old_api = callbacks(legacy);
    old_api.try_commit = nullptr; old_api.preparation_revision = nullptr;
    Policy old;
    check(old.prepare_if_changed(&old_api, false, "old.cue") == Policy::Request::None && legacy.commits == 0,
          "commit-only providers never auto-prepare");
    check(old.launch(&old_api, false, "old.cue") == Policy::Request::Queued, "legacy worker PLAY remains supported");
    old.job.start(); check(finish(old) == Policy::Completion::Launch, "legacy PLAY completes as launch");
    old_api.commit_worker_safe = 0;
    check(old.launch(&old_api, false, "sync.cue") == Policy::Request::Ready && legacy.worker == legacy.owner,
          "zero worker opt-in retains synchronous commit");
    old_api = callbacks(legacy);
    check(old.prepare_if_changed(&old_api, true, "session.cue") == Policy::Request::None,
          "in-session preparation remains disabled");
    const int before_probe = legacy.probes;
    check(old.launch(&old_api, true, "session.cue") == Policy::Request::Ready &&
          legacy.worker == legacy.owner && legacy.probes == before_probe,
          "in-session commit retains synchronous legacy path without warm probe");
    return 0;
}
