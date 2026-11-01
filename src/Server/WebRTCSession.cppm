module;
#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json/json.h>

#include <expected>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>

export module WebRTCSession;

import VideoStreamer;
import MessageType;
import JsonHelpers;

export namespace webrtc_poc {

class WebRTCSession {
   public:
    using SendFn = std::copyable_function<void(std::string)>;

    WebRTCSession(VideoStreamer& shared, SendFn send) : shared_(shared), send_(std::move(send)) {}

    ~WebRTCSession() { Detach(); }
    WebRTCSession(const WebRTCSession&) = delete;
    WebRTCSession& operator=(const WebRTCSession&) = delete;

    std::expected<void, std::string> Attach() {
        if (branch_) return std::unexpected("session already attached");

        ConsumerCallbacks cb{
            .on_negotiation_needed = G_CALLBACK(&WebRTCSession::OnNegotiationNeededTrampoline),
            .on_ice_candidate = G_CALLBACK(&WebRTCSession::OnIceCandidateTrampoline),
            .on_pad_added = G_CALLBACK(&WebRTCSession::OnPadAddedTrampoline),
            .user_data = this,
        };

        auto r = shared_.AddConsumer(cb);
        if (!r) return std::unexpected(r.error());

        branch_ = std::move(*r);
        webrtcbin_ = branch_->webrtcbin;
        return {};
    }

    void Detach() {
        if (!branch_) return;
        std::println("[rtc] detaching session");
        shared_.RemoveConsumer(std::move(branch_));
        branch_.reset();
        webrtcbin_ = nullptr;
        remoteDescriptionSet_ = false;
    }

    // --- Remote SDP / ICE ---

    void SetRemoteDescription(std::string_view type, std::string_view sdpText) {
        if (!webrtcbin_) return;
        std::println("[rtc] SetRemoteDescription type={} size={}", type, sdpText.size());

        if (type == "answer" && remoteDescriptionSet_) {
            std::println(stderr, "[rtc] duplicate answer — ignoring");
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

        g_signal_emit_by_name(webrtcbin_, "set-remote-description", desc, promise);
        gst_webrtc_session_description_free(desc);
        remoteDescriptionSet_ = true;
    }

    void AddIceCandidate(int mline, std::string_view candidate) {
        if (!webrtcbin_ || candidate.empty()) return;
        std::string value(candidate);
        g_signal_emit_by_name(webrtcbin_, "add-ice-candidate", mline, value.c_str());
    }

   private:
    // --- Трамплины под C-API сигналов ---

    static void OnNegotiationNeededTrampoline(GstElement* w, gpointer data) {
        static_cast<WebRTCSession*>(data)->OnNegotiationNeeded(w);
    }
    static void OnIceCandidateTrampoline(GstElement*, guint mline, gchar* candidate, gpointer data) {
        static_cast<WebRTCSession*>(data)->OnIceCandidate(mline, candidate);
    }
    static void OnPadAddedTrampoline(GstElement*, GstPad* pad, gpointer data) {
        static_cast<WebRTCSession*>(data)->OnPadAdded(pad);
    }

    // --- Реальная логика ---

    void OnNegotiationNeeded(GstElement* w) {
        std::println("[rtc] << on-negotiation-needed >>");
        auto* promise = gst_promise_new_with_change_func(&WebRTCSession::OnOfferCreatedStatic, this, nullptr);
        g_signal_emit_by_name(w, "create-offer", nullptr, promise);
    }

    void OnIceCandidate(guint mline, gchar* candidate) {
        if (!candidate) {
            std::println("[rtc] local ICE end-of-candidates (mline={})", mline);
            return;
        }
        if (!send_) return;
        std::println("[rtc] local ICE mline={} cand={}", mline, candidate);

        Json::Value m;
        m["type"] = MessageTypeToString(MessageType::Ice);
        m["sdpMLineIndex"] = static_cast<Json::UInt>(mline);
        m["candidate"] = std::string(candidate);
        send_(JsonDump(m));
    }

    void OnPadAdded(GstPad* pad) { std::println("[rtc] webrtcbin pad-added: {}", GST_PAD_NAME(pad)); }

    static void OnOfferCreatedStatic(GstPromise* p, gpointer data) {
        static_cast<WebRTCSession*>(data)->OnOfferCreated(p);
    }

    void OnOfferCreated(GstPromise* promise) {
        std::println("[rtc] << on-offer-created >>");
        if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
            std::println(stderr, "[rtc] create-offer NOT-REPLIED");
            gst_promise_unref(promise);
            return;
        }

        const GstStructure* reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription* offer = nullptr;
        if (reply) gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

        if (offer && webrtcbin_ && send_) {
            gchar* sdp = gst_sdp_message_as_text(offer->sdp);
            std::string sdpStr = sdp ? sdp : "";
            g_free(sdp);

            std::println("[rtc] sending offer ({}b)", sdpStr.size());
            Json::Value m;
            m["type"] = MessageTypeToString(MessageType::Offer);
            m["sdp"] = std::move(sdpStr);

            // ВАЖНО: сначала send, потом set-local-description,
            // иначе дедлок на внутреннем мьютексе webrtcbin.
            send_(JsonDump(m));

            GstPromise* lp = gst_promise_new_with_change_func(
                [](GstPromise* p, gpointer) {
                    auto r = gst_promise_wait(p);
                    std::println("[rtc] set-local-description done: {}",
                                 r == GST_PROMISE_RESULT_REPLIED ? "OK" : "NOT-REPLIED");
                    gst_promise_unref(p);
                },
                nullptr, nullptr);
            g_signal_emit_by_name(webrtcbin_, "set-local-description", offer, lp);
        } else {
            std::println(stderr, "[rtc] offer is null, or session already detached");
        }

        if (offer) gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
    }

    VideoStreamer& shared_;
    SendFn send_;
    std::shared_ptr<ConsumerBranch> branch_;
    GstElement* webrtcbin_ = nullptr;
    bool remoteDescriptionSet_ = false;
};

}  // namespace webrtc_poc
