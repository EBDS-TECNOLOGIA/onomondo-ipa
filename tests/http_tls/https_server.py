# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Server side of http_tls_test.sh: answers every POST with "pong", presenting the certificate chain given.
# usage: https_server.py CHAIN_PEM KEY_PEM PORT_FILE

import http.server
import ssl
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send_response(200)
        self.send_header("Content-Length", "4")
        self.end_headers()
        self.wfile.write(b"pong")

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # A client that rejects the certificate aborts the handshake; that is what the tests provoke.
        pass


server = Server(("127.0.0.1", 0), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[1], sys.argv[2])
server.socket = ctx.wrap_socket(server.socket, server_side=True)
with open(sys.argv[3], "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
