module;
#define GST_USE_UNSTABLE_API

#include <gst/gst.h>

#include <expected>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module VideoStreamer;

export namespace webrtc_poc {

struct ConsumerCallbacks {
    GCallback on_negotiation_needed;   // void(GstElement*, gpointer)
    GCallback on_ice_candidate;        // void(GstElement*, guint, gchar*, gpointer)
    GCallback on_pad_added;            // void(GstElement*, GstPad*, gpointer)
    gpointer  user_data;
};

struct ConsumerBranch {
    GstElement*              webrtcbin        = nullptr;
    GstPad*                  tee_pad          = nullptr;
    std::vector<GstElement*> elements;               // q_in, pay, q_out, webrtcbin
    gpointer                 signal_user_data = nullptr;
};

class VideoStreamer {
   public:
    explicit VideoStreamer(std::string device) : device_(std::move(device)) {
        loop_ = g_main_loop_new(nullptr, FALSE);
        loopThread_ = std::thread([this] {
            std::println("[rtc] GLib loop running");
            g_main_loop_run(loop_);
            std::println("[rtc] GLib loop exited");
        });
    }

    ~VideoStreamer() {
        Stop();
        if (loop_) {
            g_main_loop_quit(loop_);
            if (loopThread_.joinable()) loopThread_.join();
            g_main_loop_unref(loop_);
            loop_ = nullptr;
        }
    }

    VideoStreamer(const VideoStreamer&) = delete;
    VideoStreamer& operator=(const VideoStreamer&) = delete;

    // --- Shared pipeline lifecycle (вызывается из main один раз) ---

    std::expected<void, std::string> Start() {
        std::lock_guard lk(mu_);
        if (pipeline_) return std::unexpected("already started");

        pipeline_ = gst_pipeline_new("camera-pipeline");
        if (!pipeline_) return std::unexpected("gst_pipeline_new failed");

        auto fail = [this](std::string msg) -> std::expected<void, std::string> {
            if (pipeline_) {
                gst_element_set_state(pipeline_, GST_STATE_NULL);
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
            }
            tee_ = nullptr;
            return std::unexpected(std::move(msg));
        };

        auto* src      = gst_element_factory_make("v4l2src",   "src");
        auto* caps     = gst_element_factory_make("capsfilter","caps");
        auto* parser   = gst_element_factory_make("h264parse", "parser");
        auto* preQueue = gst_element_factory_make("queue",     "pre-tee-queue");
        tee_           = gst_element_factory_make("tee",       "tee");

        if (!src || !caps || !parser || !preQueue || !tee_)
            return fail("failed to create element (v4l2src/capsfilter/h264parse/queue/tee)");

        gst_bin_add_many(GST_BIN(pipeline_), src, caps, parser, preQueue, tee_, nullptr);

        g_object_set(src, "device", device_.c_str(), nullptr);

        GstCaps* videoCaps = gst_caps_from_string("video/x-h264");
        g_object_set(caps, "caps", videoCaps, nullptr);
        gst_caps_unref(videoCaps);

        // ВАЖНО: иначе tee падает при отключении последнего клиента
        g_object_set(tee_, "allow-not-linked", TRUE, nullptr);

        if (!gst_element_link_many(src, caps, parser, preQueue, tee_, nullptr))
            return fail("link failed: v4l2src ! video/x-h264 ! h264parse ! queue ! tee");

        InstallBusWatchLocked();

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            return fail("set_state(PLAYING) failed");

        std::println("[rtc] shared pipeline PLAYING");
        return {};
    }

    void Stop() {
        std::lock_guard lk(mu_);
        if (!pipeline_) return;
        std::println("[rtc] Stop() shared pipeline");
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        tee_      = nullptr;
    }

    // --- Per-client branch (может зваться из потоков Drogon параллельно) ---

