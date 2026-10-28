#!/usr/bin/env python3
"""
Python WebRTC-клиент на GStreamer для приёма H.264 потока с server.py.

Использование:
    python client.py [ws://SERVER:8080/ws]

Если дисплея нет — замените autovideosink на fakesink sync=false.
"""

import asyncio
import json
import sys

import gi
import websockets

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp


class WebRTCClient:
    def __init__(self, server_uri: str):
        self.server_uri = server_uri
        self.pipeline: Gst.Pipeline | None = None
        self.webrtc: Gst.Element | None = None
        self.pay = None
        self.ws = None

    # ---------- GStreamer ----------
    def build_pipeline(self):
        self.pipeline = Gst.Pipeline.new("webrtc-client")

        self.webrtc = Gst.ElementFactory.make("webrtcbin", "recv")
        self.webrtc.set_property("bundle-policy", "max-bundle")
        self.webrtc.set_property("stun-server", "stun://stun.l.google.com:19302")

        # RECVONLY transceiver — создаст sink pad для приёма RTP
        direction = GstWebRTC.WebRTCRTPTransceiverDirection.RECVONLY
        caps = Gst.Caps.from_string(
            "application/x-rtp,media=video,encoding-name=H264,payload=96"
        )
        self.webrtc.emit("add-transceiver", direction, caps)

        depay = Gst.ElementFactory.make("rtph264depay", "depay")
        parse = Gst.ElementFactory.make("h264parse", "parse")
        decode = Gst.ElementFactory.make("avdec_h264", "decode")
        convert = Gst.ElementFactory.make("videoconvert", "convert")
        # Для headless-режима замените autovideosink на fakesink sync=false
        sink = Gst.ElementFactory.make("autovideosink", "sink")

        for el in (self.webrtc, depay, parse, decode, convert, sink):
            if not el:
                raise RuntimeError(f"Не удалось создать элемент: {el}")
            self.pipeline.add(el)

        self.webrtc.connect("pad-added", self._on_pad_added)
        self.webrtc.connect("on-ice-candidate", self._on_ice_candidate)

        depay.link(parse)
        parse.link(decode)
        decode.link(convert)
        convert.link(sink)

        self._depay = depay

    def _on_pad_added(self, _webrtc, pad: Gst.Pad):
        """webrtcbin создаёт src_%u пад при приёме RTP."""
        if not pad.get_name().startswith("src_"):
            return
        caps = pad.get_current_caps() or pad.query_caps(None)
        if not caps:
            return
        s = caps.get_structure(0)
        if s.get_name() == "application/x-rtp" and \
                s.get_string("encoding-name") == "H264":
            sinkpad = self._depay.get_static_pad("sink")
            if not sinkpad.is_linked():
                pad.link(sinkpad)

    def _on_ice_candidate(self, _webrtc, mline_index: int, candidate: str):
        if candidate and self.ws and self.ws.open:
            asyncio.create_task(self.ws.send(json.dumps({
                "type": "ice",
                "sdpMLineIndex": mline_index,
                "candidate": candidate,
            })))

    async def _create_and_send_offer(self):
        promise = Gst.Promise.new()
        self.webrtc.emit("create-offer", None, promise)
        promise.interrupt()
        reply = promise.get_reply()
        offer = reply.get_value("offer")

        local_p = Gst.Promise.new()
        self.webrtc.emit("set-local-description", offer, local_p)
        local_p.interrupt()

        await self.ws.send(json.dumps({
            "type": "offer",
            "sdp": offer.sdp.as_text(),
        }))
        print("[*] SDP offer отправлен")

    async def _handle_answer(self, sdp_text: str):
        res, sdpmsg = GstSdp.SDPMessage.new_from_text(sdp_text)
        if res != GstSdp.SDPResult.OK:
            raise RuntimeError("Не удалось распарсить SDP answer")
        answer = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg
        )
        p = Gst.Promise.new()
        self.webrtc.emit("set-remote-description", answer, p)
        p.interrupt()
        print("[*] SDP answer установлен")

    # ---------- WebSocket signaling ----------
    async def run(self):
        Gst.init(None)
        self.build_pipeline()
        self.pipeline.set_state(Gst.State.PLAYING)

        async with websockets.connect(self.server_uri) as ws:
            self.ws = ws
            print(f"[*] Подключён к {self.server_uri}")
            await self._create_and_send_offer()

            async for raw in ws:
                msg = json.loads(raw)
                t = msg.get("type")
                if t == "offer":
                    # сервер в этом PoC присылает answer в поле "sdp"
                    await self._handle_answer(msg["sdp"])
                elif t == "ice":
                    self.webrtc.emit(
                        "add-ice-candidate",
                        msg["sdpMLineIndex"],
                        msg["candidate"],
                    )
                elif t == "error":
                    print(f"[!] Сервер: {msg['message']}")

        self.pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    uri = sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:8080/ws"
    try:
        asyncio.run(WebRTCClient(uri).run())
    except KeyboardInterrupt:
        print("\n[*] Выход")
