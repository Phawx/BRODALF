#!/usr/bin/env python3
"""A small stand-in for OneDrive (Microsoft Graph) and Dropbox, enough for
BRODALF's cloud code. It keeps files in memory and checks sign-in the way
the real services do: PKCE on the code exchange, bearer tokens on every
call, rotating refresh tokens for Microsoft.

Usage: mock_cloud.py COMMAND [ARGS...]
Starts the server on a free local port, runs COMMAND with
BRODALF_CLOUD_TEST_URL pointing at it, and exits with COMMAND's status.

Test controls (plain GET, no sign-in):
  /control/expire          every access token stops working (401)
  /control/throttle?n=N    the next N API calls get 429 with Retry-After: 1
  /control/stats           JSON counters
  /control/tree?p=PREFIX   JSON {path: size} of stored files
  /control/corrupt?p=PATH  flip the file's first byte, keeping its size
"""
import base64
import hashlib
import json
import os
import secrets
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
PENDING_CODES = {}  # code -> (provider, challenge, redirect_uri, client_id)
ACCESS = {}  # token -> provider
REFRESH = {}  # token -> provider
STATE = {"throttle": 0, "stats": {"refresh": 0, "throttled": 0, "unauthorized": 0, "chunks": 0}}
CLIENT_IDS = {"ms": "test-onedrive-app", "db": "test-dropbox-app"}


class Tree:
    """Files and folders by path, with ids (for Graph)."""

    def __init__(self):
        self.items = {"": {"id": "root", "folder": True}}
        self.by_id = {"root": ""}

    def _new_id(self):
        return secrets.token_hex(8)

    def get(self, path):
        return self.items.get(path.strip("/"))

    def mkdirs(self, path):
        path = path.strip("/")
        if not path:
            return self.items[""]
        parent = path.rsplit("/", 1)[0] if "/" in path else ""
        self.mkdirs(parent)
        if path not in self.items:
            i = self._new_id()
            self.items[path] = {"id": i, "folder": True}
            self.by_id[i] = path
        return self.items[path]

    def put(self, path, data):
        path = path.strip("/")
        parent = path.rsplit("/", 1)[0] if "/" in path else ""
        self.mkdirs(parent)
        old = self.items.get(path)
        i = old["id"] if old else self._new_id()
        self.items[path] = {"id": i, "folder": False, "data": bytes(data)}
        self.by_id[i] = path
        return self.items[path]

    def delete(self, path):
        path = path.strip("/")
        gone = [p for p in self.items if p == path or p.startswith(path + "/")]
        for p in gone:
            self.by_id.pop(self.items[p]["id"], None)
            del self.items[p]
        return bool(gone)

    def move(self, src, dst):
        src, dst = src.strip("/"), dst.strip("/")
        parent = dst.rsplit("/", 1)[0] if "/" in dst else ""
        self.mkdirs(parent)
        for p in sorted([p for p in self.items if p == src or p.startswith(src + "/")]):
            item = self.items.pop(p)
            np = dst + p[len(src):]
            self.items[np] = item
            self.by_id[item["id"]] = np


TREES = {"ms": Tree(), "db": Tree()}


def content_hash(data):
    return base64.b64encode(hashlib.sha1(data).digest()).decode()


def graph_item(path, item):
    out = {"id": item["id"], "name": path.rsplit("/", 1)[-1]}
    if item["folder"]:
        out["folder"] = {"childCount": 0}
    else:
        out["size"] = len(item["data"])
        out["file"] = {"hashes": {"quickXorHash": content_hash(item["data"])}}
    return out


def dropbox_meta(path, item):
    if item["folder"]:
        return {".tag": "folder", "name": path.rsplit("/", 1)[-1], "path_display": "/" + path}
    return {".tag": "file", "name": path.rsplit("/", 1)[-1], "path_display": "/" + path, "size": len(item["data"]),
            "content_hash": content_hash(item["data"]), "rev": secrets.token_hex(6)}