    std::expected<std::shared_ptr<ConsumerBranch>, std::string>
    AddConsumer(const ConsumerCallbacks& cb) {
        std::lock_guard lk(mu_);
        if (!pipeline_ || !tee_) return std::unexpected("shared pipeline not started");

        auto branch = std::make_shared<ConsumerBranch>();
        branch->signal_user_data = cb.user_data;

        auto addEl = [this, &branch](const char* factory) -> GstElement* {
            auto* e = gst_element_factory_make(factory, nullptr);
            if (!e) return nullptr;
            gst_bin_add(GST_BIN(pipeline_), e);
            branch->elements.push_back(e);
            return e;
        };

        auto cleanup = [this, &branch]() {
            if (branch->webrtcbin && branch->signal_user_data)
                g_signal_handlers_disconnect_by_data(branch->webrtcbin,
                                                     branch->signal_user_data);
            for (auto* e : branch->elements) gst_element_set_state(e, GST_STATE_NULL);
            if (branch->tee_pad && tee_) {
                gst_element_release_request_pad(tee_, branch->tee_pad);
                branch->tee_pad = nullptr;
            }
            for (auto* e : branch->elements) gst_bin_remove(GST_BIN(pipeline_), e);
            branch->elements.clear();
            branch->webrtcbin = nullptr;
        };

        // 1) Создаём элементы
        auto* qIn = addEl("queue");
        if (!qIn) { cleanup(); return std::unexpected("queue missing"); }

        auto* pay = addEl("rtph264pay");
        if (!pay) { cleanup(); return std::unexpected("rtph264pay missing (plugins-good)"); }
        g_object_set(pay, "config-interval", -1, nullptr);
        g_object_set(pay, "pt", 96, nullptr);

        auto* qOut = addEl("queue");
        if (!qOut) { cleanup(); return std::unexpected("queue missing"); }

        auto* webrtcbin = addEl("webrtcbin");
        if (!webrtcbin) { cleanup(); return std::unexpected("webrtcbin missing"); }
        g_object_set(webrtcbin, "bundle-policy", "max-bundle", nullptr);
        branch->webrtcbin = webrtcbin;

        // 2) Сигналы ДО перевода в PAUSED/PLAYING — иначе потеряем
        //    первый on-negotiation-needed.
        g_signal_connect(webrtcbin, "on-negotiation-needed",
                         cb.on_negotiation_needed, cb.user_data);
        g_signal_connect(webrtcbin, "on-ice-candidate",
                         cb.on_ice_candidate, cb.user_data);
        g_signal_connect(webrtcbin, "pad-added",
                         cb.on_pad_added, cb.user_data);

        if (!gst_element_link_many(qIn, pay, qOut, webrtcbin, nullptr)) {
            cleanup();
            return std::unexpected("failed to link branch: queue ! rtph264pay ! queue ! webrtcbin");
        }

        // 3) PAUSED — безопасно подцепляться к «живому» tee
        for (auto* e : branch->elements) gst_element_set_state(e, GST_STATE_PAUSED);

        // 4) Запрос pad у tee
#if GST_CHECK_VERSION(1, 20, 0)
        GstPad* teePad = gst_element_request_pad_simple(tee_, "src_%u");
#else
        GstPad* teePad = gst_element_get_request_pad(tee_, "src_%u");
#endif
        if (!teePad) { cleanup(); return std::unexpected("failed to request tee src pad"); }
        branch->tee_pad = teePad;

        // 5) Линк
        GstPad* sinkPad = gst_element_get_static_pad(qIn, "sink");
        auto linkRet = gst_pad_link(teePad, sinkPad);
        if (sinkPad) gst_object_unref(sinkPad);
        if (linkRet != GST_PAD_LINK_OK) {
            cleanup();
            return std::unexpected("failed to link tee -> queue");
        }

        // 6) Синхронизация состояния с родителем
        for (auto* e : branch->elements) gst_element_sync_state_with_parent(e);

        std::println("[rtc] consumer attached ({} elements)", branch->elements.size());
        return branch;
    }

    void RemoveConsumer(std::shared_ptr<ConsumerBranch> branch) {
        std::lock_guard lk(mu_);
        if (!branch) return;
        std::println("[rtc] removing consumer");

        // 1) Отлинковать
        if (branch->tee_pad && !branch->elements.empty()) {
            GstPad* sink = gst_element_get_static_pad(branch->elements.front(), "sink");
            if (sink) {
                gst_pad_unlink(branch->tee_pad, sink);
                gst_object_unref(sink);
            }
        }
        // 2) Освободить request-pad
        if (branch->tee_pad && tee_) {
            gst_element_release_request_pad(tee_, branch->tee_pad);
            branch->tee_pad = nullptr;
        }
        // 3) Отключить сигналы
        if (branch->webrtcbin && branch->signal_user_data)
            g_signal_handlers_disconnect_by_data(branch->webrtcbin,
                                                 branch->signal_user_data);
        // 4) Погасить и выкинуть
        for (auto* e : branch->elements) gst_element_set_state(e, GST_STATE_NULL);
        for (auto* e : branch->elements) gst_bin_remove(GST_BIN(pipeline_), e);

        branch->elements.clear();
        branch->webrtcbin = nullptr;
    }

   private:
    void InstallBusWatchLocked() { /* тот же код, что и раньше, только для pipeline_ */ }

    std::string  device_;
    GstElement*  pipeline_ = nullptr;
    GstElement*  tee_      = nullptr;
    std::mutex   mu_;
    GMainLoop*   loop_     = nullptr;
    std::thread  loopThread_;
};

}  // namespace webrtc_poc