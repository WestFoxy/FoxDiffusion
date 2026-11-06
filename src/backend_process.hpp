#pragma once

#include "generation.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// Runs the stable-diffusion.cpp CUDA backend.
class BackendProcess {
public:
    BackendProcess();
    ~BackendProcess();
    BackendProcess(const BackendProcess&) = delete;
    BackendProcess& operator=(const BackendProcess&) = delete;

    void start(GenerationRequest request);
    void set_auto_save(bool txt2img, bool img2img) { auto_save_txt_=txt2img; auto_save_img_=img2img; }
    void cancel();
    void stop();
    bool ready() const;
    std::string status() const;
    GenerationSnapshot snapshot() const;
    void poll_snapshot(GenerationSnapshot& cached) const;
    bool running() const { return workflow_running_.load(); }

private:
    void run(GenerationRequest request, std::stop_token token);
    void run_single(GenerationRequest request, std::stop_token token);
    Raster run_upscale(const GenerationRequest& request, std::stop_token token);
    std::filesystem::path ensure_backend(std::stop_token token);
    bool ensure_server(const GenerationRequest& request, std::stop_token token);
    void stop_server();
    void read_server_output(std::stop_token token);
    void set_status(std::string value);
    struct SaveJob {
        Raster image;
        std::filesystem::path path;
        std::string metadata;
        std::uint64_t workflow = 0;
    };
    void save_outputs();
    void queue_save(SaveJob job);

#ifdef _WIN32
    HANDLE server_process_ = nullptr;
    HANDLE server_output_read_ = nullptr;
#endif
    mutable std::mutex mutex_;
    std::condition_variable save_cv_;
    std::deque<SaveJob> save_queue_;
    size_t queued_save_bytes_ = 0;
    int pending_saves_ = 0;
    bool close_saver_ = false;
    std::string save_error_;
    std::uint64_t workflow_id_ = 0;
    std::jthread save_worker_;
    GenerationSnapshot snapshot_;
    std::string backend_status_ = "Put the CUDA backend next to the app";
    std::jthread worker_;
    std::atomic_bool auto_save_txt_ = true, auto_save_img_ = false;
    std::atomic_bool cancel_requested_ = false;
    std::atomic_bool workflow_running_ = false;
    std::filesystem::path preview_path_;
    std::string server_key_;
    unsigned short server_port_ = 0;
    std::jthread server_output_worker_;
};
