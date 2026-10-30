// WebRTC PoC streaming server — single-file, C++26 (GCC 16.2).
//
// Слои:
//   1. VideoStreamer   — камера + GStreamer + GLib-цикл (только его забота).
//   2. WebRTCWebSocket — signaling: JSON ↔ методы стримера.
//
// Жизненные циклы:
//   • VideoStreamer — на стеке в main(), живёт всё время работы приложения.
//   • WebRTCWebSocket — по одному на WS-соединение (создаётся Drogon'ом
//     через лямбду в registerWebSocketController), хранит VideoStreamer&.
//   • Никакого глобального/статического состояния приложения.

#define GST_USE_UNSTABLE_API  // ОБЯЗАТЕЛЬНО до любых gst/*.h

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
#include <cstdio>
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
#include <utility>
#include <vector>

// =============================================================================
// Signaling protocol
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
// JSON helpers
// =============================================================================

std::string JsonDump(const Json::Value& value) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    auto s = Json::writeString(w, value);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::optional<Json::Value> JsonParse(std::string_view text) {
    Json::CharReaderBuilder r;
    r["collectComments"] = false;
    std::string errs;
    Json::Value v;
    std::istringstream ss{std::string(text)};
    if (!Json::parseFromStream(r, ss, &v, &errs)) return std::nullopt;
    return v;
}

std::string JsonStr(const Json::Value& v, const char* key) {
    if (v.isObject() && v.isMember(key) && v[key].isString()) return v[key].asString();
    return {};
}

int JsonInt(const Json::Value& v, const char* key, int def = 0) {
    if (v.isObject() && v.isMember(key) && v[key].isNumeric()) return v[key].asInt();
    return def;
}

std::string Truncate(std::string_view text, size_t len = 120) {
    if (text.size() <= len) return std::string(text);
    return std::string(text.substr(0, len)) + "...";
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
    std::vector<std::string> cams;
    for (int i = 0; i < 64; ++i) {
        auto path = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(path)) continue;
        if (DeviceSupportsH264(path)) {
            std::println("[cam] {}: H.264 ✓", path);
            cams.push_back(path);
        }
    }
    return cams;
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
// Слой 1: VideoStreamer — камера + GStreamer + свой GLib-цикл
// =============================================================================

class VideoStreamer {
   public:
    // Единственный «выход» наружу — отдать JSON клиенту.
    // std::copyable_function — C++26.
    using SendFn = std::copyable_function<void(std::string)>;

    explicit VideoStreamer(std::string device) : device_(std::move(device)) {
        // GLib-цикл нужен только GStreamer'у, поэтому живёт здесь.
        // Стартуем в конструкторе — чтобы Start()/Stop() можно было
        // переиспользовать при переподключениях без пересоздания треда.
        loop_ = g_main_loop_new(nullptr, FALSE);
        loopThread_ = std::thread([this] {
            std::println("[rtc] GLib loop running");
            g_main_loop_run(loop_);
            std::println("[rtc] GLib loop exited");
        });
    }

    ~VideoStreamer() {
        std::println("[rtc] ~VideoStreamer");
        Stop();  // сначала гасим pipeline, потом цикл
        if (loop_) {
            g_main_loop_quit(loop_);
            if (loopThread_.joinable()) loopThread_.join();
            g_main_loop_unref(loop_);
            loop_ = nullptr;
        }
    }

    VideoStreamer(const VideoStreamer&) = delete;
    VideoStreamer& operator=(const VideoStreamer&) = delete;

