// Заголовок WebRTC-API из gst-plugins-bad помечен как unstable; define
// должен идти до любых gst/*.h включений, иначе -Werror=cpp сработает.
#define GST_USE_UNSTABLE_API

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json/json.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// =============================================================================
// Сообщения
// =============================================================================

namespace {

enum class MessageType : std::uint8_t { Offer, Answer, Ice, Error, Ping, Pong };

}  // namespace

template <>
struct std::formatter<MessageType> : std::formatter<std::string_view> {
    auto format(MessageType type, std::format_context& ctx) const {
        std::string_view name = "unknown";
        switch (type) {
            case MessageType::Offer:
                name = "offer";
                break;
            case MessageType::Answer:
                name = "answer";
                break;
            case MessageType::Ice:
                name = "ice";
                break;
            case MessageType::Error:
                name = "error";
                break;
            case MessageType::Ping:
                name = "ping";
                break;
            case MessageType::Pong:
                name = "pong";
                break;
        }
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

namespace {

// =============================================================================
// JSON helpers (jsoncpp, встроенный в Drogon)
// =============================================================================

std::string JsonDump(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    auto result = Json::writeString(writer, value);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

std::optional<Json::Value> JsonParse(std::string_view text) {
    Json::CharReaderBuilder reader;
    reader["collectComments"] = false;
    std::string errors;
    Json::Value value;
    std::istringstream stream{std::string(text)};
    if (!Json::parseFromStream(reader, stream, &value, &errors)) return std::nullopt;
    return value;
}

std::string JsonStr(const Json::Value& value, const char* key) {
    if (value.isObject() && value.isMember(key) && value[key].isString()) return value[key].asString();
    return {};
}

int JsonInt(const Json::Value& value, const char* key, int defaultValue = 0) {
    if (value.isObject() && value.isMember(key) && value[key].isNumeric()) return value[key].asInt();
    return defaultValue;
}

std::string Truncate(std::string_view text, size_t length = 120) {
    if (text.size() <= length) return std::string(text);
    return std::string(text.substr(0, length)) + "...";
}

// =============================================================================
// Камеры
// =============================================================================

bool DeviceSupportsH264(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ::close(fd);
        return false;
    }
    auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        ::close(fd);
        return false;
    }

    bool hasH264 = false;
    v4l2_fmtdesc fmtDesc{};
    fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtDesc.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0; ++fmtDesc.index) {
        if (fmtDesc.pixelformat == V4L2_PIX_FMT_H264) {
            hasH264 = true;
            break;
        }
    }
    ::close(fd);
    return hasH264;
}

std::vector<std::string> FindH264Cameras() {
    std::vector<std::string> cameras;
    for (int i = 0; i < 64; ++i) {
        auto path = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(path)) continue;
        if (DeviceSupportsH264(path)) {
            std::println("[cam] {}: H.264 ✓", path);
            cameras.push_back(path);
        }
    }
    return cameras;
}

void PrintAllCameras() {
    std::println(stderr, "Scanning /dev/video*:");
    bool any = false;
    for (int i = 0; i < 64; ++i) {
        auto path = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(path)) continue;
        any = true;

        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::println(stderr, "  {}: cannot open ({})", path, std::strerror(errno));
            continue;
        }

        v4l2_capability cap{};
        std::string name = "<unknown>";
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) name = reinterpret_cast<const char*>(cap.card);

        std::string formats;
        v4l2_fmtdesc fmtDesc{};
        fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fmtDesc.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0; ++fmtDesc.index) {
            if (!formats.empty()) formats += ", ";
            formats += reinterpret_cast<const char*>(&fmtDesc.description);
        }
        ::close(fd);

        std::println(stderr, "  {} [{}]: {}", path, name, formats.empty() ? "<no capture formats>" : formats);
    }
    if (!any) std::println(stderr, "  (no /dev/video* devices found)");
}

// =============================================================================
// WebRTCSession
// =============================================================================

class WebRTCSession {
   public:
    // C++26: std::copyable_function — более лёгкая замена std::function.
    using SendFn = std::copyable_function<void(std::string)>;

    WebRTCSession(std::string device, SendFn send) : device_(std::move(device)), send_(std::move(send)) {}

