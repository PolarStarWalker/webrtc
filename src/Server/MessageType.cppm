module;

#include <cstdint>
#include <string>
#include <string_view>

export module MessageType;

export namespace webrtc_poc {

enum class MessageType : std::uint8_t { Offer, Answer, Ice, Error, Ping, Pong };

std::string MessageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::Offer:
            return "offer";
        case MessageType::Answer:
            return "answer";
        case MessageType::Ice:
            return "ice";
        case MessageType::Error:
            return "error";
        case MessageType::Ping:
            return "ping";
        case MessageType::Pong:
            return "pong";
    }
    return "unknown";
}

}  // namespace webrtc_poc
