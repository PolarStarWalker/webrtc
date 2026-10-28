// Заголовок WebRTC-API из gst-plugins-bad помечен как unstable; define
// должен идти до любых gst/*.h включений, иначе -Werror=cpp сработает.
#define GST_USE_UNSTABLE_API

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// =============================================================================
// Сообщения
// =============================================================================

enum class MsgType : std::uint8_t { Offer, Answer, Ice, Error, Ping, Pong };

constexpr std::string_view to_string(MsgType t) {
    switch (t) {
        case MsgType::Offer:
            return "offer";
        case MsgType::Answer:
            return "answer";
        case MsgType::Ice:
            return "ice";
        case MsgType::Error:
            return "error";
        case MsgType::Ping:
            return "ping";
        case MsgType::Pong:
            return "pong";
    }
    return "unknown";
}

static std::string trunc(std::string_view s, size_t n = 120) {
    if (s.size() <= n) return std::string(s);
    return std::string(s.substr(0, n)) + "...";
}

// =============================================================================
// Камеры
// =============================================================================

// Открывает устройство и проверяет, что это capture-устройство
// с поддержкой V4L2_PIX_FMT_H264.
static bool device_supports_h264(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ::close(fd);
        return false;
    }
    // На SBC /dev/videoN бывают ISP/codec без V4L2_CAP_VIDEO_CAPTURE.
    auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        ::close(fd);
        return false;
    }

    bool has_h264 = false;
    v4l2_fmtdesc fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmt.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
        if (fmt.pixelformat == V4L2_PIX_FMT_H264) {
            has_h264 = true;
            break;
        }
    }
    ::close(fd);
    return has_h264;
}

static std::vector<std::string> find_h264_cameras() {
    std::vector<std::string> cams;
    for (int i = 0; i < 64; ++i) {
        auto p = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(p)) continue;
        if (device_supports_h264(p)) {
            std::println("[cam] {}: H.264 ✓", p);
            cams.push_back(p);
        }
    }
    return cams;
}

// Для диагностики: перечислить все /dev/video* и их форматы.
static void print_all_cameras() {
    std::println(stderr, "Scanning /dev/video*:");
    bool any = false;
    for (int i = 0; i < 64; ++i) {
        auto p = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(p)) continue;
        any = true;

        int fd = ::open(p.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::println(stderr, "  {}: cannot open ({})", p, std::strerror(errno));
            continue;
        }

        v4l2_capability cap{};
        std::string name = "<unknown>";
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) name = reinterpret_cast<const char*>(cap.card);

        std::string fmts;
        v4l2_fmtdesc fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fmt.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
            if (!fmts.empty()) fmts += ", ";
            fmts += reinterpret_cast<const char*>(&fmt.description);
        }
        ::close(fd);

        std::println(stderr, "  {} [{}]: {}", p, name, fmts.empty() ? "<no capture formats>" : fmts);
    }
    if (!any) std::println(stderr, "  (no /dev/video* devices found)");
}

// =============================================================================
// WebRTCSession
// =============================================================================

class WebRTCSession {
   public:
    using SendFn = std::function<void(std::string)>;

    WebRTCSession(std::string device, SendFn send) : device_(std::move(device)), send_(std::move(send)) {}