    ~WebRTCSession() {
        std::println("[rtc] ~WebRTCSession");
        if (webrtcBin_) g_signal_handlers_disconnect_by_data(webrtcBin_, this);
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    WebRTCSession(const WebRTCSession&) = delete;
    WebRTCSession& operator=(const WebRTCSession&) = delete;

    std::expected<void, std::string> Start() {
        std::println("[rtc] === Start() ===");
        std::println("[rtc] device: {}", device_);

        if (!std::filesystem::exists(device_)) return std::unexpected("device not found: " + device_);
        if (std::ifstream probe(device_); !probe)
            return std::unexpected("cannot open " + device_ + " (check permissions: 'id' contains 'video'?)");

        pipeline_ = gst_pipeline_new("webrtc-pipeline");
        if (!pipeline_) return std::unexpected("gst_pipeline_new failed");

        auto makeElement = [](const char* factory, const char* name) -> GstElement* {
            auto* element = gst_element_factory_make(factory, name);
            if (!element)
                std::println(stderr, "[rtc] !! factory_make(\"{}\") FAILED", factory);
            else
                std::println("[rtc] + {} ('{}')", factory, name);
            return element;
        };

        GstElement* src = makeElement("v4l2src", "src");
        GstElement* caps = makeElement("capsfilter", "caps");
        GstElement* parser = makeElement("h264parse", "parser");
        GstElement* pay = makeElement("rtph264pay", "pay");
        GstElement* queue = makeElement("queue", "queue");
        webrtcBin_ = makeElement("webrtcbin", "webrtcbin");

        if (!src) return std::unexpected("v4l2src missing (gstreamer1.0-plugins-good)");
        if (!caps) return std::unexpected("capsfilter missing");
        if (!parser) return std::unexpected("h264parse missing (gstreamer1.0-plugins-bad)");
        if (!pay) return std::unexpected("rtph264pay missing (gstreamer1.0-plugins-good)");
        if (!queue) return std::unexpected("queue missing");
        if (!webrtcBin_) return std::unexpected("webrtcbin missing (vcpkg gstreamer[webrtc])");

        g_object_set(src, "device", device_.c_str(), nullptr);
        GstCaps* videoCaps = gst_caps_from_string("video/x-h264");
        g_object_set(caps, "caps", videoCaps, nullptr);
        gst_caps_unref(videoCaps);
        g_object_set(pay, "config-interval", -1, nullptr);
        g_object_set(pay, "pt", 96, nullptr);

        if (!GST_IS_ELEMENT(webrtcBin_)) {
            return std::unexpected(
                "webrtcbin is not a valid GstElement "
                "(static registration of webrtc/nice/dtls/srtp failed)");
        }
        gst_bin_add_many(GST_BIN(pipeline_), src, caps, parser, pay, queue, webrtcBin_, nullptr);

        if (!gst_element_link_many(src, caps, parser, pay, queue, webrtcBin_, nullptr))
            return std::unexpected(
                "link failed: v4l2src ! video/x-h264 ! h264parse ! "
                "rtph264pay ! queue ! webrtcbin  (камера не отдаёт H.264?)");

        InstallBusWatch();
        ConnectWebRtcSignals();

        std::println("[rtc] set_state(PLAYING)");
        auto ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) return std::unexpected("set_state(PLAYING) failed");

        std::println("[rtc] state_change ret = {}", ret == GST_STATE_CHANGE_ASYNC     ? "ASYNC"
                                                    : ret == GST_STATE_CHANGE_SUCCESS ? "SUCCESS"
                                                                                      : "OTHER");
        return {};
    }

    void SetRemoteDescription(std::string_view type, std::string_view sdpText) {
        std::println("[rtc] SetRemoteDescription type={} size={}", type, sdpText.size());

        if (type == "answer" && remoteDescriptionSet_) {
            std::println(stderr, "[rtc] duplicate answer, ignoring");
            return;
        }

        std::string sdpCopy(sdpText);
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(sdpCopy.c_str(), &sdp) != GST_SDP_OK) {
            std::println(stderr, "[rtc] SDP parse failed ({}b)", sdpCopy.size());
            return;
        }

        auto sdpType = (type == "offer") ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER;

        GstWebRTCSessionDescription* description = gst_webrtc_session_description_new(sdpType, sdp);