    std::expected<void, std::string> Start(SendFn send) {
        std::println("[rtc] === Start() === device={}", device_);

        // Один streamer = один активный клиент. Второй получит отказ,
        // пока первый не отключится (Stop() вызовется в handleConnectionClosed).
        if (pipeline_) return std::unexpected("streamer already in use by another connection");

        send_ = std::move(send);

        if (!std::filesystem::exists(device_)) return std::unexpected("device not found: " + device_);
        if (std::ifstream probe(device_); !probe)
            return std::unexpected("cannot open " + device_ + " (check permissions: 'id' contains 'video'?)");

        pipeline_ = gst_pipeline_new("webrtc-pipeline");
        if (!pipeline_) return std::unexpected("gst_pipeline_new failed");

        // Хелпер: при ошибке — освободить pipeline (он владеет всеми
        // добавленными элементами, включая webrtcbin) и обнулить поля.
        auto fail = [this](std::string msg) -> std::expected<void, std::string> {
            if (pipeline_) {
                gst_element_set_state(pipeline_, GST_STATE_NULL);
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
            }
            webrtcBin_ = nullptr;
            remoteDescriptionSet_ = false;
            return std::unexpected(std::move(msg));
        };

        // Элементы сразу кладутся в bin → pipeline ими владеет на всех
        // путях, включая early-return. Утечек нет.
        auto add = [this](const char* factory, const char* name) -> GstElement* {
            auto* e = gst_element_factory_make(factory, name);
            if (!e) {
                std::println(stderr, "[rtc] !! factory_make(\"{}\") FAILED", factory);
                return nullptr;
            }
            std::println("[rtc] + {} ('{}')", factory, name);
            gst_bin_add(GST_BIN(pipeline_), e);
            return e;
        };

        GstElement* src = add("v4l2src", "src");
        GstElement* caps = add("capsfilter", "caps");
        GstElement* parser = add("h264parse", "parser");
        GstElement* pay = add("rtph264pay", "pay");
        GstElement* queue = add("queue", "queue");
        webrtcBin_ = add("webrtcbin", "webrtcbin");

        if (!src) return fail("v4l2src missing (gstreamer1.0-plugins-good)");
        if (!caps) return fail("capsfilter missing");
        if (!parser) return fail("h264parse missing (gstreamer1.0-plugins-bad)");
        if (!pay) return fail("rtph264pay missing (gstreamer1.0-plugins-good)");
        if (!queue) return fail("queue missing");
        if (!webrtcBin_) return fail("webrtcbin missing (vcpkg gstreamer[webrtc])");

        g_object_set(src, "device", device_.c_str(), nullptr);
        GstCaps* videoCaps = gst_caps_from_string("video/x-h264");
        g_object_set(caps, "caps", videoCaps, nullptr);
        gst_caps_unref(videoCaps);
        g_object_set(pay, "config-interval", -1, nullptr);
        g_object_set(pay, "pt", 96, nullptr);

        if (!GST_IS_ELEMENT(webrtcBin_))
            return fail(
                "webrtcbin is not a valid GstElement "
                "(static registration of webrtc/nice/dtls/srtp failed)");

        if (!gst_element_link_many(src, caps, parser, pay, queue, webrtcBin_, nullptr))
            return fail(
                "link failed: v4l2src ! video/x-h264 ! h264parse ! "
                "rtph264pay ! queue ! webrtcbin (камера не отдаёт H.264?)");

        InstallBusWatch();
        ConnectWebRtcSignals();

        auto ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) return fail("set_state(PLAYING) failed");

