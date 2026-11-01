module;

#define GST_USE_UNSTABLE_API  // ОБЯЗАТЕЛЬНО до любых gst/*.h

#include <nlohmann/json.hpp>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

export module webrtc_stream;

using nlohmann::json;

// ─────────────────────────────────────────────────────────────────
//  Публичный интерфейс
// ─────────────────────────────────────────────────────────────────
export class WebRTCVideoStream {
   public:
    struct FrameView {
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        bool valid() const noexcept { return texture != 0 && width > 0 && height > 0; }
    };

    WebRTCVideoStream(std::string signaling_uri, GLFWwindow* share_window);
    ~WebRTCVideoStream();

    WebRTCVideoStream(const WebRTCVideoStream&) = delete;
    WebRTCVideoStream& operator=(const WebRTCVideoStream&) = delete;

    void start();
    void stop();

    std::mutex& render_mutex();
    FrameView current_frame();
    std::string last_error() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────
//  Реализация (не видна клиенту)
// ─────────────────────────────────────────────────────────────────
struct WebRTCVideoStream::Impl {
    std::string uri;
    GLFWwindow* share_window = nullptr;
    GLFWwindow* gl_window = nullptr;

    // GStreamer
    GstElement* pipeline = nullptr;
    GstElement* webrtc = nullptr;
    GstElement* appsink = nullptr;
    GMainLoop* gst_loop = nullptr;
    GMainContext* gst_context = nullptr;

    // Потоки
    std::thread signaling_thread;
    std::thread video_thread;

    // WebSocket
    ix::WebSocket ws;

    // Общая текстура
    std::mutex frame_mutex;
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    bool has_frame = false;

    // Статус
    std::atomic<bool> running{false};
    mutable std::mutex err_mutex;
    std::string last_error_;

    // ── helpers ────────────────────────────────────────────────
    void set_error(std::string msg) {
        {
            std::lock_guard lk(err_mutex);
            last_error_ = std::move(msg);
        }
        std::fprintf(stderr, "[stream] ERROR: %s\n", last_error_.c_str());
    }

    // ── GStreamer callbacks ────────────────────────────────────
    static void on_pad_added(GstElement* /*webrtc*/, GstPad* pad, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!GST_PAD_IS_SRC(pad)) return;

        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (!caps) return;

        const GstStructure* s = gst_caps_get_structure(caps, 0);
        const gchar* enc = s ? gst_structure_get_string(s, "encoding-name") : nullptr;
        if (!enc || std::strcmp(enc, "H264") != 0) {
            std::fprintf(stderr, "[stream] ignoring pad with codec=%s\n", enc ? enc : "?");
            gst_caps_unref(caps);
            return;
        }
        gst_caps_unref(caps);

        GstElement* depay = gst_bin_get_by_name(GST_BIN(self->pipeline), "depay");
        if (!depay) {
            self->set_error("no depay element");
            return;
        }
        GstPad* sinkpad = gst_element_get_static_pad(depay, "sink");
        if (sinkpad && !gst_pad_is_linked(sinkpad)) {
            GstPadLinkReturn lr = gst_pad_link(pad, sinkpad);
            if (lr != GST_PAD_LINK_OK)
                std::fprintf(stderr, "[stream] pad_link failed: %d\n", lr);
            else
                std::fprintf(stderr, "[stream] webrtcbin -> depay linked\n");
        }
        if (sinkpad) gst_object_unref(sinkpad);
        gst_object_unref(depay);
    }

