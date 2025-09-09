#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <csignal>
#include <sys/mman.h>
#include <memory>
#include <functional>
#include <iomanip>
#include <queue>

#include <libcamera/libcamera.h>
#include <libcamera/framebuffer.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/controls.h>
#include <libcamera/stream.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

using namespace libcamera;
using namespace std::chrono;
using namespace std::literals::chrono_literals;

// --- Global ---
std::mutex log_mutex;

// RTSP and Camera Configuration
constexpr int RTSP_PORT = 8554;
constexpr const char* RTSP_MOUNT_POINT = "/stream";
constexpr int CAPTURE_WIDTH_HR = 1920;
constexpr int CAPTURE_HEIGHT_HR = 1080;
constexpr int CAPTURE_WIDTH_LR = 640;
constexpr int CAPTURE_HEIGHT_LR = 480;
constexpr int CAPTURE_FPS = 30;

struct FrameData {
    size_t size;
    void* data;
};

template <typename T>
class ThreadSafeQueue {
private:
    mutable std::mutex mtx;
    std::queue<T> data_queue;
    std::condition_variable cv;
    std::atomic<bool> stopped{false};

public:
    void push(T new_value) {
        if (stopped) return;
        std::lock_guard<std::mutex> lock(mtx);
        data_queue.push(std::move(new_value));
        cv.notify_one();
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        if (data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }
    
    bool wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !data_queue.empty() || stopped.load(); });
        if (stopped.load() && data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }
    
    void stop() {
        if (stopped.exchange(true)) return;
        cv.notify_all();
    }
};

class CameraApp; // Forward declaration

class GStreamerRTSPServer {
private:
    GMainLoop *loop_;
    GstRTSPServer *server_;
    GstAppSrc *appsrc_;
    std::thread server_thread_;
    std::atomic<bool> is_running_;
    guint source_id_ = 0;
    CameraApp *app_ = nullptr;

public:
    gboolean push_frame_idle();
    GStreamerRTSPServer() : loop_(nullptr), server_(nullptr), appsrc_(nullptr), is_running_(false) {
        gst_init(nullptr, nullptr);
    }

    ~GStreamerRTSPServer() {
        stop();
        gst_deinit();
    }

    bool start(CameraApp* app) {
        app_ = app;
        loop_ = g_main_loop_new(NULL, FALSE);
        server_ = gst_rtsp_server_new();
        
        // Set server properties
        g_object_set(server_, 
            "service", std::to_string(RTSP_PORT).c_str(),
            "backlog", 5,
            NULL);
        
        GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server_);
        GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

        const char* pipeline_str = "appsrc name=bsaps_src is-live=true do-timestamp=false format=GST_FORMAT_TIME block=false ! queue max-size-buffers=4 leaky=downstream ! videoconvert ! video/x-raw,format=I420 ! openh264enc bitrate=2000000 complexity=0 ! video/x-h264,profile=high,level=(string)4.1 ! h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 config-interval=1 mtu=1400";
        gst_rtsp_media_factory_set_launch(factory, pipeline_str);
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_media_factory_set_latency(factory, 0);
        gst_rtsp_media_factory_set_buffer_size(factory, 0);
        gst_rtsp_media_factory_set_protocols(factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
        gst_rtsp_media_factory_set_stop_on_disconnect(factory, FALSE);
        g_signal_connect(factory, "media-configure", (GCallback)media_configure_callback, this);
        
        gst_rtsp_mount_points_add_factory(mounts, RTSP_MOUNT_POINT, factory);
        g_object_unref(mounts);

        if (gst_rtsp_server_attach(server_, NULL) == 0) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "[ERROR] Failed to attach RTSP server." << std::endl;
            return false;
        }

        is_running_.store(true);
        server_thread_ = std::thread([this]() {
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                std::cout << "[INFO] RTSP stream ready at: rtsp://<your-ip>:" << RTSP_PORT << RTSP_MOUNT_POINT << std::endl;
            }
            g_main_loop_run(loop_);
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                std::cout << "[INFO] GStreamer main loop finished." << std::endl;
            }
        });

        return true;
    }

    void stop() {
        if (is_running_.exchange(false)) {
            if (source_id_ > 0) g_source_remove(source_id_);
            if (loop_) g_main_loop_quit(loop_);
            if (server_thread_.joinable()) server_thread_.join();
            if (server_) g_object_unref(server_);
            if (loop_) g_main_loop_unref(loop_);
        }
    }

