#!/usr/bin/env python3
"""MW stub: replaces the OTT MW server P4 (Section 5.4.2; TST-603, IN-05, IN-06).

Answers the requests of IF-EXT-03 (Section 3.2) with the files given on the command line, sent as
they are (so that a test can also serve invalid JSON, IN-06):
  GET /api/v1/services        -> --services FILE
  GET /api/v1/services/<id>   -> --service <id>=FILE (repeatable)
  GET /api/v1/epg?...         -> --epg FILE (the same file for every query)
Anything else, and a route without a file, gets 404 with an error object. Listens on 127.0.0.1 only.

Usage (Python standard library only, NFR-11):
  mw_stub.py [--port 8080] --services services.json [--service svc-1=svc-1.json ...] [--epg epg.json]
Every request is printed on standard output with its status.
"""

import argparse
import http.server
import json
import sys
import time
import urllib.parse


def make_handler(routes):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, fmt, *args):  # one line per request, on standard output
            print('%s %s' % (time.strftime('%Y-%m-%dT%H:%M:%S'), fmt % args), flush=True)

        def send(self, status, body):
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def error(self, status, code, message):
            self.send(status, json.dumps({'error': {'code': code, 'message': message}}).encode())

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path in routes:
                with open(routes[path], 'rb') as f:
                    self.send(200, f.read())
            elif path.startswith('/api/v1/services/'):
                self.error(404, 'SERVICE_NOT_FOUND', 'unknown service')
            else:
                self.error(404, 'INVALID_REQUEST', 'unknown path')

        def do_other(self):
            length = int(self.headers.get('Content-Length') or 0)
            if length:
                self.rfile.read(length)
            self.error(400, 'INVALID_REQUEST', 'method not allowed')

        do_POST = do_PUT = do_DELETE = do_PATCH = do_other

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--services', required=True)
    parser.add_argument('--service', action='append', default=[], metavar='ID=FILE')
    parser.add_argument('--epg')
    args = parser.parse_args()
    routes = {'/api/v1/services': args.services}
    for item in args.service:
        service_id, _, path = item.partition('=')
        if not service_id or not path:
            parser.error('--service needs ID=FILE')
        routes['/api/v1/services/' + service_id] = path
    if args.epg:
        routes['/api/v1/epg'] = args.epg
    for path in routes.values():
        open(path, 'rb').close()  # a missing file stops the stub at once
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(routes))
    print('mw_stub listening on http://127.0.0.1:%d/api/v1' % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