    ~WebRTCSession() {
        std::println("[rtc] ~WebRTCSession");
        if (webrtcbin_) g_signal_handlers_disconnect_by_data(webrtcbin_, this);
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    WebRTCSession(const WebRTCSession&) = delete;
    WebRTCSession& operator=(const WebRTCSession&) = delete;

    std::expected<void, std::string> start() {
        std::println("[rtc] === start() ===");
        std::println("[rtc] device: {}", device_);

        if (!std::filesystem::exists(device_)) return std::unexpected("device not found: " + device_);
        if (std::ifstream probe(device_); !probe)
            return std::unexpected("cannot open " + device_ + " (check permissions: 'id' contains 'video'?)");

        pipeline_ = gst_pipeline_new("webrtc-pipeline");
        if (!pipeline_) return std::unexpected("gst_pipeline_new failed");

        auto mk = [](const char* f, const char* n) -> GstElement* {
            auto* el = gst_element_factory_make(f, n);
            if (!el)
                std::println(stderr, "[rtc] !! factory_make(\"{}\") FAILED", f);
            else
                std::println("[rtc] + {} ('{}')", f, n);
            return el;
        };

        GstElement* src = mk("v4l2src", "src");
        GstElement* caps = mk("capsfilter", "caps");
        GstElement* parser = mk("h264parse", "parser");
        GstElement* pay = mk("rtph264pay", "pay");
        GstElement* queue = mk("queue", "queue");
        webrtcbin_ = mk("webrtcbin", "webrtcbin");

        if (!src) return std::unexpected("v4l2src missing (gstreamer1.0-plugins-good)");
        if (!caps) return std::unexpected("capsfilter missing");
        if (!parser) return std::unexpected("h264parse missing (gstreamer1.0-plugins-bad)");
        if (!pay) return std::unexpected("rtph264pay missing (gstreamer1.0-plugins-good)");
        if (!queue) return std::unexpected("queue missing");
        if (!webrtcbin_) return std::unexpected("webrtcbin missing (vcpkg gstreamer[webrtc])");

        g_object_set(src, "device", device_.c_str(), nullptr);
        GstCaps* c = gst_caps_from_string("video/x-h264");
        g_object_set(caps, "caps", c, nullptr);
        gst_caps_unref(c);
        g_object_set(pay, "config-interval", -1, nullptr);
        g_object_set(pay, "pt", 96, nullptr);

        if (!GST_IS_ELEMENT(webrtcbin_)) {
            return std::unexpected(
                "webrtcbin is not a valid GstElement "
                "(static registration of webrtc/nice/dtls/srtp failed)");
        }
        gst_bin_add_many(GST_BIN(pipeline_), src, caps, parser, pay, queue, webrtcbin_, nullptr);

        if (!gst_element_link_many(src, caps, parser, pay, queue, webrtcbin_, nullptr))
            return std::unexpected(
                "link failed: v4l2src ! video/x-h264 ! h264parse ! "
                "rtph264pay ! queue ! webrtcbin  (камера не отдаёт H.264?)");

        install_bus_watch();
        connect_webrtc_signals();

        std::println("[rtc] set_state(PLAYING)");
        auto ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) return std::unexpected("set_state(PLAYING) failed");

        std::println("[rtc] state_change ret = {}", ret == GST_STATE_CHANGE_ASYNC     ? "ASYNC"
                                                    : ret == GST_STATE_CHANGE_SUCCESS ? "SUCCESS"
                                                                                      : "OTHER");
        return {};
    }

    void set_remote_description(std::string_view type, std::string_view sdp_text) {
        std::println("[rtc] set_remote_description type={} size={}", type, sdp_text.size());

        if (type == "answer" && remote_desc_set_) {
            std::println(stderr, "[rtc] duplicate answer, ignoring");
            return;
        }

        std::string sdp_copy(sdp_text);
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(sdp_copy.c_str(), &sdp) != GST_SDP_OK) {
            std::println(stderr, "[rtc] SDP parse failed ({}b)", sdp_copy.size());
            return;
        }

        auto sdp_type = (type == "offer") ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER;

        GstWebRTCSessionDescription* desc = gst_webrtc_session_description_new(sdp_type, sdp);

        GstPromise* promise = gst_promise_new_with_change_func(
            [](GstPromise* p, gpointer) {
                auto r = gst_promise_wait(p);
                std::println("[rtc] set-remote-description done: {}",
                             r == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                gst_promise_unref(p);
            },
            nullptr, nullptr);
        g_signal_emit_by_name(webrtcbin_, "set-remote-description", desc, promise);
        gst_webrtc_session_description_free(desc);

        remote_desc_set_ = true;
        std::println("[rtc] remote description emitted");
    }