private:
    static void media_configure_callback(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
    void on_media_configure(GstRTSPMedia *media);

    static void need_data_callback(GstElement *appsrc, guint unused, gpointer user_data);
    static void enough_data_callback(GstElement *appsrc, gpointer user_data);
};

class CameraApp {
private:
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraManager> cameraManager_;
    std::unique_ptr<CameraConfiguration> config_;
    Stream *hr_stream_{nullptr}, *lr_stream_{nullptr};
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::vector<void*> hr_buffer_mappings_;
    std::vector<void*> lr_buffer_mappings_;
    
    std::atomic<bool> stopping_{false};
    long frameCount_ = 0;
    long long last_timestamp_ = 0;

    std::unique_ptr<GStreamerRTSPServer> rtsp_server_;
    ThreadSafeQueue<FrameData> lr_frame_queue_;
    std::thread lr_processing_thread_;

public:
    ThreadSafeQueue<FrameData> hr_frame_queue_;

    CameraApp() = default;
    ~CameraApp() { cleanup(); }

    bool initialize() {
        cameraManager_ = std::make_unique<CameraManager>();
        if (cameraManager_->start()) return false;
        if (cameraManager_->cameras().empty()) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "[ERROR] No cameras found" << std::endl;
            return false;
        }
        std::string cameraId = cameraManager_->cameras()[0]->id();
        camera_ = cameraManager_->get(cameraId);
        if (camera_->acquire()) return false;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "[INFO] Using camera: " << cameraId << std::endl;
        }
        rtsp_server_ = std::make_unique<GStreamerRTSPServer>();
        return true;
    }

    bool configure() {
        config_ = camera_->generateConfiguration({ StreamRole::VideoRecording, StreamRole::Viewfinder });
        
        // Use YUYV for HR stream and convert in GStreamer
        config_->at(0).pixelFormat = formats::YUYV;
        config_->at(0).size = Size(CAPTURE_WIDTH_HR, CAPTURE_HEIGHT_HR);
        config_->at(0).bufferCount = 8;
        
        // Configure YUYV stream for local processing (keep 480p as requested)
        config_->at(1).pixelFormat = formats::YUYV;
        config_->at(1).size = Size(CAPTURE_WIDTH_LR, CAPTURE_HEIGHT_LR);
        config_->at(1).bufferCount = 4;
        
        if (config_->validate() == CameraConfiguration::Invalid || camera_->configure(config_.get()) < 0) return false;

        hr_stream_ = config_->at(0).stream();
        lr_stream_ = config_->at(1).stream();

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        for (auto& cfg : *config_) {
            if (allocator_->allocate(cfg.stream()) < 0) return false;
            for (const auto& buffer : allocator_->buffers(cfg.stream())) {
                void* memory = mmap(nullptr, buffer->planes()[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->planes()[0].fd.get(), 0);
                if (cfg.stream() == hr_stream_) hr_buffer_mappings_.push_back(memory);
                else lr_buffer_mappings_.push_back(memory);
            }
        }
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "[INFO] Streams configured and all buffers mapped." << std::endl;
        return true;
    }

    bool start() {
        if (!rtsp_server_->start(this)) return false;
        
        lr_processing_thread_ = std::thread(&CameraApp::process_lr_frames, this);
        camera_->requestCompleted.connect(this, &CameraApp::onRequestCompleted);

        for (auto& cfg : *config_) {
            for (const auto& buffer : allocator_->buffers(cfg.stream())) {
                auto request = camera_->createRequest();
                if (request->addBuffer(cfg.stream(), buffer.get()) < 0) return false;
                requests_.push_back(std::move(request));
            }
        }

        ControlList controls;
        int64_t frame_time = 1000000 / CAPTURE_FPS;
        controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({frame_time, frame_time}));
        if (camera_->start(&controls)) return false;

        for (auto& req : requests_) camera_->queueRequest(req.get());
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "[INFO] Camera started and initial requests queued." << std::endl;
        }
        return true;
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "\n[INFO] Stopping application..." << std::endl;
        }
        
        hr_frame_queue_.stop();
        lr_frame_queue_.stop();

        if (lr_processing_thread_.joinable()) lr_processing_thread_.join();
        
        if (camera_) {
            camera_->stop();
            camera_->requestCompleted.disconnect(this, &CameraApp::onRequestCompleted);
        }

        if (rtsp_server_) rtsp_server_->stop();
    }

