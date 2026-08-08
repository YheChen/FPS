#!/usr/bin/env python3
"""Keeps an A record pointed at this machine's current public IP.

Residential IPs move. Ours changed within an hour of being written down, so
the A record has to be maintained by something other than a person noticing.

Uses Porkbun's own /ping endpoint to learn the address rather than a
third-party echo service: it is the same view Porkbun has of us, it needs no
extra trust, and the credentials are already at hand.

Only writes when the record actually differs, so it is safe to run every few
minutes -- Porkbun rate-limits, and a no-op update still counts.

Environment (or --env-file, which is what the systemd unit uses):
    PORKBUN_API_KEY         pk1_...
    PORKBUN_API_SECRET_KEY  sk1_...
    PORKBUN_DOMAIN          yanzhenchen.ca
    PORKBUN_SUBDOMAIN       fps          ("" for the bare domain)
    PORKBUN_TTL             600

Exit codes: 0 changed or already correct, 1 failed.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.porkbun.com/api/json/v3"


def call(path, payload, timeout=20):
    request = urllib.request.Request(
        f"{API}/{path}",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = json.load(response)
    except urllib.error.HTTPError as exc:
        # Porkbun returns a JSON body with the reason even on 4xx, and that
        # message is far more useful than the status code.
        try:
            body = json.load(exc)
        except Exception:
            raise SystemExit(f"porkbun {path}: HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"porkbun {path}: {exc.reason}") from exc

    if body.get("status") != "SUCCESS":
        message = body.get("message", "no message")
        # The overwhelmingly common cause, and the error does not say it.
        if "not authorized" in message.lower() or "invalid" in message.lower():
            message += (
                "  (check API ACCESS is enabled for this domain: Porkbun > "
                "Domain Management > Details > API Access -- it is per-domain "
                "and off by default)"
            )
        raise SystemExit(f"porkbun {path}: {message}")
    return body


def load_env_file(path):
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip().strip("\"'"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", help="shell-style KEY=VALUE file")
    parser.add_argument(
        "--dry-run", action="store_true", help="report, change nothing"
    )
    args = parser.parse_args()

    if args.env_file:
        load_env_file(args.env_file)

    try:
        auth = {
            "apikey": os.environ["PORKBUN_API_KEY"],
            "secretapikey": os.environ["PORKBUN_API_SECRET_KEY"],
        }
        domain = os.environ["PORKBUN_DOMAIN"]
    except KeyError as exc:
        raise SystemExit(f"missing environment variable: {exc.args[0]}")

    subdomain = os.environ.get("PORKBUN_SUBDOMAIN", "")
    ttl = os.environ.get("PORKBUN_TTL", "600")
    fqdn = f"{subdomain}.{domain}" if subdomain else domain

    current_ip = call("ping", auth)["yourIp"]

    # retrieveByNameType 404s when the record does not exist yet, which is a
    # normal first run rather than a failure.
    try:
        records = call(f"dns/retrieveByNameType/{domain}/A/{subdomain}", auth)
        existing = records.get("records") or []
        record_ip = existing[0]["content"] if existing else None
    except SystemExit:
        record_ip = None

    if record_ip == current_ip:
        print(f"{fqdn} already points at {current_ip}")
        return 0

    was = record_ip or "nothing"
    if args.dry_run:
        print(f"would update {fqdn}: {was} -> {current_ip}")
        return 0

    call(
        f"dns/editByNameType/{domain}/A/{subdomain}",
        {**auth, "content": current_ip, "ttl": ttl},
    )
    print(f"updated {fqdn}: {was} -> {current_ip}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
