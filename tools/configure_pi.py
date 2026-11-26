#!/usr/bin/env python3
"""Prepare matching ESP32/Pi configuration without printing account credentials."""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import tempfile
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]


def private_write(path, text):
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_define(text, name):
    match = re.search(r'^\s*#define\s+' + name + r'\s+("(?:[^"\\]|\\.)*")', text, re.MULTILINE)
    if not match:
        return ""
    return json.loads(match[1])


def configure(server_url, secrets_path, env_path):
    parsed = urllib.parse.urlsplit(server_url)
    if (parsed.scheme != "http" or not parsed.hostname or parsed.username or parsed.password
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment):
        raise ValueError("Use the Pi's local HTTP address, such as http://10.0.0.248:8765")
    port = parsed.port or 80
    original = secrets_path.read_text()
    values = {name: read_define(original, name) for name in
              ("SPOTIFY_CLIENT_ID", "SPOTIFY_CLIENT_SECRET", "SPOTIFY_REFRESH_TOKEN")}
    missing = [name for name, value in values.items() if not value or value.startswith("your-")]
    if missing:
        raise ValueError("Fill these in include/secrets.h first: " + ", ".join(missing))
    token = read_define(original, "PI_API_TOKEN")
    if len(token) < 24 or token.startswith("your-") or not token.isascii():
        token = secrets.token_urlsafe(32)
    values.update(PI_API_TOKEN=token, PI_PORT=str(port), PI_BIND="0.0.0.0", SPOTIFY_POLL_INTERVAL="1")
    # Keep optional weather/polling settings when regenerating credentials.
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            key, separator, value = line.strip().partition("=")
            if separator and (key.startswith("WEATHER_") or key == "SPOTIFY_POLL_INTERVAL") and key.replace("_", "").isalnum():
                parts = shlex.split(value, comments=True)
                if len(parts) != 1:
                    raise ValueError("Invalid environment value for " + key)
                values[key] = parts[0]
    updates = {"PI_SERVER_URL": server_url.rstrip("/"), "PI_API_TOKEN": token}
    updated = original
    for name, value in updates.items():
        definition = "#define " + name + " " + json.dumps(value)
        pattern = r"^\s*#define\s+" + name + r"\s+[^\n]*"
        if re.search(pattern, updated, re.MULTILINE):
            updated = re.sub(pattern, lambda match: definition, updated, flags=re.MULTILINE)
        else:
            updated = updated.rstrip() + "\n" + definition + "\n"
    env_text = "# Private: generated from include/secrets.h. Do not commit or share.\n"
    env_text += "".join(name + "=" + shlex.quote(value) + "\n" for name, value in values.items())
    private_write(env_path, env_text)
    private_write(secrets_path, updated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Pi LAN address, e.g. http://10.0.0.248:8765")
    args = parser.parse_args()
    try:
        configure(args.url, ROOT / "include/secrets.h", ROOT / "pi/spotify.env")
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
    print("Updated include/secrets.h and pi/spotify.env (credentials hidden).")
    print("Copy pi/ to your Pi, then build/upload the controller-pi environment.")


if __name__ == "__main__":
    main()