UPLOADS = {}  # session id -> {"tree", "path", "data", "size"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_CLOUD_VERBOSE"):
            sys.stderr.write("mock: " + (fmt % args) + "\n")

    # ---- plumbing ----
    def send(self, status, body=b"", ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def authorized(self, provider):
        auth = self.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else None
        with LOCK:
            ok = token in ACCESS and ACCESS[token] == provider
            if not ok:
                STATE["stats"]["unauthorized"] += 1
            elif STATE["throttle"] > 0:
                STATE["throttle"] -= 1
                STATE["stats"]["throttled"] += 1
                self.send(429, {"error": {"code": "tooManyRequests", "message": "slow down"}}, headers={"Retry-After": "1"})
                return False
        if not ok:
            self.send(401, {"error": {"code": "InvalidAuthenticationToken", "message": "token expired"}})
        return ok

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PUT(self):
        self.route("PUT")

    def do_PATCH(self):
        self.route("PATCH")

    def do_DELETE(self):
        self.route("DELETE")

    def route(self, method):
        u = urllib.parse.urlsplit(self.path)
        path, query = u.path, urllib.parse.parse_qs(u.query)
        try:
            if path.startswith("/control/"):
                return self.control(path[9:], query)
            if path in ("/ms/authorize", "/db/authorize"):
                return self.authorize(path[1:3], query)
            if path in ("/ms/token", "/db/token"):
                return self.token(path[1:3], self.body())
            if path.startswith("/graph/v1.0/"):
                return self.graph(method, path[len("/graph/v1.0"):], u.query)
            if path.startswith("/msupload/"):
                return self.graph_upload_chunk(method, path[len("/msupload/"):])
            if path.startswith("/msdownload/"):
                item_path = TREES["ms"].by_id.get(path[len("/msdownload/"):])
                item = TREES["ms"].get(item_path) if item_path is not None else None
                if not item:
                    return self.send(404, {})
                return self.send(200, item["data"], "application/octet-stream")
            if path.startswith("/dbapi/2/"):
                return self.dropbox_rpc(path[len("/dbapi/2/"):])
            if path.startswith("/dbcontent/2/"):
                return self.dropbox_content(path[len("/dbcontent/2/"):])
            self.send(404, {"error": "no route " + path})
        except Exception as e:  # keep the server alive and make the failure visible
            import traceback
            traceback.print_exc()
            self.send(500, {"error": {"message": str(e)}})

    # ---- test controls ----
    def control(self, what, q):
        with LOCK:
            if what == "expire":
                ACCESS.clear()
                return self.send(200, {})
            if what == "throttle":
                STATE["throttle"] = int(q.get("n", ["1"])[0])
                return self.send(200, {})
            if what == "stats":
                return self.send(200, STATE["stats"])
            if what == "tree":
                prov, prefix = q.get("provider", ["ms"])[0], q.get("p", [""])[0]
                t = TREES[prov]
                return self.send(200, {p: len(i["data"]) for p, i in t.items.items()
                                       if not i["folder"] and p.startswith(prefix)})
            if what == "corrupt":
                prov, p = q.get("provider", ["ms"])[0], q["p"][0]
                item = TREES[prov].get(p)
                if not item:
                    return self.send(404, {})
                d = bytearray(item["data"])
                d[0] ^= 0xFF
                item["data"] = bytes(d)
                return self.send(200, {})
        self.send(404, {})

    # ---- OAuth ----
    def authorize(self, prov, q):
        one = {k: v[0] for k, v in q.items()}
        if one.get("client_id") != CLIENT_IDS[prov] or one.get("code_challenge_method") != "S256":
            return self.send(400, "bad authorize request", "text/plain")
        if prov == "ms" and "offline_access" not in one.get("scope", ""):
            return self.send(400, "missing offline_access", "text/plain")
        if prov == "db" and one.get("token_access_type") != "offline":
            return self.send(400, "missing token_access_type", "text/plain")
        code = secrets.token_urlsafe(16)
        with LOCK:
            PENDING_CODES[code] = (prov, one["code_challenge"], one["redirect_uri"], one["client_id"])
        loc = one["redirect_uri"] + "?" + urllib.parse.urlencode({"code": code, "state": one.get("state", "")})
        self.send(302, "", "text/plain", {"Location": loc})

    def token(self, prov, raw):
        f = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
        with LOCK:
            if f.get("client_id") != CLIENT_IDS[prov]:
                return self.send(400, {"error": "invalid_client"})
            if f.get("grant_type") == "authorization_code":
                entry = PENDING_CODES.pop(f.get("code", ""), None)
                if not entry or entry[0] != prov or entry[2] != f.get("redirect_uri"):
                    return self.send(400, {"error": "invalid_grant", "error_description": "bad code"})
                digest = hashlib.sha256(f.get("code_verifier", "").encode()).digest()
                if base64.urlsafe_b64encode(digest).rstrip(b"=").decode() != entry[1]:
                    return self.send(400, {"error": "invalid_grant", "error_description": "PKCE mismatch"})
                refresh = secrets.token_urlsafe(24)
                REFRESH[refresh] = prov
            elif f.get("grant_type") == "refresh_token":
                old = f.get("refresh_token")
                if REFRESH.get(old) != prov:
                    return self.send(400, {"error": "invalid_grant", "error_description": "refresh token revoked"})
                STATE["stats"]["refresh"] += 1
                if prov == "ms":  # Microsoft rotates refresh tokens
                    del REFRESH[old]
                    refresh = secrets.token_urlsafe(24)
                    REFRESH[refresh] = prov
                else:
                    refresh = None
            else:
                return self.send(400, {"error": "unsupported_grant_type"})
            access = secrets.token_urlsafe(24)
            ACCESS[access] = prov
        out = {"access_token": access, "token_type": "bearer", "expires_in": 3600}
        if refresh:
            out["refresh_token"] = refresh
        self.send(200, out)

    # ---- Microsoft Graph ----
    def graph(self, method, path, raw_query):
        if not self.authorized("ms"):
            return
        t = TREES["ms"]
        if path == "/me" and method == "GET":
            return self.send(200, {"userPrincipalName": "tester@outlook.com", "displayName": "Tester"})
        if path == "/me/drive" and method == "GET":
            return self.send(200, {"quota": {"total": 5 * 2**40, "remaining": 5 * 2**40 - 1000, "used": 1000}})
        if path == "/me/drive/special/approot" and method == "GET":
            return self.send(200, graph_item("", t.items[""]))
        prefix = "/me/drive/special/approot:/"
        if path.startswith(prefix):
            rest = urllib.parse.unquote(path[len(prefix):])
            suffix = ""
            if rest.endswith(":/content"):
                rest, suffix = rest[:-9], "content"
            elif rest.endswith(":"):
                rest = rest[:-1]
            item = t.get(rest)
            if method == "GET":
                if not item:
                    return self.send(404, {"error": {"code": "itemNotFound", "message": "not found"}})
                if suffix == "content":
                    host = self.headers.get("Host")
                    return self.send(302, "", "text/plain", {"Location": "http://%s/msdownload/%s" % (host, item["id"])})
                return self.send(200, graph_item(rest, item))
            if method == "DELETE":
                if not item:
                    return self.send(404, {"error": {"code": "itemNotFound", "message": "not found"}})
                t.delete(rest)
                return self.send(204)
            if method == "PATCH":
                if not item:
                    return self.send(404, {"error": {"code": "itemNotFound", "message": "not found"}})
                b = json.loads(self.body())
                parent = t.by_id.get(b["parentReference"]["id"])
                if parent is None:
                    return self.send(404, {"error": {"code": "itemNotFound", "message": "no parent"}})
                dest = (parent + "/" if parent else "") + b["name"]
                if t.get(dest):
                    return self.send(409, {"error": {"code": "nameAlreadyExists", "message": "exists"}})
                t.move(rest, dest)
                return self.send(200, graph_item(dest, t.get(dest)))
        if path.startswith("/me/drive/items/"):
            rest = path[len("/me/drive/items/"):]
            if rest.endswith("/children") and method == "POST":
                parent = t.by_id.get(rest[:-9])
                if parent is None:
                    return self.send(404, {"error": {"code": "itemNotFound", "message": "no parent"}})
                b = json.loads(self.body())
                p = (parent + "/" if parent else "") + b["name"]
                if t.get(p):
                    return self.send(409, {"error": {"code": "nameAlreadyExists", "message": "exists"}})
                t.mkdirs(p)
                return self.send(201, graph_item(p, t.get(p)))
            # items/{parent-id}:/{name}:/content or :/createUploadSession
            pid, _, tail = rest.partition(":/")
            parent = t.by_id.get(pid)
            if parent is None:
                return self.send(404, {"error": {"code": "itemNotFound", "message": "no parent"}})
            name, _, action = tail.partition(":/")
            p = (parent + "/" if parent else "") + urllib.parse.unquote(name)
            if action == "content" and method == "PUT":
                data = self.body()
                if len(data) > 4 * 2**20:
                    return self.send(413, {"error": {"code": "tooLarge", "message": "use an upload session"}})
                item = t.put(p, data)
                return self.send(201, graph_item(p, item))
            if action == "createUploadSession" and method == "POST":
                self.body()
                sid = secrets.token_hex(8)
                with LOCK:
                    UPLOADS[sid] = {"tree": t, "path": p, "data": bytearray(), "size": None}
                host = self.headers.get("Host")
                return self.send(200, {"uploadUrl": "http://%s/msupload/%s" % (host, sid)})
        self.send(400, {"error": {"code": "invalidRequest", "message": "unsupported: %s %s" % (method, path)}})

    def graph_upload_chunk(self, method, sid):
        if self.headers.get("Authorization"):
            return self.send(401, {"error": {"code": "unauthenticated", "message": "upload URLs take no token"}})
        up = UPLOADS.get(sid)
        if not up:
            return self.send(404, {"error": {"code": "itemNotFound", "message": "no session"}})
        if method == "DELETE":
            UPLOADS.pop(sid, None)
            return self.send(204)
        rng = self.headers.get("Content-Range", "")  # bytes a-b/total
        a_b, total = rng[6:].split("/")
        a, b = (int(x) for x in a_b.split("-"))
        data = self.body()
        if a != len(up["data"]) or b - a + 1 != len(data) or len(data) % (320 * 1024) and b + 1 != int(total):
            return self.send(416, {"error": {"code": "invalidRange", "message": "bad range " + rng}})
        STATE["stats"]["chunks"] += 1
        up["data"] += data
        if len(up["data"]) < int(total):
            return self.send(202, {"nextExpectedRanges": ["%d-" % len(up["data"])]})
        item = up["tree"].put(up["path"], up["data"])
        UPLOADS.pop(sid, None)
        return self.send(201, graph_item(up["path"], item))

    # ---- Dropbox ----
    def db_not_found(self, where="path"):
        return self.send(409, {"error_summary": where + "/not_found/..", "error": {".tag": where}})

    def dropbox_rpc(self, endpoint):
        if not self.authorized("db"):
            return
        raw = self.body()
        if self.headers.get("Content-Type") != "application/json":
            return self.send(400, "Error in call: bad content type", "text/plain")
        args = json.loads(raw) if raw else None
        t = TREES["db"]
        if endpoint == "users/get_current_account":
            return self.send(200, {"email": "tester@example.com", "name": {"display_name": "Tester"}})
        if endpoint == "users/get_space_usage":
            return self.send(200, {"used": 1000, "allocation": {".tag": "individual", "allocated": 2 * 2**40}})
        if endpoint == "files/get_metadata":
            p = args["path"].strip("/")
            item = t.get(p)
            return self.send(200, dropbox_meta(p, item)) if item else self.db_not_found()
        if endpoint == "files/delete_v2":
            p = args["path"].strip("/")
            item = t.get(p)
            if not item:
                return self.db_not_found("path_lookup")
            meta = dropbox_meta(p, item)
            t.delete(p)
            return self.send(200, {"metadata": meta})
        if endpoint == "files/move_v2":
            src, dst = args["from_path"].strip("/"), args["to_path"].strip("/")
            if not t.get(src):
                return self.db_not_found("from_lookup")
            if t.get(dst):
                return self.send(409, {"error_summary": "to/conflict/file/..", "error": {".tag": "to"}})
            t.move(src, dst)
            return self.send(200, {"metadata": dropbox_meta(dst, t.get(dst))})
        self.send(400, "Unknown endpoint " + endpoint, "text/plain")

    def dropbox_content(self, endpoint):
        if not self.authorized("db"):
            return
        arg_raw = self.headers.get("Dropbox-API-Arg", "")
        try:
            arg_raw.encode("ascii")
        except UnicodeEncodeError:
            return self.send(400, "Dropbox-API-Arg must be ASCII", "text/plain")
        args = json.loads(arg_raw)
        data = self.body()
        t = TREES["db"]
        if endpoint == "files/download":
            p = args["path"].strip("/")
            item = t.get(p)
            if not item or item["folder"]:
                return self.db_not_found()
            return self.send(200, item["data"], "application/octet-stream",
                             {"Dropbox-API-Result": json.dumps(dropbox_meta(p, item))})
        if len(data) > 150 * 2**20:
            return self.send(400, "too big", "text/plain")
        if endpoint == "files/upload":
            p = args["path"].strip("/")
            if args.get("mode") != "overwrite" and t.get(p):
                return self.send(409, {"error_summary": "path/conflict/file/.."})
            return self.send(200, dropbox_meta(p, t.put(p, data)))
        if endpoint == "files/upload_session/start":
            sid = secrets.token_hex(8)
            UPLOADS[sid] = {"tree": t, "data": bytearray(data)}
            STATE["stats"]["chunks"] += 1
            return self.send(200, {"session_id": sid})
        if endpoint in ("files/upload_session/append_v2", "files/upload_session/finish"):
            cur = args["cursor"]
            up = UPLOADS.get(cur["session_id"])
            if not up:
                return self.send(409, {"error_summary": "lookup_failed/not_found/.."})
            if cur["offset"] != len(up["data"]):
                return self.send(409, {"error_summary": "lookup_failed/incorrect_offset/.."})
            up["data"] += data
            STATE["stats"]["chunks"] += 1
            if endpoint.endswith("append_v2"):
                return self.send(200, "null")
            UPLOADS.pop(cur["session_id"], None)
            p = args["commit"]["path"].strip("/")
            return self.send(200, dropbox_meta(p, t.put(p, up["data"])))
        self.send(400, "Unknown endpoint " + endpoint, "text/plain")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["BRODALF_CLOUD_TEST_URL"] = "http://127.0.0.1:%d" % server.server_address[1]
    env["BRODALF_ONEDRIVE_CLIENT_ID"] = CLIENT_IDS["ms"]
    env["BRODALF_DROPBOX_CLIENT_ID"] = CLIENT_IDS["db"]
    rc = subprocess.call(sys.argv[1:], env=env)
    server.shutdown()
    return rc


if __name__ == "__main__":
    sys.exit(main())
