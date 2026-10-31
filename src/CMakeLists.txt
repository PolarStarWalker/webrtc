target_sources(webrtc PRIVATE
        webrtc.cpp
)

target_sources(client
        PRIVATE client.cpp
        PUBLIC FILE_SET CXX_MODULES FILES webrtc_stream.cppm
)
