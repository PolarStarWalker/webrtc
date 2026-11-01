module;

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <format>
#include <memory>
#include <print>
#include <string>

export module WebRTCWebSocket;

import VideoStreamer;
import JsonHelpers;
import MessageType;

export namespace webrtc_poc {

class WebRTCWebSocket : public drogon::WebSocketController<WebRTCWebSocket, false> {
   public:
    explicit WebRTCWebSocket(VideoStreamer& streamer) : streamer_(streamer) {}

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr&, const drogon::WebSocketConnectionPtr& conn) override {
        std::println("[ws] === SESSION START ===");

        const std::weak_ptr weakConn = conn;
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
            err["type"] = MessageTypeToString(MessageType::Error);
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
            pong["type"] = MessageTypeToString(MessageType::Pong);
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

}  // namespace webrtc_poc