        GstPromise* promise = gst_promise_new_with_change_func(
            [](GstPromise* p, gpointer) {
                auto result = gst_promise_wait(p);
                std::println("[rtc] set-remote-description done: {}",
                             result == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                gst_promise_unref(p);
            },
            nullptr, nullptr);
        g_signal_emit_by_name(webrtcBin_, "set-remote-description", description, promise);
        gst_webrtc_session_description_free(description);

        remoteDescriptionSet_ = true;
        std::println("[rtc] remote description emitted");
    }

    void AddIceCandidate(int mline, std::string_view candidate) {
        if (candidate.empty()) return;
        std::string value(candidate);
        g_signal_emit_by_name(webrtcBin_, "add-ice-candidate", mline, value.c_str());
    }

   private:
    void InstallBusWatch() {
        GstBus* bus = gst_element_get_bus(pipeline_);
        gst_bus_add_watch(
            bus,
            [](GstBus*, GstMessage* message, gpointer data) -> gboolean {
                GstElement* pipeline = static_cast<GstElement*>(data);
                switch (GST_MESSAGE_TYPE(message)) {
                    case GST_MESSAGE_ERROR: {
                        GError* error = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_error(message, &error, &debug);
                        std::println(stderr, "[gst] !! ERROR from {}: {}", GST_OBJECT_NAME(message->src),
                                     error ? error->message : "?");
                        if (debug) std::println(stderr, "[gst]    {}", debug);
                        if (error) g_error_free(error);
                        g_free(debug);
                        break;
                    }
                    case GST_MESSAGE_WARNING: {
                        GError* error = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_warning(message, &error, &debug);
                        std::println(stderr, "[gst] WARNING from {}: {}", GST_OBJECT_NAME(message->src),
                                     error ? error->message : "?");
                        if (debug) std::println(stderr, "[gst]    {}", debug);
                        if (error) g_error_free(error);
                        g_free(debug);
                        break;
                    }
                    case GST_MESSAGE_STATE_CHANGED:
                        if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
                            GstState oldState, newState, pending;
                            gst_message_parse_state_changed(message, &oldState, &newState, &pending);
                            std::println("[gst] pipeline {} -> {} (pending {})", gst_element_state_get_name(oldState),
                                         gst_element_state_get_name(newState), gst_element_state_get_name(pending));
                        }
                        break;
                    case GST_MESSAGE_EOS:
                        std::println("[gst] EOS");
                        break;
                    default:
                        break;
                }
                return TRUE;
            },
            pipeline_);
        gst_object_unref(bus);
    }

    void ConnectWebRtcSignals() {
        g_signal_connect(webrtcBin_, "on-negotiation-needed", G_CALLBACK(&WebRTCSession::OnNegotiationNeeded), this);
        g_signal_connect(webrtcBin_, "on-ice-candidate", G_CALLBACK(&WebRTCSession::OnIceCandidate), this);
        g_signal_connect(webrtcBin_, "pad-added", G_CALLBACK(&WebRTCSession::OnPadAdded), this);
    }

    static void OnNegotiationNeeded(GstElement* webrtc, gpointer data) {
        std::println("[rtc] << on-negotiation-needed >>");
        GstPromise* promise = gst_promise_new_with_change_func(&WebRTCSession::OnOfferCreated,
                                                               static_cast<WebRTCSession*>(data), nullptr);
        g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
    }

    static void OnIceCandidate(GstElement*, guint mline, gchar* candidate, gpointer data) {
        auto* self = static_cast<WebRTCSession*>(data);
        if (!candidate) {
            std::println("[rtc] local ICE end-of-candidates (mline={})", mline);
            return;
        }
        std::println("[rtc] local ICE mline={} cand={}", mline, candidate);

        Json::Value message;
        message["type"] = std::format("{}", MessageType::Ice);
        message["sdpMLineIndex"] = static_cast<Json::UInt>(mline);
        message["candidate"] = std::string(candidate);
        self->send_(JsonDump(message));
    }

    static void OnPadAdded(GstElement*, GstPad* pad, gpointer) {
        std::println("[rtc] webrtcbin pad-added: {}", GST_PAD_NAME(pad));
    }

    static void OnOfferCreated(GstPromise* promise, gpointer data) {
        auto* self = static_cast<WebRTCSession*>(data);
        std::println("[rtc] << on-offer-created >>");

        if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
            std::println(stderr, "[rtc] create-offer NOT-REPLIED");
            gst_promise_unref(promise);
            return;
        }

        const GstStructure* reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription* offer = nullptr;
        if (reply) {
            gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
        }

        if (offer) {
            gchar* sdp = gst_sdp_message_as_text(offer->sdp);
            std::string sdpString = sdp ? sdp : "";
            g_free(sdp);

            std::println("[rtc] sending offer ({}b)", sdpString.size());
            Json::Value message;
            message["type"] = std::format("{}", MessageType::Offer);
            message["sdp"] = std::move(sdpString);
            self->send_(JsonDump(message));

            GstPromise* localPromise = gst_promise_new_with_change_func(
                [](GstPromise* p, gpointer) {
                    auto result = gst_promise_wait(p);
                    std::println("[rtc] set-local-description done: {}",
                                 result == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                    gst_promise_unref(p);
                },
                nullptr, nullptr);
            g_signal_emit_by_name(self->webrtcBin_, "set-local-description", offer, localPromise);
            gst_webrtc_session_description_free(offer);
        } else {
            std::println(stderr, "[rtc] offer is null in reply");
        }

        gst_promise_unref(promise);
    }

    std::string device_;
    SendFn send_;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtcBin_ = nullptr;
    bool remoteDescriptionSet_ = false;
};

