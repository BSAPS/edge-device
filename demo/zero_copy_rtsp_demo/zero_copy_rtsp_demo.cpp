/*

※ How to Compile

g++ -std=c++17 -g -O2 -Wall -I/usr/include/libcamera \
`pkg-config --cflags gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0` \
-o zero_copy_rtsp_demo zero_copy_rtsp_demo.cpp -lcamera -lcamera-base \
`pkg-config --libs gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0` -lpthread


*/ 



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

// RTSP 서버 설정을 위한 상수
constexpr int RTSP_PORT = 8554;
constexpr const char* RTSP_MOUNT_POINT = "/stream";
constexpr int CAPTURE_WIDTH = 1920;
constexpr int CAPTURE_HEIGHT = 1080;
constexpr int CAPTURE_FPS = 30;

// 프레임 데이터를 담을 구조체
struct FrameData {
    void* data;
    size_t size;
    size_t buffer_index;
};

// 스레드 안전 큐 (Blocking Pop 기능 추가)
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

    // 대기하며 pop 하는 함수
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
        stopped.store(true);
        cv.notify_all(); // 모든 대기 중인 스레드를 깨움
    }
};


class RTSPServer {
private:
    GMainLoop *loop;
    GstRTSPServer *server;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;
    GstAppSrc *appsrc;
    std::thread server_thread;
    std::atomic<bool> is_running;
    int width, height, fps;
    GstClockTime timestamp;

public:
    RTSPServer(int port, const std::string& mount_point, int w, int h, int f)
        : loop(nullptr), server(nullptr), mounts(nullptr), factory(nullptr), appsrc(nullptr),
          is_running(false), width(w), height(h), fps(f), timestamp(0) {
        std::cout << "[INFO] Initializing GStreamer..." << std::endl;
        gst_init(nullptr, nullptr);
    }

    ~RTSPServer() {
        stop();
        std::cout << "[INFO] Deinitializing GStreamer..." << std::endl;
        gst_deinit();
    }

    bool start() {
        loop = g_main_loop_new(NULL, FALSE);
        server = gst_rtsp_server_new();
        g_object_set(server, "service", std::to_string(RTSP_PORT).c_str(), NULL);
        mounts = gst_rtsp_server_get_mount_points(server);
        factory = gst_rtsp_media_factory_new();

        // ** GStreamer 파이프라인 수정: v4l2convert와 v4l2h264enc 사이에 queue 추가 **
        std::string pipeline_str = "appsrc name=mysrc ! queue ! v4l2convert ! video/x-raw,format=NV12 ! queue ! v4l2h264enc ! video/x-h264,level=(string)4 ! rtph264pay name=pay0 pt=96";
        std::cout << "[DEBUG] GStreamer Pipeline: " << pipeline_str << std::endl;
        gst_rtsp_media_factory_set_launch(factory, pipeline_str.c_str());
        gst_rtsp_media_factory_set_shared(factory, TRUE);

        g_signal_connect(factory, "media-configure", (GCallback)media_configure_callback, this);
        
        gst_rtsp_mount_points_add_factory(mounts, RTSP_MOUNT_POINT, factory);
        g_object_unref(mounts);

        if (gst_rtsp_server_attach(server, NULL) == 0) {
            std::cerr << "[ERROR] Failed to attach RTSP server. Ensure the port is not in use." << std::endl;
            return false;
        }

        is_running.store(true);
        server_thread = std::thread([this]() {
            std::cout << "[INFO] RTSP stream ready at: rtsp://<your-ip-address>:" << RTSP_PORT << RTSP_MOUNT_POINT << std::endl;
            g_main_loop_run(loop);
            std::cout << "[INFO] GStreamer main loop finished." << std::endl;
        });

        return true;
    }

