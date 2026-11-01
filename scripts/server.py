#!/usr/bin/env python3
"""WebRTC H.264 camera streaming server (GStreamer webrtcbin + aiohttp).

Multi-client version: один общий pipeline читает камеру и через tee
раздаёт поток в персональные ветки для каждого клиента.
"""

import asyncio
import glob
import json
import logging
import os
import shutil
import subprocess
import sys

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")

from gi.repository import Gst, GstWebRTC, GstSdp  # noqa: E402

from aiohttp import web, WSMsgType  # noqa: E402

Gst.init(None)

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
log = logging.getLogger("webrtc-server")

HTTP_HOST = "0.0.0.0"
HTTP_PORT = 8888

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPTS_DIR)


# ---------- Camera discovery ----------

def find_h264_cameras():
    devices = sorted(glob.glob("/dev/video*"))
    if shutil.which("v4l2-ctl") is None:
        log.warning("v4l2-ctl not found; falling back to /dev/video* glob")
        return devices

    cams = []
    for dev in devices:
        try:
            out = subprocess.run(
                ["v4l2-ctl", "-d", dev, "--list-formats-ext"],
                capture_output=True, text=True, timeout=3,
            ).stdout
        except Exception as e:
            log.debug("v4l2-ctl failed for %s: %s", dev, e)
            continue
        if "H264" in out or "H.264" in out:
            cams.append(dev)
    return cams


# ---------- Static file lookup ----------

def _index_candidates():
    return [
        os.path.join(SCRIPTS_DIR, "index.html"),
        os.path.join(SCRIPTS_DIR, "static", "index.html"),
        os.path.join(SCRIPTS_DIR, "..", "web", "index.html"),
        os.path.join(SCRIPTS_DIR, "..", "static", "index.html"),
        os.path.join(SCRIPTS_DIR, "..", "client.html"),
        os.path.join(PROJECT_DIR, "index.html"),
    ]


def _find_index():
    for p in _index_candidates():
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


# ---------- Shared pipeline (один на всех) ----------