private:
    void onRequestCompleted(Request* request) {
        if (stopping_.load()) return;

        if (request->status() != Request::RequestComplete) {
            if (request->status() != Request::RequestCancelled) {
                 request->reuse(Request::ReuseBuffers);
                 if (camera_ && !stopping_.load()) camera_->queueRequest(request);
            }
            return;
        }

        auto get_buffer_index = [&](Stream* stream, FrameBuffer* buffer) {
            const auto& buffers = allocator_->buffers(stream);
            for (size_t i = 0; i < buffers.size(); ++i) if (buffers[i].get() == buffer) return i;
            return (size_t)-1;
        };

        auto it_hr = request->buffers().find(hr_stream_);
        if (it_hr != request->buffers().end()) {
            size_t idx = get_buffer_index(hr_stream_, it_hr->second);
            if (idx != (size_t)-1) {
                hr_frame_queue_.push({it_hr->second->planes()[0].length, hr_buffer_mappings_[idx]});
            }
        }

        auto it_lr = request->buffers().find(lr_stream_);
        if (it_lr != request->buffers().end()) {
            size_t idx = get_buffer_index(lr_stream_, it_lr->second);
            if (idx != (size_t)-1) {
                lr_frame_queue_.push({it_lr->second->planes()[0].length, lr_buffer_mappings_[idx]});
            }
        }

        if (++frameCount_ % (CAPTURE_FPS * 3) == 0) {
            long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            if (last_timestamp_ != 0) {
                double fps = (double)(CAPTURE_FPS * 3) * 1000.0 / (now - last_timestamp_);
                std::lock_guard<std::mutex> lock(log_mutex);
                std::cout << "[STATUS] HR Stream FPS: " << std::fixed << std::setprecision(2) << fps << std::endl;
            }
            last_timestamp_ = now;
        }

        request->reuse(Request::ReuseBuffers);
        if (camera_ && !stopping_.load()) camera_->queueRequest(request);
    }

    void process_lr_frames() {
        int frame_count = 0;
        while (!stopping_.load()) {
            FrameData frame;
            if (lr_frame_queue_.wait_and_pop(frame)) {
                if (++frame_count % 30 == 0) {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    std::cout << "[INFO] Received 30 LR (YUYV) frames. Latest frame size: " << frame.size << std::endl;
                }
            }
        }
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "[INFO] LR frame processing thread finished." << std::endl;
    }

    void cleanup() {
        stop();
        if (allocator_) {
            if (hr_stream_ && !hr_buffer_mappings_.empty()) {
                 for (size_t i = 0; i < hr_buffer_mappings_.size(); ++i) munmap(hr_buffer_mappings_[i], allocator_->buffers(hr_stream_)[i]->planes()[0].length);
            }
            if (lr_stream_ && !lr_buffer_mappings_.empty()) {
                 for (size_t i = 0; i < lr_buffer_mappings_.size(); ++i) munmap(lr_buffer_mappings_[i], allocator_->buffers(lr_stream_)[i]->planes()[0].length);
            }
            allocator_.reset();
        }
        requests_.clear();
        if (camera_) camera_->release();
        if (cameraManager_) cameraManager_->stop();
        {
             std::lock_guard<std::mutex> lock(log_mutex);
             std::cout << "[INFO] Cleanup complete." << std::endl;
        }
    }
};

// GStreamer RTSP Server Method Implementations
void GStreamerRTSPServer::on_media_configure(GstRTSPMedia *media) {
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    appsrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "bsaps_src"));
    
    if (!appsrc_) {
        g_print("Failed to get appsrc element\n");
        return;
    }
    
    // Configure appsrc properties for YUYV stream
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "YUY2",
        "width", G_TYPE_INT, CAPTURE_WIDTH_HR,
        "height", G_TYPE_INT, CAPTURE_HEIGHT_HR,
        "framerate", GST_TYPE_FRACTION, CAPTURE_FPS, 1,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
        NULL);
    
    g_object_set(G_OBJECT(appsrc_),
        "caps", caps,
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        "do-timestamp", FALSE,
        "min-latency", G_GINT64_CONSTANT(0),
        "max-latency", G_GINT64_CONSTANT(0),
        "block", FALSE,
        "max-bytes", G_GUINT64_CONSTANT(0),
        "emit-signals", TRUE,
        NULL);
    gst_caps_unref(caps);
    
    g_signal_connect(appsrc_, "need-data", (GCallback)need_data_callback, this);
    g_signal_connect(appsrc_, "enough-data", (GCallback)enough_data_callback, this);
}

