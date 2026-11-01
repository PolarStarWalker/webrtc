target_sources(webrtc
        PRIVATE
        webrtc.cpp
        PRIVATE FILE_SET CXX_MODULES FILES
        MessageType.cppm
        JsonHelpers.cppm
        Camera.cppm
        VideoStreamer.cppm
        WebRTCWebSocket.cppm
)