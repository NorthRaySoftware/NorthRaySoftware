import json, hashlib, base64, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
S = {"approved": False, "at": None, "rt": None, "gat": None, "grt": None, "challenge": None, "revoked": [], "deny": False, "n": 0}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def form(self):
        n = int(self.headers.get("Content-Length") or 0); return dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))
    def new_twitch(self):
        S["n"] += 1; S["at"] = "tat%d" % S["n"]; S["rt"] = "trt%d" % S["n"]
        return {"access_token": S["at"], "refresh_token": S["rt"], "expires_in": 14400, "scope": ["channel:read:stream_key"], "token_type": "bearer"}
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query)); auth = self.headers.get("Authorization","")
        if u.path == "/approve": S["approved"] = True; return self.send(200, {})
        if u.path == "/expire": S["at"] = None; S["gat"] = None; return self.send(200, {})
        if u.path == "/state": return self.send(200, S)
        if u.path == "/setexpires": S["expires"] = int(q["s"]); return self.send(200, {})
        if u.path == "/twitch/validate":
            if S["at"] and auth in ("OAuth " + S["at"], "Bearer " + S["at"]): return self.send(200, {"client_id":"tw-client","login":"cjtest","scopes":["channel:read:stream_key"],"user_id":"42","expires_in":14000})
            return self.send(401, {"status":401,"message":"invalid access token"})
        if u.path == "/twitch/key":
            if not (S["at"] and auth == "Bearer " + S["at"] and self.headers.get("Client-Id") == "tw-client"): return self.send(401, {"error":"Unauthorized","status":401,"message":"Invalid OAuth token"})
            if q.get("broadcaster_id") != "42": return self.send(401, {"error":"Unauthorized","status":401,"message":"broadcaster mismatch"})
            return self.send(200, {"data":[{"stream_key":"live_42_mockkey"}]})
        if u.path == "/yt/channels":
            if S["gat"] and auth == "Bearer " + S["gat"]: return self.send(200, {"items":[{"id":"UC1","snippet":{"title":"CJ Test Channel"}}]})
            return self.send(401, {"error":{"code":401,"message":"Invalid Credentials"}})
        if u.path == "/yt/streams":
            if not (S["gat"] and auth == "Bearer " + S["gat"]): return self.send(401, {"error":{"code":401,"message":"Invalid Credentials"}})
            if q.get("mine") != "true" or "cdn" not in q.get("part",""): return self.send(400, {"error":{"code":400,"message":"bad request"}})
            return self.send(200, {"items":[{"snippet":{"title":"Default stream key","isDefaultStream":True},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"streamName":"yt-mock-key","ingestionAddress":"rtmp://a.rtmp.youtube.com/live2","rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2"}},"contentDetails":{"isReusable":True}}]})
        self.send(404, {})
    def do_POST(self):
        u = urllib.parse.urlparse(self.path); f = self.form()
        if u.path == "/expect": S["challenge"] = f["challenge"]; return self.send(200, {})
        if u.path == "/twitch/device":
            if f.get("client_id") != "tw-client" or f.get("scopes") != "channel:read:stream_key": return self.send(400, {"status":400,"message":"invalid client"})
            S["approved"] = False
            return self.send(200, {"device_code":"dc1","expires_in":int(S.get("expires",600)),"interval":1,"user_code":"WXYZ1234","verification_uri":"https://www.twitch.tv/activate?public=true&device-code=WXYZ1234"})
        if u.path == "/twitch/token":
            if f.get("client_id") != "tw-client" or "client_secret" in f: return self.send(400, {"status":400,"message":"invalid client"})
            if f.get("grant_type") == "urn:ietf:params:oauth:grant-type:device_code":
                if f.get("device_code") != "dc1": return self.send(400, {"status":400,"message":"invalid device code"})
                if not S["approved"]: return self.send(400, {"status":400,"message":"authorization_pending"})
                S["approved"] = False
                return self.send(200, self.new_twitch())
            if f.get("grant_type") == "refresh_token":
                if f.get("refresh_token") != S["rt"] or not S["rt"]: return self.send(400, {"status":400,"message":"Invalid refresh token"})
                return self.send(200, self.new_twitch())
            return self.send(400, {"status":400,"message":"bad grant"})
        if u.path == "/twitch/revoke":
            S["revoked"].append("twitch:" + f.get("token","")); 
            if f.get("token") == S["at"]: S["at"] = None; S["rt"] = None
            return self.send(200, {})
        if u.path == "/google/token":
            if f.get("client_id") != "g-client" or f.get("client_secret") != "g-secret": return self.send(401, {"error":"invalid_client"})
            if f.get("grant_type") == "authorization_code":
                v = f.get("code_verifier","")
                ch = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b"=").decode()
                if f.get("code") != "gcode1" or ch != S["challenge"] or not f.get("redirect_uri","").startswith("http://127.0.0.1:"): return self.send(400, {"error":"invalid_grant","error_description":"Bad code or verifier"})
                S["gat"], S["grt"] = "gat1", "grt1"
                return self.send(200, {"access_token":"gat1","refresh_token":"grt1","expires_in":3599,"scope":"https://www.googleapis.com/auth/youtube.readonly","token_type":"Bearer"})
            if f.get("grant_type") == "refresh_token":
                if f.get("refresh_token") != S["grt"] or not S["grt"]: return self.send(400, {"error":"invalid_grant"})
                S["gat"] = "gat2"; return self.send(200, {"access_token":"gat2","expires_in":3599,"token_type":"Bearer"})
        if u.path == "/google/revoke":
            S["revoked"].append("google:" + f.get("token","")); S["grt"] = None; S["gat"] = None; return self.send(200, {})
        self.send(404, {})
ThreadingHTTPServer(("127.0.0.1", 18088), H).serve_forever()