void GStreamerRTSPServer::media_configure_callback(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
    static_cast<GStreamerRTSPServer*>(user_data)->on_media_configure(media);
}

static gboolean push_frame_idle_wrapper(gpointer user_data) {
    if (!user_data) return G_SOURCE_REMOVE;
    GStreamerRTSPServer* server = static_cast<GStreamerRTSPServer*>(user_data);
    if (!server) return G_SOURCE_REMOVE;
    return server->push_frame_idle();
}

gboolean GStreamerRTSPServer::push_frame_idle() {
    if (!appsrc_ || !app_) {
        return G_SOURCE_REMOVE;
    }
    
    // Check if appsrc is still valid GStreamer object
    if (!GST_IS_APP_SRC(appsrc_)) {
        return G_SOURCE_REMOVE;
    }
    
    FrameData frame;
    if (app_->hr_frame_queue_.try_pop(frame)) {
        if (!frame.data || frame.size == 0) {
            return G_SOURCE_CONTINUE;
        }
        
        // Create buffer with proper timestamp
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.size, nullptr);
        if (!buffer) {
            return G_SOURCE_CONTINUE;
        }
        
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            return G_SOURCE_CONTINUE;
        }
        
        memcpy(map.data, frame.data, frame.size);
        gst_buffer_unmap(buffer, &map);
        
        // Set timestamp using running time
        static GstClockTime start_time = GST_CLOCK_TIME_NONE;
        static guint64 frame_count = 0;
        
        if (start_time == GST_CLOCK_TIME_NONE) {
            start_time = gst_util_get_timestamp();
        }
        
        GstClockTime duration = GST_SECOND / CAPTURE_FPS;
        GstClockTime timestamp = frame_count * duration;
        
        GST_BUFFER_PTS(buffer) = timestamp;
        GST_BUFFER_DTS(buffer) = timestamp;
        GST_BUFFER_DURATION(buffer) = duration;
        GST_BUFFER_OFFSET(buffer) = frame_count;
        GST_BUFFER_OFFSET_END(buffer) = frame_count + 1;
        
        frame_count++;
        
        GstFlowReturn ret;
        g_signal_emit_by_name(appsrc_, "push-buffer", buffer, &ret);
        
        if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

void GStreamerRTSPServer::need_data_callback(GstElement *appsrc, guint unused, gpointer user_data) {
    if (!user_data) return;
    GStreamerRTSPServer* self = static_cast<GStreamerRTSPServer*>(user_data);
    if (!self || !self->is_running_.load()) return;
    
    if (self->source_id_ == 0) {
        self->source_id_ = g_idle_add(push_frame_idle_wrapper, user_data);
    }
}

void GStreamerRTSPServer::enough_data_callback(GstElement *appsrc, gpointer user_data) {
    if (!user_data) return;
    GStreamerRTSPServer* self = static_cast<GStreamerRTSPServer*>(user_data);
    if (!self || !self->is_running_.load()) return;
    
    if (self->source_id_ != 0) {
        g_source_remove(self->source_id_);
        self->source_id_ = 0;
    }
}

static std::atomic<bool> should_exit{false};
static CameraApp* app_instance = nullptr;

void signal_handler(int signal) {
    if (should_exit.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "\n[INFO] Signal " << signal << " received. Initiating graceful shutdown..." << std::endl;
    }
    if (app_instance) app_instance->stop();
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        CameraApp app;
        app_instance = &app;
        if (!app.initialize() || !app.configure() || !app.start()) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "[FATAL] Application setup failed." << std::endl;
            return -1;
        }
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "[INFO] Running... Press Ctrl+C to stop." << std::endl;
        }
        while (!should_exit.load()) {
            std::this_thread::sleep_for(100ms);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cerr << "[FATAL] An unhandled exception occurred: " << e.what() << std::endl;
        return -1;
    }
    
    std::this_thread::sleep_for(500ms); // Final delay for GStreamer cleanup
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "[INFO] Application terminated." << std::endl;
    }
    return 0;
}