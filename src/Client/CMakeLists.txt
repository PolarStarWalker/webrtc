target_sources(client
        PRIVATE client.cpp
        PUBLIC FILE_SET CXX_MODULES FILES WebrtcStream.cppm
)
