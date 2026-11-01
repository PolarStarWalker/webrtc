add_subdirectory(Server)


target_sources(client
        PRIVATE client.cpp
        PUBLIC FILE_SET CXX_MODULES FILES webrtc_stream.cppm
)