    static void on_ice_candidate(GstElement* /*webrtc*/, guint mline, gchar* candidate, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!candidate || !*candidate) return;
        json j;
        j["type"] = "ice";
        j["sdpMLineIndex"] = mline;
        j["candidate"] = candidate;
        self->ws.send(j.dump());
    }

    static gboolean on_bus_message(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                self->set_error(std::string("GStreamer: ") + (err ? err->message : "?"));
                if (dbg) g_free(dbg);
                if (err) g_error_free(err);
                self->running = false;
                break;
            }
            case GST_MESSAGE_EOS:
                self->set_error("GStreamer EOS");
                self->running = false;
                break;
            case GST_MESSAGE_WARNING: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_warning(msg, &err, &dbg);
                std::fprintf(stderr, "[stream] WARNING: %s\n", err ? err->message : "?");
                if (dbg) g_free(dbg);
                if (err) g_error_free(err);
                break;
            }
            default:
                break;
        }
        return G_SOURCE_CONTINUE;
    }

    // ── WebSocket handlers (ixwebsocket thread) ────────────────
    void handle_ws_message(const std::string& raw) {
        json msg;
        try {
            msg = json::parse(raw);
        } catch (...) {
            return;
        }

        std::string type = msg.value("type", "");
        if (type == "offer") {
            handle_offer(msg.value("sdp", ""));
        } else if (type == "answer") {
            handle_answer(msg.value("sdp", ""));
        } else if (type == "ice") {
            int idx = msg.value("sdpMLineIndex", 0);
            std::string cand = msg.value("candidate", "");
            if (!cand.empty()) g_signal_emit_by_name(webrtc, "add-ice-candidate", idx, cand.c_str());
        } else if (type == "error") {
            set_error("server: " + msg.value("message", ""));
        }
    }

    void handle_offer(const std::string& sdp_text) {
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(sdp_text.c_str(), &sdp) != GST_SDP_OK) {
            set_error("failed to parse SDP offer");
            return;
        }
        GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

        GstPromise* p = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", offer, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gst_webrtc_session_description_free(offer);
        std::fprintf(stderr, "[stream] remote offer set\n");

        GstPromise* p2 = gst_promise_new();
        g_signal_emit_by_name(webrtc, "create-answer", nullptr, p2);
        gst_promise_wait(p2);

        const GstStructure* reply = gst_promise_get_reply(p2);
        if (!reply) {
            gst_promise_unref(p2);
            set_error("create-answer returned empty promise");
            return;
        }
        GstWebRTCSessionDescription* answer = nullptr;
        gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
        gst_promise_unref(p2);

        if (!answer) {
            set_error("create-answer returned null description");
            return;
        }

        GstPromise* p3 = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-local-description", answer, p3);
        gst_promise_wait(p3);
        gst_promise_unref(p3);

        gchar* sdp_out = gst_sdp_message_as_text(answer->sdp);
        json j;
        j["type"] = "answer";
        j["sdp"] = sdp_out ? sdp_out : "";
        if (sdp_out) g_free(sdp_out);
        ws.send(j.dump());

        gst_webrtc_session_description_free(answer);
        std::fprintf(stderr, "[stream] answer sent\n");
    }

    void handle_answer(const std::string& sdp_text) {
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(sdp_text.c_str(), &sdp) != GST_SDP_OK) return;
        GstWebRTCSessionDescription* ans = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        GstPromise* p = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", ans, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gst_webrtc_session_description_free(ans);
    }

    // ── Потоки ────────────────────────────────────────────────
    void signaling_main() {
        g_main_context_push_thread_default(gst_context);
        std::fprintf(stderr, "[stream] signaling thread: GLib main loop running\n");
        g_main_loop_run(gst_loop);
        std::fprintf(stderr, "[stream] signaling thread: GLib main loop stopped\n");
        g_main_context_pop_thread_default(gst_context);
    }

    void video_main() {
        glfwMakeContextCurrent(gl_window);

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::fprintf(stderr, "[stream] video thread: texture=%u created\n", texture);

        constexpr GstClockTime kTimeout = 100 * GST_MSECOND;

        while (running) {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), kTimeout);
            if (!sample) continue;

            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstCaps* caps = gst_sample_get_caps(sample);

            GstVideoInfo info;
            if (!caps || !gst_video_info_from_caps(&info, caps)) {
                gst_sample_unref(sample);
                continue;
            }
            const int w = GST_VIDEO_INFO_WIDTH(&info);
            const int h = GST_VIDEO_INFO_HEIGHT(&info);
            if (w <= 0 || h <= 0) {
                gst_sample_unref(sample);
                continue;
            }

            GstMapInfo map;
            if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
                gst_sample_unref(sample);
                continue;
            }

            {
                std::lock_guard lk(frame_mutex);
                glBindTexture(GL_TEXTURE_2D, texture);
                if (w != width || h != height) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, map.data);
                    width = w;
                    height = h;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, map.data);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                has_frame = true;
            }

            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
        }

        glDeleteTextures(1, &texture);
        texture = 0;
        glfwMakeContextCurrent(nullptr);
        std::fprintf(stderr, "[stream] video thread: stopped\n");
    }

    // ── Setup / Teardown ──────────────────────────────────────
    void build_pipeline() {
        pipeline = gst_pipeline_new("webrtc-client");

        webrtc = gst_element_factory_make("webrtcbin", "recv");
        GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
        GstElement* parse = gst_element_factory_make("h264parse", nullptr);
        GstElement* decode = gst_element_factory_make("avdec_h264", nullptr);
        GstElement* conv = gst_element_factory_make("videoconvert", nullptr);
        appsink = gst_element_factory_make("appsink", "sink");

        if (!webrtc || !depay || !parse || !decode || !conv || !appsink)
            throw std::runtime_error(
                "missing GStreamer plugin "
                "(install gstreamer1.0-plugins-{good,bad,libav})");

        // ВАЖНО: bundle-policy — это enum, а не строка. Передавать "max-bundle"
        // в g_object_set() нельзя: g_object_set() — variadic, GObject не умеет
        // конвертировать строку в enum через этот путь, и в property уезжает
        // мусорное значение.
        g_object_set(webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "stun-server",
                     "stun://stun.l.google.com:19302", nullptr);

        // pre-declare трансивера для recvonly клиента не требуется: webrtcbin
        // создаст его сам из оффера сервера (a=sendonly → мы recvonly).
        // Если понадобится явно, современный API (GStreamer >= 1.22):
        //
        //   GstCaps* rtp_caps = gst_caps_from_string(
        //       "application/x-rtp,media=video,encoding-name=H264,payload=96");
        //   GstWebRTCRTPTransceiver* tr = nullptr;
        //   g_signal_emit_by_name(webrtc, "add-transceiver",
        //                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY,
        //                         rtp_caps, &tr);
        //   if (tr) gst_object_unref(tr);
        //   gst_caps_unref(rtp_caps);
        //
        // Старый promise-based вариант на GStreamer 1.28.x оставляет promise
        // в состоянии PENDING — отсюда и warning из gst_promise_free.

        g_object_set(appsink, "emit-signals", FALSE, "max-buffers", 2, "drop", TRUE, "sync", FALSE, nullptr);
        GstCaps* raw_caps = gst_caps_from_string("video/x-raw,format=RGBA");
        g_object_set(appsink, "caps", raw_caps, nullptr);
        gst_caps_unref(raw_caps);

        gst_bin_add_many(GST_BIN(pipeline), webrtc, depay, parse, decode, conv, appsink, nullptr);
        if (!gst_element_link_many(depay, parse, decode, conv, appsink, nullptr))
            throw std::runtime_error("failed to link decode chain");

        g_signal_connect(webrtc, "pad-added", G_CALLBACK(&Impl::on_pad_added), this);
        g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(&Impl::on_ice_candidate), this);

        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_add_watch(bus, &Impl::on_bus_message, this);
        gst_object_unref(bus);
    }

    void create_shared_gl_window() {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        gl_window = glfwCreateWindow(1, 1, "stream-ctx", nullptr, share_window);

        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

        if (!gl_window) throw std::runtime_error("failed to create shared GL context");

        glfwMakeContextCurrent(share_window);
    }
};