class SharedCameraPipeline:
    """Один pipeline читает камеру и раздаёт H.264 в N веток через tee."""

    def __init__(self, device):
        self.device = device
        self.pipeline = None
        self.tee = None

    def start(self):
        log.info("Building shared pipeline for %s", self.device)
        self.pipeline = Gst.Pipeline.new("camera-pipeline")

        src = Gst.ElementFactory.make("v4l2src", "src")
        if src is None:
            raise RuntimeError("v4l2src not available")
        src.set_property("device", self.device)

        caps = Gst.ElementFactory.make("capsfilter", "caps")
        caps.set_property("caps", Gst.Caps.from_string("video/x-h264"))

        parser = Gst.ElementFactory.make("h264parse", "parser")
        if parser is None:
            raise RuntimeError("h264parse not available")

        pre_queue = Gst.ElementFactory.make("queue", "pre-tee-queue")

        self.tee = Gst.ElementFactory.make("tee", "tee")
        if self.tee is None:
            raise RuntimeError("tee not available")
        # Не падать, если ни один клиент ещё не подключён
        self.tee.set_property("allow-not-linked", True)

        for el in (src, caps, parser, pre_queue, self.tee):
            self.pipeline.add(el)

        for a, b in ((src, caps), (caps, parser),
                     (parser, pre_queue), (pre_queue, self.tee)):
            if not a.link(b):
                raise RuntimeError(
                    f"Failed to link {a.get_name()} -> {b.get_name()}"
                )

        ret = self.pipeline.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("Shared pipeline failed to start")
        log.info("Shared pipeline PLAYING (ret=%s)", ret)

    # ---- Жизненный цикл клиентской ветки ----

    def add_consumer(self, on_negotiation_needed, on_ice_candidate, on_pad_added):
        """Создать ветку queue → rtph264pay → queue → webrtcbin и подключить к tee.

        Возвращает dict-дескриптор ветки, который потом надо передать в
        remove_consumer(). Сигналы webrtcbin подключаются ДО перевода ветки
        в PAUSED/PLAYING, чтобы не пропустить on-negotiation-needed.
        """
        q_in = Gst.ElementFactory.make("queue", None)
        pay = Gst.ElementFactory.make("rtph264pay", None)
        if pay is None:
            raise RuntimeError(
                "rtph264pay not available (install gstreamer1.0-plugins-good)"
            )
        pay.set_property("config-interval", -1)  # SPS/PPS перед каждым IDR
        pay.set_property("pt", 96)

        q_out = Gst.ElementFactory.make("queue", None)

        webrtcbin = Gst.ElementFactory.make("webrtcbin", None)
        if webrtcbin is None:
            raise RuntimeError(
                "webrtcbin not available (install gstreamer1.0-plugins-bad)"
            )
        webrtcbin.set_property("bundle-policy", "max-bundle")

        # ВАЖНО: сигналы подключаем до set_state, иначе можем потерять
        # первый on-negotiation-needed.
        webrtcbin.connect("on-negotiation-needed", on_negotiation_needed)
        webrtcbin.connect("on-ice-candidate", on_ice_candidate)
        webrtcbin.connect("pad-added", on_pad_added)

        elements = [q_in, pay, q_out, webrtcbin]
        for el in elements:
            self.pipeline.add(el)

        for a, b in ((q_in, pay), (pay, q_out), (q_out, webrtcbin)):
            if not a.link(b):
                raise RuntimeError(
                    f"Failed to link {a.get_name()} -> {b.get_name()}"
                )

        # Сначала PAUSED — безопасно привязывать к живому tee
        for el in elements:
            el.set_state(Gst.State.PAUSED)

        if hasattr(self.tee, "request_pad_simple"):
            tee_pad = self.tee.request_pad_simple("src_%u")
        else:  # GStreamer < 1.20
            tee_pad = self.tee.get_request_pad("src_%u")

        if tee_pad is None:
            raise RuntimeError("Failed to request tee src pad")

        sink_pad = q_in.get_static_pad("sink")
        if tee_pad.link(sink_pad) != Gst.PadLinkReturn.OK:
            self.tee.release_request_pad(tee_pad)
            raise RuntimeError("Failed to link tee -> queue")

        # Теперь запускаем
        for el in elements:
            el.sync_state_with_parent()

        return {
            "webrtcbin": webrtcbin,
            "tee_pad": tee_pad,
            "elements": elements,
        }

    def remove_consumer(self, branch):
        """Аккуратно отцепить ветку от tee и уничтожить её."""
        tee_pad = branch["tee_pad"]
        elements = branch["elements"]
        q_in = elements[0]

        # 1) Отлинковать от tee — дальнейшие буферы в эту ветку не пойдут
        try:
            sink_pad = q_in.get_static_pad("sink")
            if sink_pad:
                tee_pad.unlink(sink_pad)
        except Exception as e:
            log.error("Failed to unlink tee pad: %s", e)

        # 2) Освободить request-pad
        try:
            self.tee.release_request_pad(tee_pad)
        except Exception as e:
            log.error("Failed to release tee pad: %s", e)

        # 3) Погасить элементы и убрать из pipeline
        for el in elements:
            try:
                el.set_state(Gst.State.NULL)
            except Exception:
                pass
        for el in elements:
            try:
                self.pipeline.remove(el)
            except Exception as e:
                log.error("Failed to remove element %s: %s", el.get_name(), e)

    def stop(self):
        if self.pipeline:
            log.info("Stopping shared pipeline")
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None


# ---------- WebRTC session (по одной на клиента) ----------