    void stop() {
        if (is_running.exchange(false)) {
            std::cout << "[INFO] Stopping RTSP server..." << std::endl;
            if (loop) {
                g_main_loop_quit(loop);
            }
            if (server_thread.joinable()) {
                server_thread.join();
            }
            if (server) {
                g_object_unref(server);
                server = nullptr;
            }
            if (loop) {
                g_main_loop_unref(loop);
                loop = nullptr;
            }
            std::cout << "[INFO] RTSP server stopped." << std::endl;
        }
    }

    void pushFrame(void* data, size_t size, int fd) {
        if (!is_running.load() || !appsrc) {
            return;
        }

        GstState state;
        GstStateChangeReturn ret_state = gst_element_get_state(GST_ELEMENT(appsrc), &state, nullptr, GST_CLOCK_TIME_NONE);
        if (ret_state != GST_STATE_CHANGE_SUCCESS || state != GST_STATE_PLAYING) {
            return;
        }

        GstBuffer* buffer = gst_buffer_new();
        GstMemory* memory = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, data, size, 0, size, nullptr, nullptr);
        gst_buffer_append_memory(buffer, memory);

        GstFlowReturn ret;
        g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
        gst_buffer_unref(buffer);

        if (ret != GST_FLOW_OK) {
            std::cerr << "[WARN] Error pushing buffer to appsrc, flow return: " << gst_flow_get_name(ret) << std::endl;
        }
    }

private:
    static void media_configure_callback(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
        RTSPServer* self = static_cast<RTSPServer*>(user_data);
        self->on_media_configure(media);
    }

    void on_media_configure(GstRTSPMedia *media) {
        std::cout << "[DEBUG] Media configure callback triggered." << std::endl;
        GstElement *pipeline = gst_rtsp_media_get_element(media);
        GstElement *appsrc_element = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");

        if (!appsrc_element) {
            std::cerr << "[ERROR] Could not find appsrc element 'mysrc' in pipeline" << std::endl;
            return;
        }
        
        // ** Appsrc Caps 수정: colorimetry 값을 "srgb"로 변경 **
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                            "format", G_TYPE_STRING, "RGB",
                                            "width", G_TYPE_INT, width,
                                            "height", G_TYPE_INT, height,
                                            "framerate", GST_TYPE_FRACTION, fps, 1,
                                            "interlace-mode", G_TYPE_STRING, "progressive",
                                            "colorimetry", G_TYPE_STRING, "srgb",
                                            NULL);
        
        std::cout << "[DEBUG] Setting appsrc caps to: " << gst_caps_to_string(caps) << std::endl;

        g_object_set(G_OBJECT(appsrc_element),
                     "caps", caps,
                     "format", GST_FORMAT_TIME,
                     "is-live", TRUE,
                     "do-timestamp", TRUE,
                     NULL);
        gst_caps_unref(caps);

        this->appsrc = GST_APP_SRC(appsrc_element);
        g_object_unref(appsrc_element);
    }
};

class CameraStreamer {
private:
    std::shared_ptr<Camera> camera;
    std::unique_ptr<CameraManager> cameraManager;
    std::unique_ptr<CameraConfiguration> config;
    Stream* stream;
    std::shared_ptr<FrameBufferAllocator> allocator;
    std::vector<std::vector<void*>> bufferPlaneMappings;
    
    std::atomic<bool> stopping;
    
    std::unique_ptr<RTSPServer> rtsp_server;
    
    ThreadSafeQueue<FrameData> frame_queue;
    std::thread processing_thread;
    size_t frameCount;

public:
    CameraStreamer() : stream(nullptr), stopping(false), frameCount(0) {}

    ~CameraStreamer() {
        cleanup();
    }

