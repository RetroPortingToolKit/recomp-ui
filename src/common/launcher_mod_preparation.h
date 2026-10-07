#ifndef LAUNCHER_MOD_PREPARATION_H
#define LAUNCHER_MOD_PREPARATION_H

#include "launcher_mod_commit_job.h"
#include "../recomp_launcher.h"
#include <cstdio>

/* Backend-owned policy around the actual joined worker. The provider owns
 * readiness; the observed key only coalesces requests and prevents a failed
 * preparation from retrying every frame. It never authorizes a launch. */
class LauncherModPreparation {
public:
    enum class Request { None, Ready, Queued, Failed };
    enum class Completion { None, Prepared, Launch, Failed, Closed };
    LauncherModCommitJob job;

    static bool supports(const RecompLauncherCModProvider* provider, bool in_session) {
        return !in_session && provider && provider->commit_worker_safe &&
               provider->commit && provider->try_commit && provider->preparation_revision;
    }

    Request prepare_if_changed(const RecompLauncherCModProvider* provider,
                               bool in_session, const char* image) {
        if (job.pending() || !supports(provider, in_session) || !image || !image[0])
            return Request::None;
        if (callback_failed_ && observed_ && provider == provider_ && image_ == image)
            return Request::None;
        unsigned long long revision = 0;
        if (!read_revision(provider, revision)) {
            observe(provider, image, 0);
            callback_failed_ = true;
            return Request::Failed;
        }
        if (observed_ && provider == provider_ && image_ == image && revision_ == revision)
            return Request::None;
        if (!observe(provider, image, revision)) return Request::Failed;
        return request(provider, image, false);
    }

    Request launch(const RecompLauncherCModProvider* provider,
                   bool in_session, const char* image) {
        if (job.pending()) return Request::Queued;
        error_[0] = '\0';
        callback_failed_ = false; // Explicit PLAY may retry a failed callback.
        if (!provider || !provider->commit) return Request::Ready;
        if (supports(provider, in_session)) {
            unsigned long long revision = 0;
            if (!read_revision(provider, revision)) return Request::Failed;
            if (!observe(provider, image ? image : "", revision)) return Request::Failed;
            return request(provider, image ? image : "", true);
        }
        /* A commit-only provider is never prepared automatically. Existing
         * worker opt-in applies to PLAY; in-session callers remain synchronous. */
        if (!in_session && provider->commit_worker_safe)
            return queue(provider, image, true);
        if (provider->commit(provider->ctx, image ? image : "")) return Request::Ready;
        copy_error(provider->last_error ? provider->last_error(provider->ctx) : nullptr);
        return Request::Failed;
    }

    bool launches_on_success() const { return launch_; }
    Completion finish(bool close_requested) {
        if (!job.finish()) return Completion::None;
        if (close_requested) return Completion::Closed;
        if (!job.result()) return Completion::Failed;
        return launch_ ? Completion::Launch : Completion::Prepared;
    }
    const char* error() const { return error_[0] ? error_.data() : job.error(); }

private:
    bool read_revision(const RecompLauncherCModProvider* provider,
                       unsigned long long& revision) {
        try {
            revision = provider->preparation_revision(provider->ctx);
            return true;
        } catch (const std::exception& e) { copy_error(e.what()); }
          catch (...) { copy_error("The mod preparation callback raised an exception."); }
        callback_failed_ = true;
        return false;
    }
    bool observe(const RecompLauncherCModProvider* provider, const char* image,
                 unsigned long long revision) {
        try { image_ = image; }
        catch (const std::exception& e) { copy_error(e.what()); return false; }
        provider_ = provider;
        revision_ = revision;
        observed_ = true;
        callback_failed_ = false;
        return true;
    }
    Request request(const RecompLauncherCModProvider* provider, const char* image,
                    bool launch) {
        error_[0] = '\0';
        try {
            const int ready = provider->try_commit(provider->ctx, image);
            if (ready == 1) return Request::Ready;
            if (ready != 0) {
                copy_error(provider->last_error ? provider->last_error(provider->ctx) : nullptr);
                return Request::Failed;
            }
        } catch (const std::exception& e) {
            copy_error(e.what()); callback_failed_ = true;
            return Request::Failed;
        } catch (...) {
            copy_error("The mod preparation callback raised an exception."); callback_failed_ = true;
            return Request::Failed;
        }
        return queue(provider, image, launch);
    }
    Request queue(const RecompLauncherCModProvider* provider, const char* image,
                  bool launch) {
        launch_ = launch;
        return job.queue(provider->commit, provider->last_error, provider->ctx, image)
            ? Request::Queued : Request::Failed;
    }
    void copy_error(const char* text) {
        std::snprintf(error_.data(), error_.size(), "%s",
                      text && text[0] ? text : "The mod operation failed.");
    }
    const RecompLauncherCModProvider* provider_ = nullptr;
    std::string image_;
    unsigned long long revision_ = 0;
    bool observed_ = false, launch_ = false, callback_failed_ = false;
    std::array<char, 1024> error_{};
};

#endif
