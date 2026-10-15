#ifndef LAUNCHER_MOD_COMMIT_JOB_H
#define LAUNCHER_MOD_COMMIT_JOB_H

#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

/* Owned by one backend invocation. Only the provider callback and its error
 * accessor run on the worker; the owner must exclude all other provider
 * access while pending(). Destruction joins before the borrowed ctx can die. */
class LauncherModCommitJob {
public:
    using Commit = int (*)(void*, const char*);
    using LastError = const char* (*)(void*);
    using Progress = int (*)(void*, float, const char*);
    using CommitWithProgress = int (*)(void*, const char*, Progress, void*);
    struct Status {float fraction=-1.0f;std::array<char,1024> text{};};

    ~LauncherModCommitJob() {
        request_cancel();
        if (worker_.joinable()) worker_.join();
    }
    LauncherModCommitJob() = default;
    LauncherModCommitJob(const LauncherModCommitJob&) = delete;
    LauncherModCommitJob& operator=(const LauncherModCommitJob&) = delete;

    bool queue(Commit commit, LastError last_error, void* ctx, const char* image,
               CommitWithProgress cooperative = nullptr) {
        if (pending_ || !commit) return false;
        try {
            image_ = image ? image : "";
        } catch (const std::exception& e) {
            copy_error(e.what());
            return false;
        }
        commit_ = commit; last_error_ = last_error; ctx_ = ctx;
        cooperative_=cooperative;
        cancel_.store(false,std::memory_order_relaxed);
        {std::lock_guard<std::mutex> lock(status_mutex_);status_=Status{};}
        error_[0] = '\0';
        result_ = false;
        done_.store(false, std::memory_order_relaxed);
        pending_ = queued_ = true;
        return true;
    }

    /* Call after the frame that queued the request has finished its provider
     * reads. A thread-creation failure is a completed failure, never a launch. */
    void start() {
        if (!queued_) return;
        queued_ = false;
        try {
            worker_ = std::thread([this] {
                try {
                    result_ = (cooperative_
                        ? cooperative_(ctx_, image_.c_str(),report,this)
                        : commit_(ctx_, image_.c_str())) != 0;
                    if (!result_)
                        copy_error(last_error_ ? last_error_(ctx_) : nullptr);
                } catch (const std::exception& e) {
                    result_ = false;
                    copy_error(e.what());
                } catch (...) {
                    result_ = false;
                    copy_error("The mod operation raised an exception.");
                }
                done_.store(true, std::memory_order_release);
            });
        } catch (const std::exception& e) {
            copy_error(e.what());
            done_.store(true, std::memory_order_release);
        }
    }

    bool pending() const { return pending_; }
    bool queued() const { return queued_; }
    /* Nonblocking until completion; result/error may be read only after this
     * returns true. Joining also covers the worker's return after publication. */
    bool finish() {
        if (!pending_ || !done_.load(std::memory_order_acquire)) return false;
        if (worker_.joinable()) worker_.join();
        pending_ = false;
        return true;
    }
    bool result() const { return result_; }
    const char* error() const { return error_.data(); }
    bool can_cancel() const {return cooperative_!=nullptr;}
    void request_cancel() {if(can_cancel())cancel_.store(true,std::memory_order_relaxed);}
    bool cancelled() const {return cancel_.load(std::memory_order_relaxed);}
    Status status() const {std::lock_guard<std::mutex> lock(status_mutex_);return status_;}

private:
    static int report(void* ctx,float fraction,const char* text) {
        auto& job=*static_cast<LauncherModCommitJob*>(ctx);
        if(job.cancelled())return 0;
        std::lock_guard<std::mutex> lock(job.status_mutex_);
        job.status_.fraction=fraction>=0.0f && fraction<=1.0f ? fraction : -1.0f;
        std::strncpy(job.status_.text.data(),text?text:"",job.status_.text.size()-1);
        job.status_.text.back()='\0';
        return job.cancelled()?0:1;
    }
    void copy_error(const char* text) {
        if (!text || !text[0]) text = "The mod operation failed.";
        std::strncpy(error_.data(), text, error_.size() - 1);
        error_.back() = '\0';
    }
    std::thread worker_;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_{false};
    mutable std::mutex status_mutex_;
    Status status_;
    bool pending_ = false, queued_ = false, result_ = false;
    Commit commit_ = nullptr;
    CommitWithProgress cooperative_ = nullptr;
    LastError last_error_ = nullptr;
    void* ctx_ = nullptr;
    std::string image_;
    std::array<char, 1024> error_{};
};

#endif
