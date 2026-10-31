#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <gst/gst.h>

#include <cstdio>
#include <memory>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

import webrtc_stream;

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

extern "C" {
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(videoconvertscale);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(webrtc);
GST_PLUGIN_STATIC_DECLARE(nice);
GST_PLUGIN_STATIC_DECLARE(dtls);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(udp);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(libav);
}

int main() {
    gst_init(nullptr, nullptr);

    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(videoconvertscale);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(webrtc);
    GST_PLUGIN_STATIC_REGISTER(nice);
    GST_PLUGIN_STATIC_REGISTER(dtls);
    GST_PLUGIN_STATIC_REGISTER(srtp);
    GST_PLUGIN_STATIC_REGISTER(udp);
    GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(libav);

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "ImGui + WebRTC", nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ── Поток WebRTC ───────────────────────────────────────────
    auto stream = std::make_unique<WebRTCVideoStream>("ws://opi5plus.local:8888/ws", window);
    stream->start();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("WebRTC stream");

        // Держим мьютекс от ImGui::Image до glFinish(), чтобы
        // video-поток не перезаписал текстуру, пока GPU её читает.
        std::unique_lock lk(stream->render_mutex());
        auto fv = stream->current_frame();

        if (fv.valid()) {
            ImGui::Text("Frame: %dx%d (tex %u)", fv.width, fv.height, fv.texture);
            // aspect-fit в доступную область
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float sx = avail.x / fv.width;
            float sy = avail.y / fv.height;
            float s = sx < sy ? sx : sy;
            ImGui::Image((ImTextureID)(intptr_t)fv.texture, ImVec2(fv.width * s, fv.height * s));
        } else {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Ожидание потока...");
            auto err = stream->last_error();
            if (!err.empty()) ImGui::TextWrapped("error: %s", err.c_str());
        }

        ImGui::End();
        ImGui::Render();

        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Ждём, пока GPU дочитает текстуру, и отпускаем мьютекс
        glFinish();
        lk.unlock();

        glfwSwapBuffers(window);
    }

    stream.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