// =============================================================================
// main helpers
// =============================================================================

struct GLibLoop {
    GMainLoop* loop = nullptr;
    std::thread thread;

    GLibLoop() {
        loop = g_main_loop_new(nullptr, FALSE);
        thread = std::thread([this] { g_main_loop_run(loop); });
    }
    ~GLibLoop() {
        if (loop) g_main_loop_quit(loop);
        if (thread.joinable()) thread.join();
        if (loop) g_main_loop_unref(loop);
    }
    GLibLoop(const GLibLoop&) = delete;
    GLibLoop& operator=(const GLibLoop&) = delete;
};

std::string SelectCamera() {
    if (const char* env = std::getenv("WEBRTC_CAMERA"); env && *env) {
        std::string camera = env;
        std::println("Using camera from WEBRTC_CAMERA: {}", camera);
        if (!std::filesystem::exists(camera)) {
            std::println(stderr, "!! {} does not exist", camera);
            return {};
        }
        if (!DeviceSupportsH264(camera)) {
            std::println(stderr, "!! {} is not H.264-capable; passthrough pipeline will fail", camera);
        }
        return camera;
    }

    auto cameras = FindH264Cameras();
    if (cameras.empty()) {
        std::println(stderr,
                     "\nNo H.264-capable camera found.\n"
                     "Passthrough needs a camera that outputs raw H.264 over V4L2.\n\n");
        PrintAllCameras();
        std::println(stderr,
                     "\nOptions:\n"
                     "  1. Connect a camera that outputs H.264 "
                     "(Logitech C920/C922, most industrial cams).\n"
                     "  2. Force a specific device: WEBRTC_CAMERA=/dev/videoN ./webrtc\n"
                     "     (pipeline will fail unless that device really does H.264).\n"
                     "  3. Switch pipeline to transcode MJPEG/YUYV → H.264 "
                     "(decode + x264enc / v4l2h264enc).\n");
        return {};
    }

    std::println("H.264 camera(s) found:");
    for (auto& camera : cameras) std::println("  {}", camera);
    std::println("Using: {}", cameras.front());
    return cameras.front();
}

std::string LoadIndexHtml() {
    for (auto* path : {"index.html", "static/index.html", "scripts/static/index.html"}) {
        if (std::filesystem::exists(path)) {
            std::ifstream file(path);
            std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            std::println("Serving index from: {} ({}b)", path, content.size());
            return content;
        }
    }
    std::println(stderr,
                 "!! index.html not found near CWD. Returning stub. "
                 "CMake should have copied it into build-dir.");
    return "<h1>index.html not found</h1>";
}

}  // namespace

// =============================================================================
// WebRTCWebSocket — WebSocket-контроллер Drogon
// =============================================================================

class WebRTCWebSocket : public drogon::WebSocketController<WebRTCWebSocket> {
   public:
    static void SetCamera(std::string camera) { camera_ = std::move(camera); }

