#!/usr/bin/env python3
"""Tiny TCP echo server for depcheck's socket test (VITA-3): python3 tools/vita/depcheck/echo_server.py [port]."""
import socketserver
import sys


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        print("connection from", self.client_address, flush=True)
        while data := self.request.recv(1024):
            self.request.sendall(data)


socketserver.ThreadingTCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(("0.0.0.0", int(sys.argv[1]) if len(sys.argv) > 1 else 9998), Echo) as srv:
    srv.serve_forever()
