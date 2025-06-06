#!/usr/bin/env python

# OpenIO SDS functional tests
# Copyright (C) 2015-2019 OpenIO SAS, as part of OpenIO SDS
# Copyright (C) 2022-2025 OVH SAS
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3.0 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library.

import http.server
import sys
import threading
from ctypes import cdll


class DumbHttpMock(http.server.BaseHTTPRequestHandler):
    def reply(self):
        inbody = None
        length = self.headers.get("content-length")
        if length is None:
            inbody = []
            while True:
                chunk_length = self.rfile.readline()
                chunk_length = int(chunk_length)
                self.rfile.readline()
                if chunk_length <= 0:
                    break
                chunk = self.rfile.read(chunk_length)
                inbody.append(chunk)
            inbody = b"".join(inbody)
        elif int(length) > 0:
            inbody = self.rfile.read(int(length))
        else:
            inbody = b""
        self.log_message("body: %d", len(inbody))

        if len(self.server.expectations) <= 0:
            return
        req, rep = self.server.expectations.pop(0)

        # Check the request
        qpath, qhdr, qbody = req
        if qpath is not None and qpath != self.path:
            raise Exception(
                "unexpected request got: %s, expected: %s"
                % (str(self.path), str(qpath))
            )

        if qhdr is not None:
            for k, v in qhdr.items():
                if k not in self.headers:
                    raise Exception("missing headers: " + k)
                if self.headers[k] != v:
                    raise Exception(
                        "invalid header [%s] value got: %s"
                        % (str(self.headers[k]), str(v))
                    )

        # Reply
        pcode, phdr, pbody = rep
        if isinstance(pbody, str):
            pbody = pbody.encode("utf-8")
        self.send_response(pcode)
        for k, v in phdr.items():
            self.send_header(k, v)
        if "Content-Length" not in phdr:
            self.send_header("Content-Length", str(len(pbody)))
        self.end_headers()
        self.wfile.write(pbody)

    def do_HEAD(self):
        return self.reply()

    def do_GET(self):
        return self.reply()

    def do_POST(self):
        return self.reply()

    def do_PUT(self):
        return self.reply()

    def do_DELETE(self):
        return self.reply()


class Service(threading.Thread):
    def __init__(self, srv):
        threading.Thread.__init__(self)
        self.srv = srv

    def run(self):
        self.srv.serve_forever()


def test_ok(lib):
    servers, services, urls = [], [], []
    for i in range(3):
        servers.append(http.server.HTTPServer(("127.0.0.1", 7000 + i), DumbHttpMock))
    for h in servers:
        urls.append((f"http://127.0.0.1:{h.server_port}/").encode("utf-8"))
        services.append(Service(h))
    expectations = [
        (("/", {"Content-Length": "0"}, ""), (200, {}, "")),
        (("/", {"Content-Length": "0"}, ""), (200, {}, "")),
        (("/", {"Content-Length": "0"}, ""), (200, {}, "")),
        (("/", {"Content-Length": "0"}, ""), (200, {}, "")),
        (("/", {"Content-Length": "1"}, "0"), (200, {}, "")),
        (("/", {"Content-Length": "1"}, "0"), (200, {}, "")),
        (("/", {"Content-Length": "1"}, "0"), (200, {}, "")),
        (("/", {"Content-Length": "1"}, "0"), (200, {}, "")),
        (("/", {"Content-Length": "128"}, "0" * 128), (200, {}, "")),
        (("/", {"Content-Length": "128"}, "0" * 128), (200, {}, "")),
        (("/", {"Content-Length": "128"}, "0" * 128), (200, {}, "")),
        (("/", {"Content-Length": "128"}, "0" * 128), (200, {}, "")),
        (("/", {"Transfer-Encoding": "chunked"}, "0" * 128), (200, {}, "")),
    ]
    for h in servers:
        h.expectations = expectations
    for s in services:
        s.start()
    try:
        lib.test_upload_ok(0, 0, None)

        lib.test_upload_ok(0, 0, urls[0], None)
        lib.test_upload_ok(0, 0, urls[0], urls[1], urls[2], None)

        lib.test_upload_ok(0, 1, urls[0], None)
        lib.test_upload_ok(0, 1, urls[0], urls[1], urls[2], None)

        lib.test_upload_ok(0, 128, urls[0], None)
        lib.test_upload_ok(0, 128, urls[0], urls[1], urls[2], None)

        lib.test_upload_ok(0, -1, urls[0], None)
    finally:
        for h in servers:
            assert 0 == len(h.expectations)
            h.shutdown()
        for s in services:
            s.join()


if __name__ == "__main__":
    LIB = cdll.LoadLibrary(sys.argv[1] + "/liboiohttp_test.so")
    LIB.setup()
    test_ok(LIB)
