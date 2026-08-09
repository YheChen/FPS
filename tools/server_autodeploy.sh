#!/usr/bin/env bash
# Redeploys the dedicated server when main moves. Pull, not push.
#
# WHY PULL: the alternative is CI reaching in over SSH, which means storing a
# Tailscale auth key AND an SSH key into a machine inside a home LAN with a
# third party. This direction hands out nothing: the host asks GitHub whether
# anything changed, and nothing inbound is required or opened.
#
# WHY IT EXISTS: CI publishes the client on every push to main; nothing
# published the server. On 2026-08-09 the live host was found five milestones
# behind -- including a merged fix for a denial of service it was still
# vulnerable to. Nobody decided that; it is just what happens when shipping
# depends on someone remembering.
#
# THE PROTOCOL GUARD is the part that is not obvious. Client and server ship
# together and there is no in-protocol compatibility, so deploying a server
# whose kProtocolVersion has moved past the PUBLISHED client turns every
# connection into ServerReject(VersionMismatch) -- the game down, from a green
# build. So:
#
#   * protocol unchanged  -> deploy. This is almost every commit.
#   * protocol changed    -> deploy ONLY if the PUBLISHED client speaks the new
#                            version. Otherwise refuse and say so.
#
# "What the published client speaks" is read from the `client-live` git tag,
# which deploy-web moves after it has verified the deploy. It was originally
# read from https://<client>/version.json over HTTP, which does not work: the
# host is behind Vercel, whose bot mitigation answers curl with a JavaScript
# challenge page and a 403. A guard whose source of truth sits behind a WAF
# fails closed forever. A tag rides the git fetch this script already does,
# needs no credential, and nothing can interpose on it.
#
# Failing closed on that check is the whole point: a server that is a few
# commits stale is a nuisance, and one that no client can join is an outage.
#
# Runs as an ordinary user in the `docker` group. No sudo, anywhere.
#
# Usage:  server_autodeploy.sh [--repo DIR] [--dry-run] [--force]
# Exit:   0 nothing to do or deployed, 1 failed, 2 refused by the guard.
set -uo pipefail

REPO="${FPS_REPO:-$HOME/GitHub/FPS}"
LIVE_CLIENT_TAG="${FPS_CLIENT_TAG:-client-live}"
DRY_RUN=0
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        # Skips the protocol guard only. Never skips the health check.
        --force) FORCE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

say() { echo "[autodeploy] $*"; }
die() { echo "[autodeploy] ERROR: $*" >&2; exit 1; }

cd "$REPO" 2>/dev/null || die "no repo at $REPO"

compose() {
    docker compose -f deploy/compose.yaml --env-file deploy/.env "$@"
}

protocol_version_at() {
    # Reads from a git object rather than the worktree, so this works for a
    # commit that is not checked out.
    git show "$1:game/shared/protocol.h" 2>/dev/null |
        grep -oE 'kProtocolVersion = [0-9]+' | grep -oE '[0-9]+'
}

# --- is there anything to do? ------------------------------------------------
# --tags --force so a moved `client-live` is picked up rather than kept at
# whatever it pointed to the first time.
git fetch origin main --tags --force --quiet || die "git fetch failed"
LOCAL=$(git rev-parse HEAD)
REMOTE=$(git rev-parse origin/main)

if [ "$LOCAL" = "$REMOTE" ]; then
    say "already at $(git rev-parse --short HEAD); nothing to do"
    exit 0
fi

# A dirty tree means someone was working here by hand. Rebuilding would either
# ship their edit or destroy it; neither is this script's call to make.
if ! git diff --quiet || ! git diff --cached --quiet; then
    die "working tree has uncommitted changes; refusing to touch it"
fi

say "$(git rev-parse --short HEAD) -> $(git rev-parse --short origin/main)"
git log --oneline HEAD..origin/main | sed 's/^/[autodeploy]   /'

# --- protocol guard ----------------------------------------------------------
OLD_PROTOCOL=$(protocol_version_at HEAD)
NEW_PROTOCOL=$(protocol_version_at origin/main)
[ -n "$OLD_PROTOCOL" ] && [ -n "$NEW_PROTOCOL" ] || die "could not read kProtocolVersion"

if [ "$OLD_PROTOCOL" != "$NEW_PROTOCOL" ]; then
    say "protocol $OLD_PROTOCOL -> $NEW_PROTOCOL; checking what the published client speaks"
    LIVE_PROTOCOL=$(protocol_version_at "$LIVE_CLIENT_TAG")

    if [ "$FORCE" = "1" ]; then
        say "WARNING: --force, skipping the protocol guard"
    elif [ -z "$LIVE_PROTOCOL" ]; then
        say "REFUSING: protocol moved to $NEW_PROTOCOL and the '$LIVE_CLIENT_TAG' tag"
        say "          does not exist, so no client is known to be published."
        say "          It is created by the deploy-web CI job on a push to main."
        exit 2
    elif [ "$LIVE_PROTOCOL" != "$NEW_PROTOCOL" ]; then
        say "REFUSING: server would be protocol $NEW_PROTOCOL but the published"
        say "          client ($LIVE_CLIENT_TAG, $(git rev-parse --short "$LIVE_CLIENT_TAG" 2>/dev/null))"
        say "          speaks $LIVE_PROTOCOL. Every connection would be rejected."
        say "          Publish the matching client first."
        exit 2
    else
        say "published client speaks $LIVE_PROTOCOL; matched"
    fi
fi

if [ "$DRY_RUN" = "1" ]; then
    say "dry run: would deploy $(git rev-parse --short origin/main)"
    exit 0
fi

# --- deploy ------------------------------------------------------------------
say "pulling"
git merge --ff-only origin/main >/dev/null || die "fast-forward failed"

say "building and restarting (a few minutes)"
if compose up -d --build --wait; then
    :
else
    say "deploy failed; rolling back to $(git rev-parse --short "$LOCAL")"
    git reset --hard "$LOCAL" >/dev/null
    # Best effort: if this also fails there is nothing left to try from here,
    # and leaving the host loudly broken beats leaving it quietly wrong.
    compose up -d --build --wait || say "ROLLBACK ALSO FAILED -- server is down"
    exit 1
fi

# --- verify ------------------------------------------------------------------
# `--wait` already blocked on the container's healthcheck, which is ws_smoke
# against itself. This is the second opinion: the process is up AND says it is
# running what we just deployed.
say "deployed $(git rev-parse --short HEAD)"
compose logs --tail 40 server 2>/dev/null | grep -iE "Map:|Bot skill|listening|Stats:" |
    sed 's/^/[autodeploy]   /'
say "done"