    bool initialize() {
        std::cout << "[INFO] Initializing CameraStreamer..." << std::endl;
        cameraManager = std::make_unique<CameraManager>();
        if (cameraManager->start()) {
            std::cerr << "[ERROR] Failed to start camera manager" << std::endl;
            return false;
        }
        if (cameraManager->cameras().empty()) {
            std::cerr << "[ERROR] No cameras found" << std::endl;
            return false;
        }
        std::string cameraId = cameraManager->cameras()[0]->id();
        camera = cameraManager->get(cameraId);
        std::cout << "[INFO] Using camera: " << cameraId << std::endl;

        if (camera->acquire()) {
            std::cerr << "[ERROR] Failed to acquire camera" << std::endl;
            return false;
        }

        config = camera->generateConfiguration({StreamRole::Viewfinder});
        StreamConfiguration& streamConfig = config->at(0);
        streamConfig.size = Size(CAPTURE_WIDTH, CAPTURE_HEIGHT);
        // ** Libcamera 색상 포맷 : RGB888 -> BGR888 **
        streamConfig.pixelFormat = libcamera::formats::BGR888;
        streamConfig.bufferCount = 8;
        config->validate();
        
        if (camera->configure(config.get()) < 0) {
            std::cerr << "[ERROR] Failed to configure camera" << std::endl;
            return false;
        }
        stream = streamConfig.stream();

        std::cout << "[INFO] Stream configured: " << streamConfig.size.width << "x" << streamConfig.size.height
                  << " " << streamConfig.pixelFormat.toString() << " with " << streamConfig.bufferCount << " buffers." << std::endl;

        rtsp_server = std::make_unique<RTSPServer>(RTSP_PORT, RTSP_MOUNT_POINT, CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_FPS);

        return setupBuffers();
    }