// ─────────────────────────────────────────────────────────────────
//  Публичные методы
// ─────────────────────────────────────────────────────────────────
WebRTCVideoStream::WebRTCVideoStream(std::string uri, GLFWwindow* share_window) : impl_(std::make_unique<Impl>()) {
    impl_->uri = std::move(uri);
    impl_->share_window = share_window;

    gst_init(nullptr, nullptr);

    impl_->gst_context = g_main_context_new();
    impl_->gst_loop = g_main_loop_new(impl_->gst_context, FALSE);

    impl_->create_shared_gl_window();
    impl_->build_pipeline();

    impl_->ws.setUrl(impl_->uri);
    impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        using T = ix::WebSocketMessageType;
        if (msg->type == T::Message) {
            impl_->handle_ws_message(msg->str);
        } else if (msg->type == T::Open) {
            std::fprintf(stderr, "[stream] ws connected: %s\n", impl_->uri.c_str());
        } else if (msg->type == T::Close) {
            std::fprintf(stderr, "[stream] ws closed\n");
        } else if (msg->type == T::Error) {
            impl_->set_error("ws: " + msg->errorInfo.reason);
        }
    });
}

WebRTCVideoStream::~WebRTCVideoStream() { stop(); }

void WebRTCVideoStream::start() {
    if (impl_->running.exchange(true)) return;

    impl_->signaling_thread = std::thread(&Impl::signaling_main, impl_.get());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    GstStateChangeReturn r = gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        impl_->set_error("pipeline failed to PLAY");
        impl_->running = false;
        // Аккуратно прибираем за собой, чтобы не остался висящий поток.
        if (impl_->gst_loop) g_main_loop_quit(impl_->gst_loop);
        if (impl_->signaling_thread.joinable()) impl_->signaling_thread.join();
        return;
    }

    impl_->video_thread = std::thread(&Impl::video_main, impl_.get());
    impl_->ws.start();
}

void WebRTCVideoStream::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;

    impl_->ws.stop();

    if (impl_->pipeline) gst_element_set_state(impl_->pipeline, GST_STATE_NULL);

    if (impl_->gst_loop) g_main_loop_quit(impl_->gst_loop);

    if (impl_->video_thread.joinable()) impl_->video_thread.join();
    if (impl_->signaling_thread.joinable()) impl_->signaling_thread.join();

    if (impl_->pipeline) {
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
    }
    if (impl_->gst_loop) {
        g_main_loop_unref(impl_->gst_loop);
        impl_->gst_loop = nullptr;
    }
    if (impl_->gst_context) {
        g_main_context_unref(impl_->gst_context);
        impl_->gst_context = nullptr;
    }
    if (impl_->gl_window) {
        glfwDestroyWindow(impl_->gl_window);
        impl_->gl_window = nullptr;
    }
}

std::mutex& WebRTCVideoStream::render_mutex() { return impl_->frame_mutex; }

WebRTCVideoStream::FrameView WebRTCVideoStream::current_frame() {
    FrameView v;
    if (impl_->has_frame) {
        v.texture = impl_->texture;
        v.width = impl_->width;
        v.height = impl_->height;
    }
    return v;
}

std::string WebRTCVideoStream::last_error() const {
    if (!impl_) return {};
    std::lock_guard lk(impl_->err_mutex);
    return impl_->last_error_;
}