        std::println("[rtc] state_change ret = {}", ret == GST_STATE_CHANGE_ASYNC     ? "ASYNC"
                                                    : ret == GST_STATE_CHANGE_SUCCESS ? "SUCCESS"
                                                                                      : "OTHER");
        return {};
    }

    // Идемпотентен. Не трогает GLib-цикл — он живёт всё время жизни стримера.
    void Stop() {
        if (!pipeline_) return;
        std::println("[rtc] Stop()");
        if (webrtcBin_) g_signal_handlers_disconnect_by_data(webrtcBin_, this);
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        webrtcBin_ = nullptr;
        remoteDescriptionSet_ = false;
        send_ = nullptr;
    }

    void SetRemoteDescription(std::string_view type, std::string_view sdpText) {
        if (!webrtcBin_) return;
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
        GstWebRTCSessionDescription* desc = gst_webrtc_session_description_new(sdpType, sdp);

        GstPromise* promise = gst_promise_new_with_change_func(
            [](GstPromise* p, gpointer) {
                auto r = gst_promise_wait(p);
                std::println("[rtc] set-remote-description done: {}",
                             r == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                gst_promise_unref(p);
            },
            nullptr, nullptr);
        g_signal_emit_by_name(webrtcBin_, "set-remote-description", desc, promise);
        gst_webrtc_session_description_free(desc);

        remoteDescriptionSet_ = true;
        std::println("[rtc] remote description emitted");
    }

    void AddIceCandidate(int mline, std::string_view candidate) {
        if (!webrtcBin_ || candidate.empty()) return;
        std::string value(candidate);
        g_signal_emit_by_name(webrtcBin_, "add-ice-candidate", mline, value.c_str());
    }

   private:
    void InstallBusWatch() {
        GstBus* bus = gst_element_get_bus(pipeline_);
        gst_bus_add_watch(
            bus,
            [](GstBus*, GstMessage* msg, gpointer data) -> gboolean {
                auto* pipeline = static_cast<GstElement*>(data);
                switch (GST_MESSAGE_TYPE(msg)) {
                    case GST_MESSAGE_ERROR: {
                        GError* err = nullptr;
                        gchar* dbg = nullptr;
                        gst_message_parse_error(msg, &err, &dbg);
                        std::println(stderr, "[gst] !! ERROR from {}: {}", GST_OBJECT_NAME(msg->src),
                                     err ? err->message : "?");
                        if (dbg) std::println(stderr, "[gst]    {}", dbg);
                        if (err) g_error_free(err);
                        g_free(dbg);
                        break;
                    }
                    case GST_MESSAGE_WARNING: {
                        GError* err = nullptr;
                        gchar* dbg = nullptr;
                        gst_message_parse_warning(msg, &err, &dbg);
                        std::println(stderr, "[gst] WARNING from {}: {}", GST_OBJECT_NAME(msg->src),
                                     err ? err->message : "?");
                        if (dbg) std::println(stderr, "[gst]    {}", dbg);
                        if (err) g_error_free(err);
                        g_free(dbg);
                        break;
                    }
                    case GST_MESSAGE_STATE_CHANGED:
                        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                            GstState o, n, p;
                            gst_message_parse_state_changed(msg, &o, &n, &p);
                            std::println("[gst] pipeline {} -> {} (pending {})", gst_element_state_get_name(o),
                                         gst_element_state_get_name(n), gst_element_state_get_name(p));
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
        g_signal_connect(webrtcBin_, "on-negotiation-needed", G_CALLBACK(&VideoStreamer::OnNegotiationNeeded), this);
        g_signal_connect(webrtcBin_, "on-ice-candidate", G_CALLBACK(&VideoStreamer::OnIceCandidate), this);
        g_signal_connect(webrtcBin_, "pad-added", G_CALLBACK(&VideoStreamer::OnPadAdded), this);
    }

    static void OnNegotiationNeeded(GstElement* w, gpointer data) {
        std::println("[rtc] << on-negotiation-needed >>");
        auto* promise = gst_promise_new_with_change_func(&VideoStreamer::OnOfferCreated, data, nullptr);
        g_signal_emit_by_name(w, "create-offer", nullptr, promise);
    }

    static void OnIceCandidate(GstElement*, guint mline, gchar* candidate, gpointer data) {
        auto* self = static_cast<VideoStreamer*>(data);
        if (!candidate) {
            std::println("[rtc] local ICE end-of-candidates (mline={})", mline);
            return;
        }
        if (!self->send_) return;
        std::println("[rtc] local ICE mline={} cand={}", mline, candidate);

        Json::Value m;
        m["type"] = std::format("{}", MessageType::Ice);
        m["sdpMLineIndex"] = static_cast<Json::UInt>(mline);
        m["candidate"] = std::string(candidate);
        self->send_(JsonDump(m));
    }

    static void OnPadAdded(GstElement*, GstPad* pad, gpointer) {
        std::println("[rtc] webrtcbin pad-added: {}", GST_PAD_NAME(pad));
    }

    static void OnOfferCreated(GstPromise* promise, gpointer data) {
        auto* self = static_cast<VideoStreamer*>(data);
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

        if (offer && self->webrtcBin_ && self->send_) {
            gchar* sdp = gst_sdp_message_as_text(offer->sdp);
            std::string sdpStr = sdp ? sdp : "";
            g_free(sdp);

            std::println("[rtc] sending offer ({}b)", sdpStr.size());
            Json::Value m;
            m["type"] = std::format("{}", MessageType::Offer);
            m["sdp"] = std::move(sdpStr);
            self->send_(JsonDump(m));

            GstPromise* lp = gst_promise_new_with_change_func(
                [](GstPromise* p, gpointer) {
                    auto r = gst_promise_wait(p);
                    std::println("[rtc] set-local-description done: {}",
                                 r == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                    gst_promise_unref(p);
                },
                nullptr, nullptr);
            g_signal_emit_by_name(self->webrtcBin_, "set-local-description", offer, lp);
        } else {
            std::println(stderr, "[rtc] offer is null, or pipeline already stopped");
        }
        if (offer) gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
    }

    std::string device_;
    SendFn send_;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtcBin_ = nullptr;
    bool remoteDescriptionSet_ = false;

    // GLib-цикл — только забота VideoStreamer'а.
    GMainLoop* loop_ = nullptr;
    std::thread loopThread_;
};

// =============================================================================
// Слой 2: WebRTCWebSocket — signaling
// =============================================================================
//
// Владеет ссылкой на VideoStreamer (не shared_ptr!). Экземпляр
// создаётся Drogon'ом per-connection через лямбду в main().
// Макросы WS_PATH_LIST_* намеренно не используются — они предполагают
// конструктор по умолчанию и дублировали бы /ws при явной регистрации.

class WebRTCWebSocket : public drogon::WebSocketController<WebRTCWebSocket, false> {
   public:
    explicit WebRTCWebSocket(VideoStreamer& streamer) : streamer_(streamer) {}

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr&, const drogon::WebSocketConnectionPtr& conn) override {
        std::println("[ws] === SESSION START ===");

        // weak_ptr разрывает цикл conn → send → conn.
        // queueInLoop — отправка строго из loop'а Drogon, а не из GStreamer-треда.
        std::weak_ptr<drogon::WebSocketConnection> weakConn = conn;
        auto send = [weakConn](std::string msg) {
            drogon::app().getLoop()->queueInLoop([weakConn, msg = std::move(msg)]() mutable {
                if (auto locked = weakConn.lock(); locked && locked->connected()) {
                    std::println("[ws] -> {}b: {}", msg.size(), Truncate(msg));
                    locked->send(std::move(msg));
                }
            });
        };

        if (auto r = streamer_.Start(std::move(send)); !r) {
            std::println(stderr, "[ws] !! Start FAILED: {}", r.error());
            Json::Value err;
            err["type"] = std::format("{}", MessageType::Error);
            err["message"] = r.error();
            conn->send(JsonDump(err));
            conn->shutdown();
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

        auto mt = JsonStr(msg, "type");
        if (mt == "answer") {
            auto sdp = JsonStr(msg, "sdp");
            std::println("[ws] applying answer ({}b)", sdp.size());
            streamer_.SetRemoteDescription("answer", sdp);
        } else if (mt == "ice") {
            streamer_.AddIceCandidate(JsonInt(msg, "sdpMLineIndex"), JsonStr(msg, "candidate"));
        } else if (mt == "ping") {
            Json::Value pong;
            pong["type"] = std::format("{}", MessageType::Pong);
            conn->send(JsonDump(pong));
        } else {
            std::println(stderr, "[ws] !! unknown type: {}", mt);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {
        std::println("[ws] === SESSION END ===");
        streamer_.Stop();
    }

   private:
    VideoStreamer& streamer_;
};

// =============================================================================
// main helpers
// =============================================================================

std::string SelectCamera() {
    if (const char* env = std::getenv("WEBRTC_CAMERA"); env && *env) {
        std::string cam = env;
        std::println("Using camera from WEBRTC_CAMERA: {}", cam);
        if (!std::filesystem::exists(cam)) {
            std::println(stderr, "!! {} does not exist", cam);
            return {};
        }
        if (!DeviceSupportsH264(cam)) std::println(stderr, "!! {} is not H.264-capable; passthrough will fail", cam);
        return cam;
    }

    auto cams = FindH264Cameras();
    if (cams.empty()) {
        std::println(stderr,
                     "\nNo H.264-capable camera found.\n"
                     "Passthrough needs a camera that outputs raw H.264 over V4L2.\n\n");
        PrintAllCameras();
        std::println(stderr,
                     "\nOptions:\n"
                     "  1. Connect a camera that outputs H.264.\n"
                     "  2. Force a specific device: WEBRTC_CAMERA=/dev/videoN ./webrtc\n"
                     "  3. Switch pipeline to transcode MJPEG/YUYV -> H.264.\n");
        return {};
    }
    std::println("H.264 camera(s) found:");
    for (auto& c : cams) std::println("  {}", c);
    std::println("Using: {}", cams.front());
    return cams.front();
}

std::string LoadIndexHtml() {
    for (auto* path : {"index.html", "static/index.html", "scripts/static/index.html"}) {
        if (std::filesystem::exists(path)) {
            std::ifstream f(path);
            std::string c{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            std::println("Serving index from: {} ({}b)", path, c.size());
            return c;
        }
    }
    std::println(stderr,
                 "!! index.html not found near CWD. Returning stub. "
                 "CMake should have copied it into build-dir.");
    return "<h1>index.html not found</h1>";
}

}  // namespace

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

    std::string camera = SelectCamera();
    if (camera.empty()) return 1;

    // ★ VideoStreamer — стековый, один на приложение.
    //   Контроллеры получат ссылку на него (см. лямбду ниже).
    VideoStreamer streamer(camera);

    // ★ HTML — обычная строка на стеке main. Живёт всё время работы
    //   приложения (run() блокирующий), поэтому string_view безопасен.
    const std::string html = LoadIndexHtml();
    const std::string_view htmlView = html;

    auto serveIndex = [htmlView](const drogon::HttpRequestPtr&,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeString("text/html; charset=utf-8");
        resp->setBody(std::string{htmlView});  // drogon копирует внутрь response
        cb(resp);
    };

    std::println("HTTP/WS on http://0.0.0.0:8888");

    // ★ Ручная регистрация. Inline-лямбда захватывает streamer по ссылке,
    //   внутри создаётся shared_ptr<WebRTCWebSocket> — ровно там, где это
    //   нужно Drogon'у. Никаких отдельных wsBinder-переменных.
    drogon::app()
        .addListener("0.0.0.0", 8888)
        .registerHandler("/", serveIndex)
        .registerHandler("/index.html", serveIndex)
        .registerController(std::make_shared<WebRTCWebSocket>(streamer))
        .run();

    std::println("Bye");
    return 0;
}
