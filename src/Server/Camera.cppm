module;

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

export module Camera;

export namespace webrtc_poc {

inline bool DeviceSupportsH264(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ::close(fd);
        return false;
    }

    auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        ::close(fd);
        return false;
    }

    bool hasH264 = false;
    v4l2_fmtdesc fmtDesc{};
    fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtDesc.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0; ++fmtDesc.index) {
        if (fmtDesc.pixelformat == V4L2_PIX_FMT_H264) {
            hasH264 = true;
            break;
        }
    }
    ::close(fd);
    return hasH264;
}

inline std::vector<std::string> FindH264Cameras() {
    std::vector<std::string> cams;
    for (int i = 0; i < 64; ++i) {
        auto path = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(path)) continue;
        if (DeviceSupportsH264(path)) {
            std::println("[cam] {}: H.264 ✓", path);
            cams.push_back(path);
        }
    }
    return cams;
}

inline void PrintAllCameras() {
    std::println(stderr, "Scanning /dev/video*:");
    bool any = false;
    for (int i = 0; i < 64; ++i) {
        auto path = "/dev/video" + std::to_string(i);
        if (!std::filesystem::exists(path)) continue;
        any = true;
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::println(stderr, "  {}: cannot open ({})", path, std::strerror(errno));
            continue;
        }
        v4l2_capability cap{};
        std::string name = "<unknown>";
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) name = reinterpret_cast<const char*>(cap.card);

        std::string formats;
        v4l2_fmtdesc fmtDesc{};
        fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fmtDesc.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0; ++fmtDesc.index) {
            if (!formats.empty()) formats += ", ";
            formats += reinterpret_cast<const char*>(&fmtDesc.description);
        }
        ::close(fd);
        std::println(stderr, "  {} [{}]: {}", path, name, formats.empty() ? "<no capture formats>" : formats);
    }
    if (!any) std::println(stderr, "  (no /dev/video* devices found)");
}

inline std::string SelectCamera() {
    if (const char* env = std::getenv("WEBRTC_CAMERA"); env && *env) {
        std::string cam = env;
        std::println("Using camera from WEBRTC_CAMERA: {}", cam);
        if (!std::filesystem::exists(cam)) {
            std::println(stderr, "!! {} does not exist", cam);
            return {};
        }
        if (!DeviceSupportsH264(cam)) std::println(stderr, "!! {} is not H.264-capable; passthrough will fail", cam);
        return cam;
    }

    auto cams = FindH264Cameras();
    if (cams.empty()) {
        std::println(stderr,
                     "\nNo H.264-capable camera found.\n"
                     "Passthrough needs a camera that outputs raw H.264 over V4L2.\n\n");
        PrintAllCameras();
        std::println(stderr,
                     "\nOptions:\n"
                     "  1. Connect a camera that outputs H.264.\n"
                     "  2. Force a specific device: WEBRTC_CAMERA=/dev/videoN ./webrtc\n"
                     "  3. Switch pipeline to transcode MJPEG/YUYV -> H.264.\n");
        return {};
    }
    std::println("H.264 camera(s) found:");
    for (auto& c : cams) std::println("  {}", c);
    std::println("Using: {}", cams.front());
    return cams.front();
}

}  // namespace webrtc_poc
