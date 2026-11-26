#!/usr/bin/env python3
"""Control the running bridge's Spotify updates without calling Spotify."""

import argparse
import json
import os
from pathlib import Path
import urllib.error
import urllib.request

from spotify_bridge import load_env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("on", "off", "status"))
    parser.add_argument("--env-file", default=str(Path(__file__).with_name("spotify.env")))
    args = parser.parse_args()
    try:
        load_env(args.env_file)
        token = os.environ["PI_API_TOKEN"]
        port = int(os.environ.get("PI_PORT", "8765"))
        payload = None if args.action == "status" else json.dumps({"enabled": args.action == "on"}).encode()
        request = urllib.request.Request(
            "http://127.0.0.1:%d/polling" % port, data=payload,
            headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        # Stay on this Pi even if the shell has an HTTP proxy configured.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(request, timeout=35) as response:
            polling = json.load(response)["polling"]
        print("Spotify updates: " + ("ON" if polling["enabled"] else "OFF"))
        print("Display: " + ("connected" if polling["display_online"] else "no recent check-in"))
        print("Polling: " + ("active (subject to Spotify cooldown)" if polling["active"] else
                             "stopped manually" if not polling["enabled"] else "waiting for display"))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, "Could not control bridge (%s). Check that spotify-bridge.service is running.\n" %
                    type(error).__name__)


if __name__ == "__main__":
    main()