class WebRTCSession:
    def __init__(self, ws, shared_pipeline):
        self.ws = ws
        self.shared = shared_pipeline
        self.branch = None
        self.webrtcbin = None
        self.remote_description_set = False
        self.local_description_set = False
        self.loop = asyncio.get_event_loop()

    def attach(self):
        self.branch = self.shared.add_consumer(
            self.on_negotiation_needed,
            self.on_ice_candidate,
            self.on_pad_added,
        )
        self.webrtcbin = self.branch["webrtcbin"]
        log.info("Branch attached (%d elements)", len(self.branch["elements"]))

    # --- GStreamer signals ---

    def on_pad_added(self, webrtcbin, pad):
        log.debug("pad-added: %s", pad.get_name())

    def on_negotiation_needed(self, webrtcbin):
        log.info("Negotiation needed -> create-offer")
        promise = Gst.Promise.new_with_change_func(
            self._on_offer_created, webrtcbin, None
        )
        webrtcbin.emit("create-offer", None, promise)

    def on_ice_candidate(self, webrtcbin, mline_index, candidate):
        if not candidate:
            log.debug("Local ICE mline=%d end-of-candidates", mline_index)
            return
        log.debug("Local ICE mline=%d candidate=%s", mline_index, candidate)
        self._send({
            "type": "ice",
            "sdpMLineIndex": mline_index,
            "candidate": candidate,
        })

    # --- Promise callbacks ---

    def _on_offer_created(self, promise, webrtcbin, user_data):
        result = promise.wait()
        if result != Gst.PromiseResult.REPLIED:
            log.error("create-offer: wait result=%s", result)
            return
        reply = promise.get_reply()
        if reply is None:
            log.error("create-offer: no reply struct")
            return
        offer = reply.get_value("offer")
        if offer is None:
            log.error("create-offer: no 'offer' field")
            return

        sdp_text = offer.sdp.as_text()
        log.info("Offer created (%d bytes)", len(sdp_text))

        # Отправляем offer до set-local-description, иначе deadlock на
        # внутреннем мьютексе webrtcbin.
        self._send({"type": "offer", "sdp": sdp_text})

        local_promise = Gst.Promise.new_with_change_func(
            self._on_local_description_set, webrtcbin, None
        )
        webrtcbin.emit("set-local-description", offer, local_promise)

    def _on_local_description_set(self, promise, webrtcbin, user_data):
        result = promise.wait()
        if result != Gst.PromiseResult.REPLIED:
            log.error("set-local-description: wait result=%s", result)
            return
        log.info("Local description set OK")
        self.local_description_set = True

    def _on_remote_description_set(self, promise, user_data):
        result = promise.wait()
        if result != Gst.PromiseResult.REPLIED:
            log.error("set-remote-description: wait result=%s", result)
            return
        log.info("Remote description applied OK")
        self.remote_description_set = True

    # --- Remote SDP / ICE ---

    def set_remote_description(self, sdp_type, sdp_text):
        log.info("set-remote-description type=%s size=%d", sdp_type, len(sdp_text))

        if sdp_type == "answer" and self.remote_description_set:
            log.warning("Duplicate remote answer — ignoring")
            return

        ret, sdp_msg = GstSdp.SDPMessage.new_from_text(sdp_text)
        if ret != GstSdp.SDPResult.OK:
            log.error("Failed to parse SDP (result=%s): %r", ret, sdp_text[:200])
            return

        if sdp_type == "offer":
            remote = GstWebRTC.WebRTCSessionDescription.new(
                GstWebRTC.WebRTCSDPType.OFFER, sdp_msg
            )
        elif sdp_type == "answer":
            remote = GstWebRTC.WebRTCSessionDescription.new(
                GstWebRTC.WebRTCSDPType.ANSWER, sdp_msg
            )
        else:
            log.error("Unsupported SDP type: %s", sdp_type)
            return

        promise = Gst.Promise.new_with_change_func(
            self._on_remote_description_set, None
        )
        self.webrtcbin.emit("set-remote-description", remote, promise)

    def add_ice_candidate(self, mline_index, candidate):
        if not candidate:
            log.debug("Remote ICE end-of-candidates")
            return
        log.debug("Remote ICE mline=%d candidate=%s", mline_index, candidate)
        self.webrtcbin.emit("add-ice-candidate", mline_index, candidate)

    # --- Helpers ---

    def _send(self, obj):
        try:
            asyncio.run_coroutine_threadsafe(
                self.ws.send_str(json.dumps(obj)), self.loop
            )
        except Exception as e:
            log.error("WS send failed: %s", e)

    def close(self):
        if self.branch:
            log.info("Detaching client branch")
            self.shared.remove_consumer(self.branch)
            self.branch = None
            self.webrtcbin = None