    void handleNewConnection(const drogon::HttpRequestPtr&, const drogon::WebSocketConnectionPtr& conn) override {
        std::println("[ws] === SESSION START ===");

        // ВАЖНО: send() нельзя вызывать из потока GStreamer напрямую.
        // Все отправки заворачиваем в event loop Drogon через queueInLoop.
        // weak_ptr разрывает цикл conn -> context -> WebRTCSession -> send_ -> conn.
        std::weak_ptr<drogon::WebSocketConnection> weakConn = conn;
        auto send = [weakConn](std::string message) {
            drogon::app().getLoop()->queueInLoop([weakConn, message = std::move(message)]() mutable {
                if (auto locked = weakConn.lock(); locked && locked->connected()) {
                    std::println("[ws] -> {}b: {}", message.size(), Truncate(message));
                    locked->send(std::move(message));
                }
            });
        };

        auto rtc = std::make_shared<WebRTCSession>(camera_, std::move(send));
        conn->setContext(rtc);  // shared_ptr<WebRTCSession> -> shared_ptr<void> неявно

        if (auto result = rtc->Start(); !result) {
            std::println(stderr, "[ws] !! Start FAILED: {}", result.error());
            Json::Value errorMessage;
            errorMessage["type"] = std::format("{}", MessageType::Error);
            errorMessage["message"] = result.error();
            conn->send(JsonDump(errorMessage));
            conn->setContext(std::shared_ptr<void>{});
            conn->shutdown();
        } else {
            std::println("[ws] pipeline started; waiting for answer/ice");
        }
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        if (type != drogon::WebSocketMessageType::Text) return;

        std::println("[ws] <- {}b: {}", message.size(), Truncate(message));

        auto parsed = JsonParse(message);
        if (!parsed) {
            std::println(stderr, "[ws] !! bad JSON");
            return;
        }
        const Json::Value& msg = *parsed;

        // getContext<T>() возвращает std::shared_ptr<T> по значению.
        auto rtc = conn->getContext<WebRTCSession>();
        if (!rtc) return;

        auto messageType = JsonStr(msg, "type");
        if (messageType == "answer") {
            auto sdp = JsonStr(msg, "sdp");
            std::println("[ws] applying answer ({}b)", sdp.size());
            rtc->SetRemoteDescription("answer", sdp);
        } else if (messageType == "ice") {
            rtc->AddIceCandidate(JsonInt(msg, "sdpMLineIndex"), JsonStr(msg, "candidate"));
        } else if (messageType == "ping") {
            Json::Value pong;
            pong["type"] = std::format("{}", MessageType::Pong);
            conn->send(JsonDump(pong));
        } else {
            std::println(stderr, "[ws] !! unknown type: {}", messageType);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        std::println("[ws] === SESSION END ===");
        // Сбрасываем shared_ptr<WebRTCSession> немедленно.
        if (conn->hasContext()) {
            conn->setContext(std::shared_ptr<void>{});
        }
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

   private:
    inline static std::string camera_;
};

// =============================================================================
// main
// =============================================================================

extern "C" {
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(videoconvertscale);
GST_PLUGIN_STATIC_DECLARE(video4linux2);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(webrtc);
GST_PLUGIN_STATIC_DECLARE(nice);
GST_PLUGIN_STATIC_DECLARE(dtls);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(udp);
}

int main() {
    gst_init(nullptr, nullptr);

    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(videoconvertscale);
    GST_PLUGIN_STATIC_REGISTER(video4linux2);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(webrtc);
    GST_PLUGIN_STATIC_REGISTER(nice);
    GST_PLUGIN_STATIC_REGISTER(dtls);
    GST_PLUGIN_STATIC_REGISTER(srtp);
    GST_PLUGIN_STATIC_REGISTER(udp);

    GLibLoop glib;

    std::string camera = SelectCamera();
    if (camera.empty()) return 1;
    WebRTCWebSocket::SetCamera(std::move(camera));

    auto html = std::make_shared<std::string>(LoadIndexHtml());
    auto serveIndex = [html](const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setContentTypeString("text/html; charset=utf-8");
        response->setBody(*html);
        callback(response);
    };

    std::println("HTTP/WS on http://0.0.0.0:8888");

    drogon::app()
        .addListener("0.0.0.0", 8888)
        .registerHandler("/", serveIndex)
        .registerHandler("/index.html", serveIndex)
        .run();

    std::println("Bye");
    return 0;
}