    bool setupBuffers() {
        std::cout << "[INFO] Setting up DMA buffers..." << std::endl;
        allocator = std::make_shared<FrameBufferAllocator>(camera);
        if (allocator->allocate(stream) < 0) {
            std::cerr << "[ERROR] Failed to allocate buffers" << std::endl;
            return false;
        }
        for (const auto& buffer : allocator->buffers(stream)) {
            std::vector<void*> planeMappings;
            for (const auto& plane : buffer->planes()) {
                void* memory = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, plane.fd.get(), 0);
                if (memory == MAP_FAILED) {
                    std::cerr << "[ERROR] Failed to mmap buffer plane" << std::endl;
                    return false;
                }
                planeMappings.push_back(memory);
            }
            bufferPlaneMappings.push_back(planeMappings);
        }
        std::cout << "[INFO] " << bufferPlaneMappings.size() << " DMA buffers mapped successfully." << std::endl;
        return true;
    }

    void cleanup() {
        std::cout << "[INFO] Cleaning up resources..." << std::endl;
        stop();
        if (allocator) {
            const auto& buffers = allocator->buffers(stream);
            for (size_t i = 0; i < bufferPlaneMappings.size(); ++i) {
                for (size_t j = 0; j < bufferPlaneMappings[i].size(); ++j) {
                    if (bufferPlaneMappings[i][j] != MAP_FAILED) {
                        munmap(bufferPlaneMappings[i][j], buffers[i]->planes()[j].length);
                    }
                }
            }
        }
        if (camera) {
            camera->release();
        }
        std::cout << "[INFO] Cleanup complete." << std::endl;
    }

    bool start() {
        if (!camera || !rtsp_server) {
            std::cerr << "[ERROR] Cannot start, camera or RTSP server not initialized." << std::endl;
            return false;
        }
        
        std::cout << "[INFO] Starting RTSP Server..." << std::endl;
        if (!rtsp_server->start()) {
            std::cerr << "[ERROR] Failed to start RTSP server" << std::endl;
            return false;
        }
        
        processing_thread = std::thread(&CameraStreamer::processFrames, this);

        camera->requestCompleted.connect(this, &CameraStreamer::onRequestCompleted);

        std::vector<std::unique_ptr<Request>> requests;
        for (const auto& buffer : allocator->buffers(stream)) {
            auto request = camera->createRequest();
            if (!request || request->addBuffer(stream, buffer.get()) < 0) {
                std::cerr << "[ERROR] Failed to create request or add buffer" << std::endl;
                return false;
            }
            requests.push_back(std::move(request));
        }

        ControlList controls;
        int64_t frame_time = 1000000 / CAPTURE_FPS;
        controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({frame_time, frame_time}));

        std::cout << "[INFO] Starting camera..." << std::endl;
        if (camera->start(&controls)) {
            std::cerr << "[ERROR] Failed to start camera" << std::endl;
            return false;
        }
        for (auto& request : requests) {
            camera->queueRequest(request.release());
        }
        std::cout << "[INFO] Camera started and initial requests queued." << std::endl;
        return true;
    }

    void stop() {
        if (stopping.exchange(true)) return;
        
        std::cout << "[INFO] Stopping camera streamer..." << std::endl;
        
        frame_queue.stop(); 
        if (processing_thread.joinable()) {
            processing_thread.join();
        }
        if (rtsp_server) {
            rtsp_server->stop();
        }
        if (camera) {
            camera->stop();
            camera->requestCompleted.disconnect(this, &CameraStreamer::onRequestCompleted);
        }
    }

    void onRequestCompleted(Request* request) {
        if (stopping.load()) {
            request->reuse(Request::ReuseBuffers);
            camera->queueRequest(request);
            return;
        }

        if (request->status() != Request::RequestComplete) {
            if (request->status() != Request::RequestCancelled) {
                 std::cerr << "[WARN] Request failed with status " << request->status() << std::endl;
            }
            request->reuse(Request::ReuseBuffers);
            camera->queueRequest(request);
            return;
        }

        FrameBuffer* buffer = request->buffers().begin()->second;
        size_t bufferIndex = 0;
        const auto& buffers = allocator->buffers(stream);
        for (size_t i = 0; i < buffers.size(); ++i) {
            if (buffers[i].get() == buffer) {
                bufferIndex = i;
                break;
            }
        }
        
        // ** 프레임 크기 계산 복원: RGB888은 단일 평면이므로 첫 번째 plane의 크기만 사용 **
        frame_queue.push({bufferPlaneMappings[bufferIndex][0], buffer->planes()[0].length, bufferIndex});
        
        request->reuse(Request::ReuseBuffers);
        camera->queueRequest(request);
    }
    
    void processFrames() {
        while (!stopping.load()) {
            FrameData frame;
            if (frame_queue.wait_and_pop(frame)) {
                rtsp_server->pushFrame(frame.data, frame.size, frame.buffer_index);
                frameCount++;
                if (frameCount % (CAPTURE_FPS * 5) == 0) {
                    std::cout << "[DEBUG] " << frameCount << " frames processed and sent to RTSP server." << std::endl;
                }
            }
        }
        std::cout << "[INFO] Frame processing thread finished." << std::endl;
    }
};

static std::atomic<bool> shouldExit{false};
static CameraStreamer* streamerInstance = nullptr;

void signalHandler(int signal) {
    std::cout << "\n[INFO] Signal " << signal << " received. Exiting gracefully..." << std::endl;
    shouldExit.store(true);
    if (streamerInstance) {
        streamerInstance->stop();
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "========================================================" << std::endl;
    std::cout << "   Zero-Copy Camera to RTSP Streamer" << std::endl;
    std::cout << "========================================================" << std::endl;

    try {
        CameraStreamer streamer;
        streamerInstance = &streamer;

        if (!streamer.initialize()) {
            std::cerr << "[FATAL] Initialization failed. Exiting." << std::endl;
            return -1;
        }

        if (!streamer.start()) {
            std::cerr << "[FATAL] Streamer start failed. Exiting." << std::endl;
            return -1;
        }

        while (!shouldExit.load()) {
            std::this_thread::sleep_for(100ms);
        }

        std::cout << "\n[INFO] Main loop exited. Stopping streamer..." << std::endl;
        streamer.stop();
        std::cout << "[INFO] Streamer stopped successfully." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] An unhandled exception occurred: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
