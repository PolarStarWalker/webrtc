#!/usr/bin/env python3
"""
Python WebRTC клиент (answerer) на GStreamer для приёма H.264 потока.
Сервер выступает инициатором (offerer), клиент отвечает answer.
"""

import asyncio
import json
import sys
import threading

import gi
import websockets

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp, GLib


class WebRTCClient:
    def __init__(self, server_uri: str):
        self.server_uri = server_uri
        self.pipeline: Gst.Pipeline | None = None
        self.webrtc: Gst.Element | None = None
        self.depay: Gst.Element | None = None
        self.ws = None

        self._loop: asyncio.AbstractEventLoop | None = None
        self._glib_loop: GLib.MainLoop | None = None
        self._glib_thread: threading.Thread | None = None
        self._bus_watch_id: int | None = None

    def _start_glib(self):
        self._glib_loop = GLib.MainLoop()
        self._glib_thread = threading.Thread(
            target=self._glib_loop.run, name="glib-main", daemon=True
        )
        self._glib_thread.start()
        print("[*] GLib main loop запущен в фоновом потоке")

    def _stop_glib(self):
        if self._glib_loop:
            self._glib_loop.quit()
            self._glib_loop = None

    def build_pipeline(self):
        self.pipeline = Gst.Pipeline.new("webrtc-client")

        self.webrtc = Gst.ElementFactory.make("webrtcbin", "recv")
        self.webrtc.set_property("bundle-policy", "max-bundle")
        self.webrtc.set_property("stun-server", "stun://stun.l.google.com:19302")

        # RECVONLY transceiver — мы хотим только принимать видео
        direction = GstWebRTC.WebRTCRTPTransceiverDirection.RECVONLY
        caps = Gst.Caps.from_string(
            "application/x-rtp,media=video,encoding-name=H264,payload=96"
        )
        self.webrtc.emit("add-transceiver", direction, caps)

        self.depay = Gst.ElementFactory.make("rtph264depay", "depay")
        parse = Gst.ElementFactory.make("h264parse", "parse")
        decode = Gst.ElementFactory.make("avdec_h264", "decode")
        convert = Gst.ElementFactory.make("videoconvert", "convert")
        sink = Gst.ElementFactory.make("autovideosink", "sink")

        for name, el in [
            ("webrtcbin", self.webrtc), ("rtph264depay", self.depay),
            ("h264parse", parse), ("avdec_h264", decode),
            ("videoconvert", convert), ("autovideosink", sink),
        ]:
            if not el:
                raise RuntimeError(
                    f"Не удалось создать GStreamer элемент '{name}'. "
                    f"Проверьте установку gstreamer1.0-plugins-good/bad/libav."
                )
            self.pipeline.add(el)

        self.webrtc.connect("pad-added", self._on_pad_added)
        self.webrtc.connect("on-ice-candidate", self._on_ice_candidate)

        self.depay.link(parse)
        parse.link(decode)
        decode.link(convert)
        convert.link(sink)

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        self._bus_watch_id = bus.connect("message", self._on_bus_message)

    def _on_bus_message(self, bus, message):
        t = message.type
        if t == Gst.MessageType.ERROR:
            err, dbg = message.parse_error()
            print(f"[!] GStreamer ERROR: {err.message}\n    debug: {dbg}")
            if self._loop and self._loop.is_running():
                self._loop.call_soon_threadsafe(self._on_fatal)
        elif t == Gst.MessageType.EOS:
            print("[*] EOS")
            if self._loop and self._loop.is_running():
                self._loop.call_soon_threadsafe(self._on_fatal)
        elif t == Gst.MessageType.WARNING:
            w, d = message.parse_warning()
            print(f"[!] GStreamer WARNING: {w.message}\n    debug: {d}")

    def _on_fatal(self):
        asyncio.ensure_future(self._shutdown())

    async def _shutdown(self):
        if self.ws and not self.ws.closed:
            try:
                await self.ws.close()
            except Exception:
                pass

    def _on_pad_added(self, _webrtc, pad: Gst.Pad):
        if not pad.get_name().startswith("src_"):
            return
        caps = pad.get_current_caps() or pad.query_caps(None)
        if not caps:
            return
        s = caps.get_structure(0)
        if not s or s.get_name() != "application/x-rtp":
            return
        enc = s.get_string("encoding-name")
        if enc != "H264":
            print(f"[!] Неожидаемый кодек: {enc}, игнорирую пад")
            return
        sinkpad = self.depay.get_static_pad("sink")
        if not sinkpad.is_linked():
            pad.link(sinkpad)
            print("[*] webrtcbin → rtph264depay: linked")

    def _on_ice_candidate(self, _webrtc, mline_index: int, candidate: str):
        if not candidate:
            return
        if not (self._loop and self.ws and self.ws.state.name == "OPEN"):
            return
        self._loop.call_soon_threadsafe(
            lambda: asyncio.ensure_future(self._send_ice(mline_index, candidate))
        )

    async def _send_ice(self, mline_index: int, candidate: str):
        try:
            await self.ws.send(json.dumps({
                "type": "ice",
                "sdpMLineIndex": mline_index,
                "candidate": candidate,
            }))
        except Exception as e:
            print(f"[!] ICE send error: {e}")

    async def _handle_offer(self, sdp_text: str):
        """Получили offer от сервера → создаём answer и отправляем обратно."""
        res, sdpmsg = GstSdp.SDPMessage.new_from_text(sdp_text)
        if res != GstSdp.SDPResult.OK:
            raise RuntimeError("Не удалось распарсить SDP offer")

        offer = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.OFFER, sdpmsg
        )
        p = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", offer, p)
        p.wait()
        print("[*] SDP offer установлен")

        # Создаём answer
        ans_p = Gst.Promise.new()
        self.webrtc.emit("create-answer", None, ans_p)
        ans_p.wait()
        reply = ans_p.get_reply()
        if reply is None:
            raise RuntimeError("webrtcbin не вернул answer (promise пуст)")
        answer = reply.get_value("answer")
        if answer is None:
            raise RuntimeError("webrtcbin вернул пустой answer")

        local_p = Gst.Promise.new()
        self.webrtc.emit("set-local-description", answer, local_p)
        local_p.wait()

        await self.ws.send(json.dumps({
            "type": "answer",
            "sdp": answer.sdp.as_text(),
        }))
        print("[*] SDP answer отправлен")

    async def _handle_answer(self, sdp_text: str):
        """На случай, если сервер пришлёт answer (но в нашей схеме он offerer)."""
        res, sdpmsg = GstSdp.SDPMessage.new_from_text(sdp_text)
        if res != GstSdp.SDPResult.OK:
            raise RuntimeError("Не удалось распарсить SDP answer")
        answer = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg
        )
        p = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", answer, p)
        p.wait()
        print("[*] SDP answer установлен")

    async def run(self):
        Gst.init(None)
        self._loop = asyncio.get_running_loop()

        self._start_glib()

        try:
            self.build_pipeline()
            ret = self.pipeline.set_state(Gst.State.PLAYING)
            if ret == Gst.StateChangeReturn.FAILURE:
                raise RuntimeError("Pipeline перешёл в FAILURE при старте")
            print("[*] Pipeline PLAYING")

            async with websockets.connect(self.server_uri) as ws:
                self.ws = ws
                print(f"[*] Подключён к {self.server_uri}")

                # Клиент не создаёт offer — ждём его от сервера
                async for raw in ws:
                    try:
                        msg = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    t = msg.get("type")
                    if t == "offer":
                        await self._handle_offer(msg["sdp"])
                    elif t == "answer":
                        await self._handle_answer(msg["sdp"])
                    elif t == "ice":
                        self.webrtc.emit(
                            "add-ice-candidate",
                            msg["sdpMLineIndex"],
                            msg["candidate"],
                        )
                    elif t == "error":
                        print(f"[!] Сервер: {msg['message']}")

            print("[*] WebSocket закрыт")

        finally:
            if self.pipeline:
                if self._bus_watch_id is not None:
                    bus = self.pipeline.get_bus()
                    bus.disconnect(self._bus_watch_id)
                    bus.remove_signal_watch()
                    self._bus_watch_id = None
                self.pipeline.set_state(Gst.State.NULL)
                self.pipeline = None
            self._stop_glib()


def normalize_uri(raw: str) -> str:
    s = raw.strip()
    if "://" not in s:
        s = "ws://" + s
    scheme, _, rest = s.partition("://")
    if scheme not in ("ws", "wss"):
        raise ValueError(f"Неподдерживаемая схема: {scheme}")
    if "/" in rest:
        netloc, _, path = rest.partition("/")
        path = "/" + path
    else:
        netloc, path = rest, ""
    if ":" not in netloc:
        netloc = f"{netloc}:8888"
    if not path:
        path = "/ws"
    return f"{scheme}://{netloc}{path}"


if __name__ == "__main__":
    raw = sys.argv[1] if len(sys.argv) > 1 else "opi5plus.local"
    uri = normalize_uri(raw)
    print(f"[*] Использую URI: {uri}")
    try:
        asyncio.run(WebRTCClient(uri).run())
    except KeyboardInterrupt:
        print("\n[*] Выход")
