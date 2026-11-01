#define GST_USE_UNSTABLE_API

#include <drogon/drogon.h>
#include <gst/gst.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <string_view>

import Camera;
import VideoStreamer;
import WebRTCWebSocket;

using webrtc_poc::SelectCamera;
using webrtc_poc::VideoStreamer;
using webrtc_poc::WebRTCWebSocket;

namespace {

std::string LoadIndexHtml() {
    for (auto* path : {"index.html", "static/index.html", "scripts/static/index.html"}) {
        if (std::filesystem::exists(path)) {
            std::ifstream f(path);
            std::string c{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            std::println("Serving index from: {} ({}b)", path, c.size());
            return c;
        }
    }
    std::println(stderr,
                 "!! index.html not found near CWD. Returning stub. "
                 "CMake should have copied it into build-dir.");
    return "<h1>index.html not found</h1>";
}

}  // namespace

extern "C" {
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(videoconvertscale);
GST_PLUGIN_STATIC_DECLARE(video4linux2);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(webrtc);
GST_PLUGIN_STATIC_DECLARE(nice);
GST_PLUGIN_STATIC_DECLARE(dtls);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(udp);
}

int main() {
    gst_init(nullptr, nullptr);

    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(videoconvertscale);
    GST_PLUGIN_STATIC_REGISTER(video4linux2);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(webrtc);
    GST_PLUGIN_STATIC_REGISTER(nice);
    GST_PLUGIN_STATIC_REGISTER(dtls);
    GST_PLUGIN_STATIC_REGISTER(srtp);
    GST_PLUGIN_STATIC_REGISTER(udp);

    const std::string camera = SelectCamera();
    if (camera.empty()) return 1;

    VideoStreamer streamer(camera);
    if (auto r = streamer.Start(); !r) {
        std::println(stderr, "!! Failed to start shared pipeline: {}", r.error());
        return 1;
    }

    const std::string html = LoadIndexHtml();
    const std::string_view htmlView = html;

    auto serveIndex = [htmlView](const drogon::HttpRequestPtr&,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeString("text/html; charset=utf-8");
        resp->setBody(std::string{htmlView});
        cb(resp);
    };

    std::println("HTTP/WS on http://0.0.0.0:8888");

    drogon::app()
        .addListener("0.0.0.0", 8888)
        .registerHandler("/", serveIndex)
        .registerHandler("/index.html", serveIndex)
        .registerController(std::make_shared<WebRTCWebSocket>(streamer))
        .run();

    std::println("Bye");
    return 0;
}
