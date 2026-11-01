#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <gst/gst.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <print>
#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

import webrtc_stream;

namespace {

void glfw_error_callback(int error, const char* description) {
    std::print(stderr, "GLFW Error {}: {}\n", error, description);
}

}  // namespace

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

    // ── Состояние подключения ────────────────────────────────────────────
    char host_buf[256] = "opi5plus.local:8888";
    std::string connect_error;
    std::unique_ptr<WebRTCVideoStream> stream;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ── Фоновое окно ─────────────────────────────────────────────────
        const ImVec2 display = ImGui::GetIO().DisplaySize;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(display);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        constexpr ImGuiWindowFlags kBgFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground;

        ImGui::Begin("##background", nullptr, kBgFlags);
        {
            if (stream) {
                std::unique_lock lk(stream->render_mutex());
                auto fv = stream->current_frame();
                if (fv.valid()) {
                    const float s = std::min(display.x / float(fv.width), display.y / float(fv.height));
                    const ImVec2 imgSize(fv.width * s, fv.height * s);
                    const ImVec2 imgPos((display.x - imgSize.x) * 0.5f, (display.y - imgSize.y) * 0.5f);
                    ImGui::SetCursorPos(imgPos);
                    ImGui::Image((ImTextureID)(intptr_t)fv.texture, imgSize, ImVec2(0, 0), ImVec2(1, 1));
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(2);

        // ── Окно подключения / статуса ───────────────────────────────────
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("status");

        if (!stream) {
            // ── Не подключены: форма ввода ────────────────────────────────
            ImGui::TextUnformatted("Сервер (host:port):");
            ImGui::SetNextItemWidth(260);
            const bool enter =
                ImGui::InputText("##host", host_buf, sizeof(host_buf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();

            if (ImGui::Button("Connect") || enter) {
                connect_error.clear();
                const std::string input(host_buf);

                // Разрешаем "<host>:<port>" и "[ipv6]:<port>".
                const auto colon = input.rfind(':');
                const bool ok = colon != std::string::npos && colon != 0 && colon + 1 < input.size();

                if (!ok) {
                    connect_error = "Ожидается формат <host>:<port>";
                } else {
                    try {
                        stream = std::make_unique<WebRTCVideoStream>("ws://" + input + "/ws", window);
                        stream->start();
                    } catch (const std::exception& e) {
                        connect_error = e.what();
                        stream.reset();
                    }
                }
            }

            if (!connect_error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", connect_error.c_str());
        } else {
            // ── Подключены: кнопка Disconnect + статус ──────────────────
            if (ImGui::Button("Disconnect")) {
                // ВАЖНО: после reset() указатель нулевой, до конца этого
                // кадра stream больше НЕ трогаем. Статус покажем в else.
                stream.reset();
            } else {
                ImGui::SameLine();
                std::unique_lock lk(stream->render_mutex());
                if (auto fv = stream->current_frame(); fv.valid())
                    ImGui::Text("Frame: %dx%d (tex %u)", fv.width, fv.height, fv.texture);
                else
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Ожидание потока...");

                if (auto err = stream->last_error(); !err.empty()) ImGui::TextWrapped("error: %s", err.c_str());
            }
        }

        ImGui::End();

        ImGui::Render();

        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glFinish();

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