    void add_ice_candidate(int mline, std::string_view candidate) {
        if (candidate.empty()) return;
        std::string c(candidate);
        g_signal_emit_by_name(webrtcbin_, "add-ice-candidate", mline, c.c_str());
    }

   private:
    void install_bus_watch() {
        GstBus* bus = gst_element_get_bus(pipeline_);
        gst_bus_add_watch(
            bus,
            [](GstBus*, GstMessage* msg, gpointer data) -> gboolean {
                GstElement* p = static_cast<GstElement*>(data);
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
                        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(p)) {
                            GstState old_s, new_s, pending;
                            gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                            std::println("[gst] pipeline {} -> {} (pending {})", gst_element_state_get_name(old_s),
                                         gst_element_state_get_name(new_s), gst_element_state_get_name(pending));
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

    void connect_webrtc_signals() {
        g_signal_connect(webrtcbin_, "on-negotiation-needed", G_CALLBACK(&WebRTCSession::on_negotiation_needed), this);
        g_signal_connect(webrtcbin_, "on-ice-candidate", G_CALLBACK(&WebRTCSession::on_ice_candidate), this);
        g_signal_connect(webrtcbin_, "pad-added", G_CALLBACK(&WebRTCSession::on_pad_added), this);
    }

    static void on_negotiation_needed(GstElement* w, gpointer data) {
        std::println("[rtc] << on-negotiation-needed >>");
        GstPromise* promise = gst_promise_new_with_change_func(&WebRTCSession::on_offer_created,
                                                               static_cast<WebRTCSession*>(data), nullptr);
        g_signal_emit_by_name(w, "create-offer", nullptr, promise);
    }

    static void on_ice_candidate(GstElement*, guint mline, gchar* candidate, gpointer data) {
        auto* self = static_cast<WebRTCSession*>(data);
        if (!candidate) {
            std::println("[rtc] local ICE end-of-candidates (mline={})", mline);
            return;
        }
        std::println("[rtc] local ICE mline={} cand={}", mline, candidate);
        json msg = {
            {         "type", to_string(MsgType::Ice)},
            {"sdpMLineIndex",                   mline},
            {    "candidate",  std::string(candidate)}
        };
        self->send_(msg.dump());
    }

    static void on_pad_added(GstElement*, GstPad* pad, gpointer) {
        std::println("[rtc] webrtcbin pad-added: {}", GST_PAD_NAME(pad));
    }

    static void on_offer_created(GstPromise* promise, gpointer data) {
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
            std::string sdp_str = sdp ? sdp : "";
            g_free(sdp);

            std::println("[rtc] sending offer ({}b)", sdp_str.size());
            json msg = {
                {"type", to_string(MsgType::Offer)},
                { "sdp",        std::move(sdp_str)}
            };
            self->send_(msg.dump());

            GstPromise* lp = gst_promise_new_with_change_func(
                [](GstPromise* p, gpointer) {
                    auto r = gst_promise_wait(p);
                    std::println("[rtc] set-local-description done: {}",
                                 r == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                    gst_promise_unref(p);
                },
                nullptr, nullptr);
            g_signal_emit_by_name(self->webrtcbin_, "set-local-description", offer, lp);
            gst_webrtc_session_description_free(offer);
        } else {
            std::println(stderr, "[rtc] offer is null in reply");
        }

        gst_promise_unref(promise);
    }

    std::string device_;
    SendFn send_;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtcbin_ = nullptr;
    bool remote_desc_set_ = false;
};

// =============================================================================
// WebSocketSession
// =============================================================================

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
   public:
    WebSocketSession(websocket::stream<beast::tcp_stream> ws, std::string device)
        : ws_(std::move(ws)), device_(std::move(device)), executor_(ws_.get_executor()) {
        ws_.text(true);
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    }

    net::awaitable<void> run() {
        std::println("[ws] === SESSION START ===");
        try {
            rtc_ = std::make_unique<WebRTCSession>(device_, [this](std::string m) { enqueue(std::move(m)); });

            if (auto r = rtc_->start(); !r) {
                std::println(stderr, "[ws] !! start FAILED: {}", r.error());
                json err = {
                    {   "type", to_string(MsgType::Error)},
                    {"message",                 r.error()}
                };
                boost::system::error_code ec;
                co_await ws_.async_write(net::buffer(err.dump()), net::redirect_error(net::use_awaitable, ec));
                ws_.close(websocket::close_code::internal_error, ec);
                co_return;
            }
            std::println("[ws] pipeline started; waiting for answer/ice");

            co_await read_loop();

            boost::system::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
            std::println("[ws] === SESSION END ===");
        } catch (std::exception& e) {
            std::println(stderr, "[ws] !! exception: {}", e.what());
        }
    }

   private:
    net::awaitable<void> read_loop() {
        beast::flat_buffer buf;
        for (;;) {
            boost::system::error_code ec;
            co_await ws_.async_read(buf, net::redirect_error(net::use_awaitable, ec));
            if (ec) {
                std::println("[ws] read: {}", ec.message());
                co_return;
            }

            std::string data = beast::buffers_to_string(buf.data());
            buf.consume(buf.size());
            std::println("[ws] <- {}b: {}", data.size(), trunc(data));

            auto msg = json::parse(data, nullptr, false);
            if (msg.is_discarded()) {
                std::println(stderr, "[ws] !! bad JSON");
                continue;
            }
            co_await dispatch(msg);
        }
    }

    net::awaitable<void> dispatch(const json& msg) {
        auto type = msg.value("type", "");
        if (type == "answer") {
            std::println("[ws] applying answer ({}b)", msg.value("sdp", "").size());
            rtc_->set_remote_description("answer", msg.value("sdp", ""));
        } else if (type == "ice") {
            rtc_->add_ice_candidate(msg.value("sdpMLineIndex", 0), msg.value("candidate", ""));
        } else if (type == "ping") {
            json pong = {
                {"type", to_string(MsgType::Pong)}
            };
            co_await ws_.async_write(net::buffer(pong.dump()), net::use_awaitable);
        } else {
            std::println(stderr, "[ws] !! unknown type: {}", type);
        }
    }

    void enqueue(std::string msg) {
        std::println("[ws] -> {}b: {}", msg.size(), trunc(msg));
        auto self = shared_from_this();
        net::post(executor_, [self, m = std::move(msg)]() mutable {
            self->queue_.push_back(std::move(m));
            self->try_send();
        });
    }

    void try_send() {
        if (sending_ || queue_.empty()) return;
        sending_ = true;
        ws_.async_write(net::buffer(queue_.front()),
                        [self = shared_from_this()](boost::system::error_code ec, size_t n) {
                            self->sending_ = false;
                            if (ec) {
                                std::println(stderr, "[ws] write: {}", ec.message());
                                return;
                            }
                            std::println("[ws] wrote {}b", n);
                            self->queue_.pop_front();
                            self->try_send();
                        });
    }

    websocket::stream<beast::tcp_stream> ws_;
    std::string device_;
    net::any_io_executor executor_;
    std::unique_ptr<WebRTCSession> rtc_;
    std::deque<std::string> queue_;
    bool sending_ = false;
};

// =============================================================================
// HTTP / WS handler
// =============================================================================

net::awaitable<void> handle_http(tcp::socket sock, std::string camera, std::string index_html) {
    beast::tcp_stream stream(std::move(sock));
    beast::flat_buffer buf;
    http::request<http::string_body> req;

    boost::system::error_code ec;
    co_await http::async_read(stream, buf, req, net::redirect_error(net::use_awaitable, ec));
    if (ec) co_return;

    std::println("[http] {} {}", req.method_string(), req.target());

    if (req.target() == "/ws" && websocket::is_upgrade(req)) {
        websocket::stream<beast::tcp_stream> ws(std::move(stream));
        co_await ws.async_accept(req, net::use_awaitable);
        auto session = std::make_shared<WebSocketSession>(std::move(ws), camera);
        co_await session->run();
        co_return;
    }

    http::response<http::string_body> res;
    if (req.target() == "/" || req.target() == "/index.html") {
        res = {http::status::ok, req.version()};
        res.set(http::field::content_type, "text/html; charset=utf-8");
        res.body() = std::move(index_html);
        std::println("[http] serving index ({}b)", res.body().size());
    } else {
        res = {http::status::not_found, req.version()};
        res.body() = "Not Found";
    }
    res.keep_alive(false);
    res.prepare_payload();

    co_await http::async_write(stream, res, net::redirect_error(net::use_awaitable, ec));
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
}

net::awaitable<void> listener(tcp::acceptor acc, std::string camera, std::string index_html) {
    for (;;) {
        boost::system::error_code ec;
        tcp::socket sock = co_await acc.async_accept(net::redirect_error(net::use_awaitable, ec));
        if (ec) continue;
        net::co_spawn(acc.get_executor(), handle_http(std::move(sock), camera, index_html), net::detached);
    }
}

// =============================================================================
// main helpers
// =============================================================================

// GLib main loop должен крутиться в отдельном потоке: webrtcbin эмитит
// on-negotiation-needed через g_idle_add, и без GMainLoop сигнал не сработает.
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

// Выбор камеры: env override -> auto-detect по V4L2.
// Возвращает пустую строку, если ничего не выбрано (уже напечатав причину).
static std::string select_camera() {
    if (const char* env = std::getenv("WEBRTC_CAMERA"); env && *env) {
        std::string cam = env;
        std::println("Using camera from WEBRTC_CAMERA: {}", cam);
        if (!std::filesystem::exists(cam)) {
            std::println(stderr, "!! {} does not exist", cam);
            return {};
        }
        if (!device_supports_h264(cam)) {
            std::println(stderr, "!! {} is not H.264-capable; passthrough pipeline will fail", cam);
        }
        return cam;
    }

    auto cams = find_h264_cameras();
    if (cams.empty()) {
        std::println(stderr,
                     "\nNo H.264-capable camera found.\n"
                     "Passthrough needs a camera that outputs raw H.264 over V4L2.\n\n");
        print_all_cameras();
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
    for (auto& c : cams) std::println("  {}", c);
    std::println("Using: {}", cams.front());
    return cams.front();
}

// index.html ищется по относительным путям от CWD — CMake копирует его
// в build-dir через file(COPY ...) в корневом CMakeLists.txt.
static std::string load_index_html() {
    for (auto* p : {"index.html", "static/index.html", "scripts/static/index.html"}) {
        if (std::filesystem::exists(p)) {
            std::ifstream f(p);
            std::string s{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            std::println("Serving index from: {} ({}b)", p, s.size());
            return s;
        }
    }
    std::println(stderr,
                 "!! index.html not found near CWD. Returning stub. "
                 "CMake should have copied it into build-dir.");
    return "<h1>index.html not found</h1>";
}

// Создаёт acceptor и запускает listener-корутину.
static std::optional<tcp::acceptor> setup_acceptor(net::io_context& ioc, std::uint16_t port) {
    try {
        return tcp::acceptor{
            ioc, {tcp::v4(), port}
        };
    } catch (std::exception& e) {
        std::println(stderr, "!! cannot bind port {}: {}", port, e.what());
        return std::nullopt;
    }
}

// Главный цикл asio + graceful shutdown по SIGINT/SIGTERM.
static void run_event_loop(net::io_context& ioc) {
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
        std::println("\nShutting down...");
        ioc.stop();
    });
    ioc.run();
}

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

    GLibLoop glib;  // запускает GMainLoop в отдельном потоке

    std::string camera = select_camera();
    if (camera.empty()) return 1;

    std::string index_html = load_index_html();

    net::io_context ioc{1};

    auto acceptor = setup_acceptor(ioc, 8888);
    if (!acceptor) return 1;

    std::println("HTTP/WS on http://0.0.0.0:8888");
    net::co_spawn(ioc, listener(std::move(*acceptor), std::move(camera), std::move(index_html)), net::detached);

    run_event_loop(ioc);

    std::println("Bye");
    return 0;
}
