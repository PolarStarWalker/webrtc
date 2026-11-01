module;

#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json/json.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

export module VideoStreamer;

import MessageType;
import JsonHelpers;

export namespace webrtc_poc {

class VideoStreamer {
   public:
    using SendFn = std::copyable_function<void(std::string)>;

    explicit VideoStreamer(std::string device) : device_(std::move(device)) {
        loop_ = g_main_loop_new(nullptr, FALSE);
        loopThread_ = std::thread([this] {
            std::println("[rtc] GLib loop running");
            g_main_loop_run(loop_);
            std::println("[rtc] GLib loop exited");
        });
    }

    ~VideoStreamer() {
        std::println("[rtc] ~VideoStreamer");
        Stop();
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

        if (pipeline_) return std::unexpected("streamer already in use by another connection");

        send_ = std::move(send);

        if (!std::filesystem::exists(device_)) return std::unexpected("device not found: " + device_);
        if (std::ifstream probe(device_); !probe)
            return std::unexpected("cannot open " + device_ + " (check permissions: 'id' contains 'video'?)");

        pipeline_ = gst_pipeline_new("webrtc-pipeline");
        if (!pipeline_) return std::unexpected("gst_pipeline_new failed");

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
        m["type"] = MessageTypeToString(MessageType::Ice);
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
        if (reply) gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

        if (offer && self->webrtcBin_ && self->send_) {
            gchar* sdp = gst_sdp_message_as_text(offer->sdp);
            std::string sdpStr = sdp ? sdp : "";
            g_free(sdp);

            std::println("[rtc] sending offer ({}b)", sdpStr.size());
            Json::Value m;
            m["type"] = MessageTypeToString(MessageType::Offer);
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

    GMainLoop* loop_ = nullptr;
    std::thread loopThread_;
};

}  // namespace webrtc_poc
