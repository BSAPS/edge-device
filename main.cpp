#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <libcamera/libcamera.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer.h>
#include <libcamera/stream.h>
#include <libcamera/request.h>

using namespace libcamera;
using namespace std::chrono_literals;

namespace {
    std::atomic<bool> is_running{true};
} // namespace

static void signal_handler(int signum) {
    is_running = false;
    std::cout << "\nCaught signal " << signum << ", stopping..." << std::endl;
}

class CameraFeeder {
public:
    CameraFeeder() = default;
    ~CameraFeeder() { cleanup(); }

    int initialize() {
        camera_manager_ = std::make_unique<CameraManager>();
        if (camera_manager_->start()) return -1;
        if (camera_manager_->cameras().empty()) {
            std::cerr << "[ERROR] No cameras found." << std::endl;
            return -1;
        }
        std::string camera_id = camera_manager_->cameras()[0]->id();
        camera_ = camera_manager_->get(camera_id);
        if (!camera_ || camera_->acquire()) {
            std::cerr << "[ERROR] Failed to acquire camera." << std::endl;
            return -1;
        }
        std::cout << "[INFO] Acquired camera: " << camera_id << std::endl;
        return 0;
    }

    int configure() {
        config_ = camera_->generateConfiguration({ StreamRole::Viewfinder, StreamRole::Viewfinder });
        if (!config_) {
            std::cerr << "[ERROR] Failed to generate configuration." << std::endl;
            return -1;
        }
        
        // Configure 1080p stream for RTSP
        config_->at(0).size = Size(1920, 1080);
        config_->at(0).pixelFormat = formats::YUYV; // Use YUYV packed format
        config_->at(0).bufferCount = 6; // More buffers for smoother streaming
        config_->at(0).colorSpace = ColorSpace::Sycc;

        // Configure 480p stream for inference
        config_->at(1).size = Size(640, 480);
        config_->at(1).pixelFormat = formats::BGR888;
        config_->at(1).bufferCount = 4;
        config_->at(1).colorSpace = ColorSpace::Sycc;

        if (config_->validate() == CameraConfiguration::Invalid) {
            std::cerr << "[ERROR] Failed to validate configuration." << std::endl;
            return -1;
        }
        
        if (camera_->configure(config_.get()) < 0) {
            std::cerr << "[ERROR] Failed to configure camera." << std::endl;
            return -1;
        }
        
        hr_stream_ = config_->at(0).stream();
        lr_stream_ = config_->at(1).stream();

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        for (StreamConfiguration &cfg : *config_) {
            if (allocator_->allocate(cfg.stream()) < 0) {
                std::cerr << "[ERROR] Failed to allocate buffers." << std::endl;
                return -1;
            }
        }
        std::cout << "[INFO] Camera configured with 1080p and 480p streams." << std::endl;
        return 0;
    }