# ---------- HTTP handlers ----------

async def ws_handler(request):
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)

    log.info("Client connected: %s", request.remote)
    session = WebRTCSession(ws, request.app["shared_pipeline"])

    try:
        session.attach()
    except Exception as e:
        log.exception("Pipeline setup failed")
        await ws.send_str(json.dumps({"type": "error", "message": str(e)}))
        await ws.close()
        return ws

    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                except json.JSONDecodeError:
                    log.warning("Non-JSON message: %r", msg.data)
                    continue

                mtype = data.get("type")
                log.info("<- %s", mtype)

                if mtype == "answer":
                    session.set_remote_description("answer", data["sdp"])
                elif mtype == "ice":
                    session.add_ice_candidate(
                        data["sdpMLineIndex"], data.get("candidate")
                    )
                elif mtype == "ping":
                    await ws.send_str(json.dumps({"type": "pong"}))
                else:
                    log.warning("Unknown message type: %s", mtype)
            elif msg.type == WSMsgType.ERROR:
                log.error("WS error: %s", ws.exception())
    finally:
        log.info("Client disconnected: %s", request.remote)
        session.close()

    return ws


async def index(request):
    path = _find_index()
    if path:
        log.debug("Serving index from %s", path)
        return web.FileResponse(path)

    searched = "\n".join("  - " + os.path.abspath(p) for p in _index_candidates())
    msg = (
        "index.html not found.\n\nSearched:\n"
        f"{searched}\n\n"
        "Place index.html in any of the above (e.g. scripts/static/index.html)."
    )
    log.error("index.html not found. Searched:\n%s", searched)
    return web.Response(text=msg, status=404, content_type="text/plain")


def make_static_handler():
    static_dirs = [
        os.path.join(SCRIPTS_DIR, "static"),
        os.path.join(PROJECT_DIR, "static"),
    ]

    async def handler(request):
        rel = request.match_info["path"]
        if ".." in rel.split("/") or rel.startswith("/"):
            raise web.HTTPForbidden()
        for base in static_dirs:
            full = os.path.abspath(os.path.join(base, rel))
            if (
                    full.startswith(os.path.abspath(base) + os.sep)
                    and os.path.isfile(full)
            ):
                return web.FileResponse(full)
        raise web.HTTPNotFound()

    return handler


def main():
    cams = find_h264_cameras()
    if not cams:
        log.error("No H.264 cameras found")
        sys.exit(1)
    log.info("Found H.264 camera(s): %s", cams)
    cam = cams[0]
    log.info("Using: %s", cam)

    found_index = _find_index()
    if found_index:
        log.info("index.html: %s", found_index)
    else:
        log.warning("index.html НЕ найден. Положите его в scripts/static/index.html")
        for p in _index_candidates():
            log.warning("  проверено: %s", os.path.abspath(p))

    # Общий pipeline — один на весь сервер
    shared = SharedCameraPipeline(cam)
    try:
        shared.start()
    except Exception:
        log.exception("Failed to start shared pipeline")
        sys.exit(1)

    app = web.Application()
    app["camera_device"] = cam
    app["shared_pipeline"] = shared
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/static/{path:.*}", make_static_handler())

    async def _cleanup(app):
        shared.stop()

    app.on_cleanup.append(_cleanup)

    log.info("HTTP/WebSocket: http://%s:%d", HTTP_HOST, HTTP_PORT)
    web.run_app(app, host=HTTP_HOST, port=HTTP_PORT)


if __name__ == "__main__":
    main()