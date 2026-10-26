#!/usr/bin/env python3
"""Get a Spotify refresh token for the ESP32 controller.

Run on your laptop (Python 3 only, no packages needed):

    python3 tools/get_refresh_token.py

It asks for your app's Client ID and Client Secret, opens Spotify's login page,
catches the redirect on http://127.0.0.1:8888/callback, and prints the refresh
token to paste into include/secrets.h.

Your Spotify app must have that exact redirect URI added in the dashboard.
"""

import base64
import getpass
import http.server
import json
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing"
AUTHORIZE_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"


def wait_for_code(expected_state):
    """Serves one request on 127.0.0.1:8888 and returns the ?code= from it."""
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(url.query)
            if query.get("state", [None])[0] != expected_state:
                result["error"] = "state mismatch (try again)"
            elif "error" in query:
                result["error"] = query["error"][0]
            else:
                result["code"] = query.get("code", [None])[0]

            ok = "code" in result and result["code"]
            body = (
                "<h2>Done - you can close this tab and go back to the terminal.</h2>"
                if ok
                else "<h2>Spotify login failed: %s</h2>" % result.get("error", "no code")
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, *args):
            pass

    try:
        server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
    except OSError as e:
        sys.exit("Couldn't listen on 127.0.0.1:8888 (%s). Is something else using that port?" % e)
    with server:
        while not result:
            server.handle_request()

    if not result.get("code"):
        sys.exit("Login failed: %s" % result.get("error", "no code returned"))
    return result["code"]


def exchange_code(client_id, client_secret, code):
    data = urllib.parse.urlencode(
        {"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT_URI}
    ).encode()
    auth = base64.b64encode(("%s:%s" % (client_id, client_secret)).encode()).decode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=data,
        headers={
            "Authorization": "Basic " + auth,
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")
        sys.exit("Token exchange failed (HTTP %d): %s" % (e.code, detail))


def main():
    print("Spotify refresh token helper")
    print("Find these at developer.spotify.com/dashboard -> your app -> Settings.\n")
    client_id = input("Client ID: ").strip()
    client_secret = getpass.getpass("Client Secret (hidden): ").strip()
    if not client_id or not client_secret:
        sys.exit("Both Client ID and Client Secret are required.")

    state = secrets.token_urlsafe(16)
    url = AUTHORIZE_URL + "?" + urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPES,
            "state": state,
            "show_dialog": "true",
        }
    )

    print("\nOpening Spotify login in your browser. If it doesn't open, visit:\n")
    print(url + "\n")
    webbrowser.open(url)

    code = wait_for_code(state)
    tokens = exchange_code(client_id, client_secret, code)

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit("Spotify didn't return a refresh token: %s" % tokens)

    granted = set(tokens.get("scope", "").split())
    missing = set(SCOPES.split()) - granted
    if missing:
        print("Warning: these scopes were not granted: %s" % " ".join(sorted(missing)))

    print("\nSuccess. Put these in include/secrets.h:\n")
    print('#define SPOTIFY_CLIENT_ID "%s"' % client_id)
    print('#define SPOTIFY_CLIENT_SECRET "<your client secret>"')
    print('#define SPOTIFY_REFRESH_TOKEN "%s"' % refresh)
    print("\nKeep the refresh token private: it controls your Spotify playback.")


if __name__ == "__main__":
    main()
