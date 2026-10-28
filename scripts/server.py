#!/usr/bin/env python3
"""WebRTC H.264 camera streaming server (GStreamer webrtcbin + aiohttp)."""

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


# ---------- WebRTC session ----------

class WebRTCSession:
    def __init__(self, ws, camera_device):
        self.ws = ws
        self.camera_device = camera_device
        self.pipeline = None
        self.webrtcbin = None
        self.remote_description_set = False
        self.local_description_set = False
        self.loop = asyncio.get_event_loop()

    def build_pipeline(self):
        log.info("Building pipeline for %s", self.camera_device)
        self.pipeline = Gst.Pipeline.new(f"pipeline-{id(self)}")

        src = Gst.ElementFactory.make("v4l2src", "src")
        if src is None:
            raise RuntimeError("v4l2src not available")
        src.set_property("device", self.camera_device)

        caps = Gst.ElementFactory.make("capsfilter", "caps")
        caps.set_property("caps", Gst.Caps.from_string("video/x-h264"))

        parser = Gst.ElementFactory.make("h264parse", "parser")
        if parser is None:
            raise RuntimeError("h264parse not available")

        pay = Gst.ElementFactory.make("rtph264pay", "pay")
        if pay is None:
            raise RuntimeError(
                "rtph264pay not available (install gstreamer1.0-plugins-good)"
            )
        pay.set_property("config-interval", -1)
        pay.set_property("pt", 96)

        queue = Gst.ElementFactory.make("queue", "queue")

        self.webrtcbin = Gst.ElementFactory.make("webrtcbin", "webrtcbin")
        if self.webrtcbin is None:
            raise RuntimeError(
                "webrtcbin not available (install gstreamer1.0-plugins-bad)"
            )
        self.webrtcbin.set_property("bundle-policy", "max-bundle")

        for el in (src, caps, parser, pay, queue, self.webrtcbin):
            self.pipeline.add(el)

        for a, b in (
                (src, caps),
                (caps, parser),
                (parser, pay),
                (pay, queue),
                (queue, self.webrtcbin),
        ):
            if not a.link(b):
                raise RuntimeError(
                    f"Failed to link {a.get_name()} -> {b.get_name()}"
                )

        self.webrtcbin.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtcbin.connect("on-ice-candidate", self.on_ice_candidate)
        self.webrtcbin.connect("pad-added", self.on_pad_added)

        ret = self.pipeline.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("Pipeline failed to start")
        log.info("Pipeline state=PLAYING (ret=%s)", ret)

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
        # Пустая строка = конец сбора кандидатов; можно не пересылать.
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

        # Отправляем offer немедленно, local-description ставим асинхронно —
        # иначе получаем deadlock на внутреннем мьютексе webrtcbin.
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
        # У set-local-description NULL reply — это успех, не проверяем get_reply().
        log.info("Local description set OK")
        self.local_description_set = True

    def _on_remote_description_set(self, promise, user_data):
        result = promise.wait()
        if result != Gst.PromiseResult.REPLIED:
            log.error("set-remote-description: wait result=%s", result)
            return
        # То же: NULL reply — успех.
        log.info("Remote description applied OK")
        self.remote_description_set = True

    # --- Remote SDP / ICE ---

    def set_remote_description(self, sdp_type, sdp_text):
        log.info("set-remote-description type=%s size=%d", sdp_type, len(sdp_text))

        if sdp_type == "answer" and self.remote_description_set:
            log.warning("Duplicate remote answer — ignoring")
            return

        ret, sdp_msg = GstSdp.SDPMessage.new_from_text(sdp_text)
        # ФИКС: ret == OK (0) — успех, а не провал. not OK → ошибка.
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
        if self.pipeline:
            log.info("Stopping pipeline")
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None


# ---------- HTTP handlers ----------

async def ws_handler(request):
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)

    log.info("Client connected: %s", request.remote)
    session = WebRTCSession(ws, request.app["camera_device"])

    try:
        session.build_pipeline()
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

    app = web.Application()
    app["camera_device"] = cam
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/static/{path:.*}", make_static_handler())

    log.info("HTTP/WebSocket: http://%s:%d", HTTP_HOST, HTTP_PORT)
    web.run_app(app, host=HTTP_HOST, port=HTTP_PORT)


if __name__ == "__main__":
    main()
