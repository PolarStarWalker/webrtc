module;
#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <expected>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <unordered_map>

export module WebRTCWebSocket;

import VideoStreamer;
import WebRTCSession;
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

        auto session = std::make_shared<WebRTCSession>(streamer_, std::move(send));
        if (auto r = session->Attach(); !r) {
            std::println(stderr, "[ws] !! session Attach FAILED: {}", r.error());
            Json::Value err;
            err["type"] = MessageTypeToString(MessageType::Error);
            err["message"] = r.error();
            conn->send(JsonDump(err));
            conn->shutdown();
            return;
        }

        std::lock_guard lk(mu_);
        sessions_[conn.get()] = std::move(session);
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        if (type != drogon::WebSocketMessageType::Text) return;
        std::println("[ws] <- {}b: {}", message.size(), Truncate(message));

        std::shared_ptr<WebRTCSession> session;
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(conn.get());
            if (it == sessions_.end()) {
                std::println(stderr, "[ws] !! no session for conn");
                return;
            }
            session = it->second;  // shared_ptr copy — безопасно работать вне лока
        }

        auto parsed = JsonParse(message);
        if (!parsed) {
            std::println(stderr, "[ws] !! bad JSON");
            return;
        }
        const Json::Value& msg = *parsed;
        auto mt = JsonStr(msg, "type");

        if (mt == "answer") {
            session->SetRemoteDescription("answer", JsonStr(msg, "sdp"));
        } else if (mt == "ice") {
            session->AddIceCandidate(JsonInt(msg, "sdpMLineIndex"), JsonStr(msg, "candidate"));
        } else if (mt == "ping") {
            Json::Value pong;
            pong["type"] = MessageTypeToString(MessageType::Pong);
            conn->send(JsonDump(pong));
        } else {
            std::println(stderr, "[ws] !! unknown type: {}", mt);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        std::shared_ptr<WebRTCSession> session;
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(conn.get());
            if (it != sessions_.end()) {
                session = std::move(it->second);
                sessions_.erase(it);
            }
        }
        if (session) {
            session.reset();  // ~WebRTCSession -> Detach -> RemoveConsumer
            std::println("[ws] === SESSION END ===");
        }
    }

   private:
    VideoStreamer& streamer_;
    std::mutex mu_;
    std::unordered_map<drogon::WebSocketConnection*, std::shared_ptr<WebRTCSession>> sessions_;
};

}  // namespace webrtc_poc