    int start_capture() {
        if (pipe(ffmpeg_pipe_) < 0) {
            std::cerr << "[ERROR] Failed to create pipe for ffmpeg." << std::endl;
            return -1;
        }

        ffmpeg_pid_ = fork();
        if (ffmpeg_pid_ == 0) { // Child process
            close(ffmpeg_pipe_[1]); // Close write end of the pipe
            dup2(ffmpeg_pipe_[0], STDIN_FILENO); // Redirect stdin to read from the pipe
            close(ffmpeg_pipe_[0]);

            char* const argv[] = {
                (char*)"ffmpeg",
                (char*)"-f", (char*)"rawvideo",
                (char*)"-pix_fmt", (char*)"yuyv422",
                (char*)"-s", (char*)"1920x1080",
                (char*)"-r", (char*)"30",
                (char*)"-i", (char*)"-",
                (char*)"-c:v", (char*)"h264_v4l2m2m",
                (char*)"-b:v", (char*)"2000000", // 2 Mbps bitrate
                (char*)"-f", (char*)"rtsp",
                (char*)"rtsp://localhost:8554/stream",
                nullptr
            };
            execvp("ffmpeg", argv);
            // If execvp returns, an error occurred
            std::cerr << "[ERROR] Failed to execute ffmpeg." << std::endl;
            exit(1);
        } else if (ffmpeg_pid_ > 0) { // Parent process
            close(ffmpeg_pipe_[0]); // Close read end of the pipe
            std::cout << "[INFO] Started ffmpeg process with PID: " << ffmpeg_pid_ << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to fork ffmpeg process." << std::endl;
            return -1;
        }

        camera_->requestCompleted.connect(this, &CameraFeeder::request_completed);
        for (StreamConfiguration &cfg : *config_) {
            for (const auto &buffer : allocator_->buffers(cfg.stream())) {
                std::unique_ptr<Request> request = camera_->createRequest();
                if (!request || request->addBuffer(cfg.stream(), buffer.get()) < 0) return -1;
                requests_.push_back(std::move(request));
            }
        }
        if (camera_->start()) return -1;
        for (auto &request : requests_) camera_->queueRequest(request.get());
        std::cout << "[INFO] Camera started, piping frames to ffmpeg." << std::endl;
        return 0;
    }

private:
    void request_completed(Request *request) {
        if (request->status() == Request::RequestCancelled || !is_running) return;

        // High-resolution stream for ffmpeg
        auto it_hr = request->buffers().find(hr_stream_);
        if (it_hr != request->buffers().end()) {
            FrameBuffer *buffer = it_hr->second;
            size_t total_size = 0;
            for (const auto& plane : buffer->planes()) total_size += plane.length;
            
            void *data = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, buffer->planes()[0].fd.get(), 0);
            if (data != MAP_FAILED) {
                if (write(ffmpeg_pipe_[1], data, total_size) < 0) {
                    if (errno != EPIPE) {
                       // Ignore broken pipe errors, which happen on shutdown
                    }
                }
                munmap(data, total_size);
            }
        }

        // Low-resolution stream for inference (placeholder)
        auto it_lr = request->buffers().find(lr_stream_);
        if (it_lr != request->buffers().end()) {
            // Placeholder for inference logic
        }

        // Log status every 150 frames (~5 seconds at 30fps)
        if (++frameCount_ % 150 == 0) {
            std::cout << "[STATUS] Frames processed: " << frameCount_ << std::endl;
        }

        request->reuse(Request::ReuseBuffers);
        camera_->queueRequest(request);
    }

    void cleanup() {
        is_running = false;
        if (camera_) {
            camera_->stop();
            camera_->requestCompleted.disconnect(this, &CameraFeeder::request_completed);
        }
        
        if (ffmpeg_pid_ > 0) {
            close(ffmpeg_pipe_[1]); // Close the pipe to signal EOF to ffmpeg
            int status;
            waitpid(ffmpeg_pid_, &status, 0); // Wait for ffmpeg to terminate
            std::cout << "\n[INFO] ffmpeg process terminated." << std::endl;
        }

        requests_.clear();
        if (allocator_) {
            for (StreamConfiguration &cfg : *config_) allocator_->free(cfg.stream());
        }
        
        if(camera_) {
            camera_->release();
            camera_.reset();
        }
        
        if(camera_manager_) {
            camera_manager_->stop();
        }
        std::cout << "[INFO] Cleanup complete." << std::endl;
    }

    std::unique_ptr<CameraManager> camera_manager_;
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    Stream *hr_stream_{nullptr};
    Stream *lr_stream_{nullptr};
    
    pid_t ffmpeg_pid_{-1};
    int ffmpeg_pipe_[2]{-1, -1};
    long frameCount_ = 0;
};

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    CameraFeeder app;
    if (app.initialize() != 0 || app.configure() != 0 || app.start_capture() != 0) {
        std::cerr << "[FATAL] Application setup failed." << std::endl;
        return -1;
    }

    std::cout << "[INFO] Running... Press Ctrl+C to stop." << std::endl;
    while(is_running) {
        std::this_thread::sleep_for(100ms);
    }
    
    return 0;
}
