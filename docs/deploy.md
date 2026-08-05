# Deployment

Two pieces (see [ADR 0005](decisions/0005-web-client.md)):

- **Client** → static WebAssembly on **Vercel**.
- **Server** → `fps_server` on a **ThinkPad T14 at home** (Fedora), behind
  **Caddy** terminating TLS so the browser can reach it over `wss://`. Both run
  as a Docker Compose stack; a native systemd install is documented as the
  alternative.

```
browser ──https──▶ Vercel (static .wasm/.js/.data)
   │
   └──wss://fps.yanzhenchen.ca──▶ Caddy (TLS) ──ws://server:7778──▶ fps_server
                                  └───────── docker compose ─────────┘
```

Why split it this way rather than serve both from the T14: Vercel gives the
page HTTPS and a CDN for free, so the T14 needs exactly **one** inbound port.
It also fails cleanly — if the T14 is off, the page still loads and reports a
connection error instead of not loading at all.

Why the proxy: a page served over HTTPS may only open secure (`wss://`)
WebSockets, and a certificate authority needs a domain, not a bare IP. Caddy
owns the certificate and keeps the C++ server as plain `ws://` on localhost.

**Bandwidth is not the constraint.** A snapshot is `10 + 34×players` bytes
([protocol.cpp](../game/shared/protocol.cpp)); with 8 players, WebSocket
framing, TLS and TCP/IP that is ~348 B, sent to each player 20 times a second
— about **0.45 Mbit/s upstream** at a full server. Any residential upload has
an order of magnitude of headroom. What *can* stop this is reachability, which
is why that comes first.

---

## 0. Three checks before anything else

Run these on the T14. Each one decides part of the setup below.

```sh
curl -s ifconfig.me; echo          # public IP as the internet sees it
cat /etc/os-release | head -2      # distro and version
dig +short NS yanzhenchen.ca       # who serves DNS
```

**Is the public IP the same as your router's WAN IP?** (Check the router's
status page.)

| Result | What it means | Which path below |
|---|---|---|
| Same | You have a real public IP; port forwarding will work | §3 **A**, **B**, or **D** |
| Different | You are behind **CGNAT** — no port forwarding can ever work | §3 **C** or **D** |

**D (Tailscale Funnel) works either way and needs nothing forwarded**, so if
this check is inconvenient to run, start there and come back to it only if you
want your own domain.

**Distro:** the build needs **CMake ≥ 3.25** and a **C++23** compiler. Current
Fedora ships CMake **4.3** and GCC **16.1** (both verified in CI); Ubuntu 24.04
has CMake 3.28 and GCC 13, and Debian 13 is fine too. Ubuntu 22.04 is **not** —
CMake 3.22 and GCC 11 are both too old, and you would be building toolchains
before the game. Check with `cmake --version && g++ --version`.

CMake 4 is worth a note because it *removed* compatibility with projects
declaring `cmake_minimum_required` below 3.5, which some transitive
dependencies still do. The server build is unaffected. If you ever enable
`FPS_ENABLE_WEBRTC` on this machine, `third_party/CMakeLists.txt` already
scopes a `CMAKE_POLICY_VERSION_MINIMUM` around the one dependency that needs
it.

Commands below are given for **Fedora** (`dnf`, `firewalld`, SELinux) with the
Debian/Ubuntu equivalent alongside. The `deploy-build` CI job builds the server
in both a `fedora` and an `ubuntu:24.04` container, so neither set is guesswork.

**DNS:** `yanzhenchen.ca` answers with `*.ns.porkbun.com`, so the zone lives
at **Porkbun**. That decides two things: the DNS-01 path (§3 B) uses Porkbun's
API module, not Cloudflare's, and dynamic DNS (§4) uses Porkbun's API too.
Porkbun's API has to be enabled per domain in their panel before either works.

Whoever serves your zone, the same rule holds: the DNS-01 module must match
them, and there is no module that works for "whatever provider". Check with
`dig +short NS <domain>` rather than assuming from where you bought it —
registrar and DNS host are often different.

---

## 1. Run the server — Docker (recommended)

The whole stack, server plus TLS proxy, is two files in `deploy/`. This is the
path to prefer: reproducible, survives reboot, and the image carries a health
check that speaks the real game protocol rather than just pinging a port.

```sh
git clone https://github.com/YheChen/FPS.git ~/fps
cd ~/fps
cp deploy/.env.example deploy/.env
$EDITOR deploy/.env                      # FPS_DOMAIN at minimum

sudo systemctl enable --now docker       # so the stack comes back after reboot
docker compose -f deploy/compose.yaml --env-file deploy/.env up -d --build
```

Check it:

```sh
docker compose -f deploy/compose.yaml ps          # server should be "healthy"
docker compose -f deploy/compose.yaml logs -f server
```

The server container is `read_only`, drops all capabilities, sets
`no-new-privileges`, and is capped at 512 MB. It is **not** published to the
host — only Caddy reaches it, over the compose network — so there is no plain
`ws://` listener on your LAN.

Two Fedora-specific notes. Bind mounts in `compose.yaml` carry `:z` because
SELinux is enforcing; without it the container gets a permission denial whose
message does not mention SELinux. And the certificate lives in a **named
volume**, so `docker compose down` does not throw it away and re-hit Let's
Encrypt rate limits on the next start.

Then skip to §3 for TLS — Caddy is already running, it just needs a reachable
domain.

<details>
<summary><b>Alternative: build and run natively (no Docker)</b></summary>

Everything from here to §3 is the non-Docker path. It is fine, and it is what
the `deploy-build` CI job exercises, but you have Docker and the compose stack
is less to get wrong.

### Build

```sh
# Fedora
sudo dnf install -y git cmake ninja-build gcc-c++ make ca-certificates

# Debian / Ubuntu
sudo apt update && sudo apt install -y git cmake ninja-build build-essential ca-certificates
```

```sh
sudo git clone https://github.com/YheChen/FPS.git /opt/fps
cd /opt/fps
cmake --preset release -DFPS_BUILD_CLIENT=OFF -DFPS_BUILD_TESTS=OFF
cmake --build --preset release --target fps_server --parallel
```

**`FPS_BUILD_CLIENT=OFF` is not optional on a headless host.** Without it the
*configure* step fails — not the build. SDL3's CMake hard-errors with "could
not find X11 or Wayland development libraries" whether or not anything links
it, so simply not building `fps_client` is not enough. The flag skips SDL3,
ImGui, glad and miniaudio entirely: they are never fetched, so the server build
is also markedly faster and needs no graphics packages at all.

(This guide previously said no graphics packages were needed and gave a command
without the flag. That command could not have worked. The `deploy-build` CI job
exists now so the claim is checked rather than asserted: bare `fedora` and
`ubuntu:24.04` containers, exactly the install lines above, `fps_server` only,
then start it — and it asserts SDL/ImGui/miniaudio were never fetched.)

Sanity check it before wiring anything up:

```sh
./build/release/game/fps_server --ws-port 7778 --no-enet --bots 2 --run-seconds 10
```

You should see the map load, a tick rate near 60/s, and two bots in the player
count.

### Run it as a systemd service

```sh
sudo useradd --system --home /opt/fps --shell /usr/sbin/nologin fps
sudo chown -R fps:fps /opt/fps
sudo cp deploy/fps-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now fps-server
systemctl status fps-server      # active, listening on ws 7778
```

The unit runs `--ws-port 7778` for browser players and keeps ENet on UDP 7777
for native ones. **For a home server, prefer browser-only**: add `--no-enet`
to `ExecStart` and you have one fewer port to forward and one fewer listener
exposed.

</details>

## 2. Keep the laptop awake

This applies either way, Docker or not. A closed lid suspends the machine on a
desktop install — Fedora Workstation included — and that takes the server with
it:

```sh
sudo tee /etc/systemd/logind.conf.d/99-fps.conf <<'EOF'
[Login]
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore
EOF
sudo systemctl restart systemd-logind
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

Fedora Workstation also has GNOME's own idle-suspend, which is separate from
logind and will still put the machine to sleep:

```sh
gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing'
```

Leave it plugged in. Charge thresholds are worth having so a permanently
plugged-in battery is not held at 100% — check what is currently set:

```sh
cat /sys/class/power_supply/BAT0/charge_control_{start,end}_threshold
```

20/80 is a good pair for a machine that mostly lives on AC.

---

## 3. TLS — pick one path

On the Docker path, Caddy is already running in the compose stack; A and B are
about how it gets a certificate, and the changes go in
`deploy/Caddyfile.docker` and `deploy/.env`. On the native path, install Caddy
as shown. **D needs neither** — it replaces Caddy entirely.

| Path | Needs inbound | Works behind CGNAT | Your own domain |
|---|---|---|---|
| **A** HTTP-01 | 80 + 443 | no | yes |
| **B** DNS-01 via Porkbun | 443 | no | yes |
| **C** Cloudflare Tunnel | nothing | **yes** | yes |
| **D** Tailscale Funnel | nothing | **yes** | no — a `ts.net` name |

**Start with D if you just want it working.** Tailscale is already installed
on this machine, the certificate is automatic, nothing is forwarded, and it
does not care whether you are behind CGNAT. The cost is the hostname: players
get `https://<machine>.<tailnet>.ts.net` rather than `fps.yanzhenchen.ca`. For
a deathmatch you share a link to, that is usually a fine trade — and moving to
A/B/C later is a config change, not a rebuild, as long as you rebuild the web
client with the new `FPS_WEB_SERVER_URL`.

### A. HTTP-01 (public IP, port 80 reachable)

The simple case — stock Caddy from the distro:

```sh
# Fedora: caddy is in the official repositories
sudo dnf install -y caddy

# Debian / Ubuntu: Caddy's own repo
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy
```

```sh
sudo cp /opt/fps/deploy/Caddyfile /etc/caddy/Caddyfile   # edit the domain first
sudo systemctl enable --now caddy
sudo systemctl reload caddy
```

Forward **TCP 80 and 443** on the router to the T14 (§4).

Many residential ISPs block inbound 80. If Caddy's log shows the ACME
challenge timing out, that is what happened — switch to **B**.

### B. DNS-01 via Porkbun (port 80 blocked, or you just prefer it)

Caddy proves domain control by writing a DNS record instead of answering on
port 80, so **only 443 ever needs to be open**.

The provider has to match wherever the zone actually lives. `yanzhenchen.ca`
is on **Porkbun** (`dig +short NS yanzhenchen.ca` → `*.ns.porkbun.com`), so
that is the module to build. Stock Caddy has no DNS modules compiled in at
all:

```sh
sudo dnf install -y golang          # Debian/Ubuntu: sudo apt install -y golang-go
go install github.com/caddyserver/xcaddy/cmd/xcaddy@latest
~/go/bin/xcaddy build --with github.com/caddy-dns/porkbun
sudo install -m 0755 ./caddy /usr/bin/caddy      # replaces the packaged binary
sudo restorecon -v /usr/bin/caddy                # Fedora: relabel for SELinux
caddy list-modules | grep dns.providers.porkbun
```

Take a recent version of the module: Porkbun moved their API hostname, and
builds before `v0.20` stopped working when the old endpoint was retired.

The `restorecon` matters on Fedora: overwriting a packaged binary can leave it
with the wrong SELinux label, and the failure mode is a permission denial that
looks nothing like a labelling problem.

Porkbun issues a **pair** of credentials, not a single token — and API access
must be switched on **per domain** in their panel (Domain Management →
Details → API Access). Miss that toggle and the keys are valid while the zone
still refuses updates, which reads exactly like a wrong key. Get them from
Account → API Access, then keep them out of the Caddyfile:

```sh
sudo mkdir -p /etc/systemd/system/caddy.service.d
sudo tee /etc/systemd/system/caddy.service.d/10-porkbun.conf <<'EOF'
[Service]
Environment=PORKBUN_API_KEY=pk1_...
Environment=PORKBUN_API_SECRET_KEY=sk1_...
EOF
sudo chmod 600 /etc/systemd/system/caddy.service.d/10-porkbun.conf
sudo systemctl daemon-reload
```

Then use the DNS-01 stanza in [`deploy/Caddyfile`](../deploy/Caddyfile) —
uncomment the `tls` block — and `sudo systemctl restart caddy`.

(On the Docker path the same block lives in
[`deploy/Caddyfile.docker`](../deploy/Caddyfile.docker), the credentials go in
`deploy/.env`, and the `caddy:2-alpine` image needs rebuilding with the module
— the Caddyfile comment has the three-line Dockerfile for it.)

Forward **TCP 443** only.

### C. Cloudflare Tunnel (behind CGNAT)

If your public IP differs from the router's WAN IP, no inbound port can reach
you. A tunnel dials *out* from the T14 and Cloudflare accepts connections on
its edge, so **nothing needs forwarding and Caddy is not needed at all** —
Cloudflare terminates TLS.

**This one has a prerequisite the others do not.** Cloudflare will only route
a hostname in a zone it serves, and `yanzhenchen.ca` is on Porkbun — so you
would first have to move the nameservers to Cloudflare (free, but it is a
change to your whole domain, not just this subdomain, and it takes time to
propagate). If you are behind CGNAT and do not want to move DNS, **path D is
the same idea without that cost**.

```sh
# cloudflared: Cloudflare publishes an RPM repo for Fedora and a deb repo for
# Debian/Ubuntu -- see their docs for the current signing key.
sudo dnf install -y cloudflared
cloudflared tunnel login
cloudflared tunnel create fps
cloudflared tunnel route dns fps fps.yanzhenchen.ca
```

Point the tunnel at the plain WebSocket server:

```yaml
# /etc/cloudflared/config.yml
tunnel: fps
credentials-file: /root/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: fps.yanzhenchen.ca
    service: ws://localhost:7778
  - service: http_status:404
```

```sh
sudo cloudflared service install
sudo systemctl enable --now cloudflared
```

Cloudflare proxies WebSockets on the free plan. Note it adds a hop through
their edge, so expect a little more latency than a direct connection — and
that free-plan tunnels are not a supported path for high-volume game traffic,
though 0.45 Mbit/s is nowhere near any threshold that matters.

### D. Tailscale Funnel (nothing inbound, no Caddy, no domain)

Tailscale is already on this machine. Funnel publishes one local port to the
public internet over Tailscale's edge, with a certificate it manages, on a
`ts.net` hostname. Nothing is forwarded and CGNAT is irrelevant.

Funnel has to be allowed in the tailnet policy first (admin console → Access
Controls → `nodeAttrs` with `funnel`, and HTTPS certificates enabled). Then, on
the T14:

```sh
tailscale serve --bg --https=443 http://127.0.0.1:7778   # only if the port is on the host
tailscale funnel --bg 443
tailscale funnel status                                   # prints the public URL
```

On the **Docker** path the server is deliberately not published to the host, so
Funnel has nothing to point at. Either publish it to loopback only — add
`ports: ["127.0.0.1:7778:7778"]` to the `server` service — or point Funnel at
Caddy instead and let it keep terminating TLS. Publishing to `127.0.0.1` is the
simpler of the two, and it stays off the LAN.

Then verify before you trust it, because Funnel is an HTTPS proxy and the
question that matters is whether it carries the WebSocket upgrade:

```sh
python3 tools/ws_smoke.py <machine>.<tailnet>.ts.net 443
```

A `ServerWelcome` back means the whole chain works. Build the web client with
that hostname as `FPS_WEB_SERVER_URL` (§5) and you are done — no §4 at all.

---

## 4. Router, firewall, and dynamic DNS

**Port forward** (paths A and B only) on the router to the T14's LAN address — give it a DHCP
reservation first, or the forward breaks the next time it renews:

| Port | Needed for |
|---|---|
| TCP 443 | `wss://` (all paths except C) |
| TCP 80 | ACME HTTP-01 only (path A) |
| UDP 7777 | native ENet players only — skip it if you run `--no-enet` |

**Host firewall.** Fedora runs `firewalld` and blocks inbound by default, so
this step is not optional there:

```sh
# Fedora
sudo firewall-cmd --permanent --add-service=https
sudo firewall-cmd --permanent --add-service=http     # path A only
sudo firewall-cmd --permanent --add-port=7777/udp    # native players only
sudo firewall-cmd --reload
sudo firewall-cmd --list-all                         # confirm

# Debian / Ubuntu, if ufw is active
sudo ufw allow 443/tcp
sudo ufw allow 80/tcp      # path A only
sudo ufw allow 7777/udp    # native players only
```

**DNS.** Paths C and D need none of this — the tunnel keeps its own routing.
For A and B, set the record up at Porkbun as follows.

*Credentials* (Account → [API Access](https://porkbun.com/account/api)). Porkbun
issues a **pair**: an API key `pk1_…` and a secret `sk1_…`, and the secret is
shown exactly once.

Then the step that is easy to miss and hard to diagnose: **API access is
per-domain and off by default.** Domain Management → `yanzhenchen.ca` →
Details → toggle **API ACCESS** on. Without it the keys are perfectly valid and
every write to this zone is refused, which presents as a wrong-key error.

Check both at once — this endpoint also reports your public IP, so it doubles
as the value the `A` record needs:

```sh
curl -sX POST https://api.porkbun.com/api/json/v3/ping \
  -H 'Content-Type: application/json' \
  -d '{"apikey":"pk1_...","secretapikey":"sk1_..."}'
```

`{"status":"SUCCESS","yourIp":"..."}` means keys and toggle are both good. Run
it **from the server**, or `yourIp` is whatever network you happened to be on.

*The record.* Domain Management → `yanzhenchen.ca` → DNS → add:

| Type | Host | Answer | TTL |
|---|---|---|---|
| `A` | `fps` | the `yourIp` from above | `600` |

Host `fps` produces `fps.yanzhenchen.ca`. TTL 600 rather than the default,
because a residential IP rotates and a stale record should expire quickly.
Confirm with `dig +short fps.yanzhenchen.ca`.

*Keeping it current.* Residential IPs change, so automate the update: reuse the
same key/secret with `ddclient` (it has a Porkbun provider) or a cron one-liner
against `api.porkbun.com/api/json/v3/dns/editByNameType`.

Keep the pair in `deploy/.env` (gitignored) or the systemd drop-in from §3 B —
never in the Caddyfile, which is world-readable in `/etc/caddy`.

**Verify:**

```sh
curl -I https://fps.yanzhenchen.ca     # valid cert, a Caddy or Cloudflare response
```

### If something is denied for no visible reason (Fedora)

SELinux is enforcing by default, and its denials rarely name themselves — a
service simply fails to bind, read, or connect. Before assuming a config error:

```sh
sudo ausearch -m AVC -ts recent          # anything denied in the last few minutes
```

Nothing here should need a policy change: the service runs from `/opt` as an
ordinary unconfined systemd unit, and Caddy from `dnf` ships with policy for
binding 80/443. The two things that do bite are a custom `caddy` binary with
the wrong label (`restorecon`, above) and a non-standard port — if you move the
game server off 7778, `semanage port -a` may be needed before Caddy can proxy
to it. **Do not `setenforce 0` to make a problem go away** on a host you are
deliberately exposing to the internet; read the denial instead.

---

## 5. Client on Vercel

Build the WASM client with the production server URL baked into its menu
default, then deploy the static output.

```sh
source ~/emsdk/emsdk_env.sh
FPS_WEB_SERVER_URL=wss://fps.yanzhenchen.ca scripts/build_web.sh
# outputs build/web/game/{fps_client.html,.js,.wasm,.data} + vercel.json

npm i -g vercel            # once
cd build/web/game
vercel --prod              # first run links/creates the project
```

Vercel serves `.wasm` as `application/wasm` automatically; `vercel.json` maps
`/` to the client and long-caches the immutable `.wasm/.data/.js`. The result
is a `https://<project>.vercel.app` URL (or attach a custom domain in the
dashboard). Opening it connects straight to the T14 — no download, no IP to
type.

Rebuild after changes by re-running the two commands above. The `.data` bundle
embeds `assets/`, so rebuild whenever assets change.

## 6. End-to-end check

Before sharing the URL, prove the whole chain rather than each piece:

```sh
# On the T14: is the game protocol really reachable through TLS?
python3 tools/ws_smoke.py fps.yanzhenchen.ca 443
```

That speaks the real handshake and expects a `ServerWelcome` back. Then open
the Vercel URL, and confirm the HUD shows a non-zero player count and a
plausible RTT. If the page loads but never connects, the client is fine and
the problem is in §3/§4 — check `journalctl -u caddy -f`.

---

## Security posture — read this once

This puts a **hand-written C++ WebSocket frame parser on your home network's
edge**, reachable by anyone. That parser had an unbounded peer-controlled
length field until M20 (a declared length near 2^64 both wrapped an arithmetic
check and requested a 16-exabyte allocation); it is fixed and tested, but the
lesson is that this is prototype network code, not a hardened service. There
is no authentication and no rate limiting. See
[networking.md](networking.md).

The difference from a cloud VM is the blast radius: a compromised throwaway VM
costs you a VM, while the T14 sits inside your LAN. If that matters to you:

- The systemd unit already runs as an unprivileged user with
  `ProtectSystem=strict`, `ProtectHome`, `NoNewPrivileges`, `PrivateTmp` and a
  read-only `/opt/fps`, plus syscall and address-family restrictions and a
  memory cap.
- Put the T14 on a guest VLAN or an isolated SSID, so a foothold reaches
  nothing else.
- Prefer `--no-enet`: one listener instead of two.
- Path C (tunnel) exposes no inbound port at all, which is the strongest
  option here even when you are not forced into it by CGNAT.

Turning it off is `sudo systemctl stop fps-server`, and nothing about this
setup needs to run when you are not playing.

---

## Cloud VM instead

Nothing above is T14-specific except §0, §3 C and the lid settings. On a cloud
VM (an Oracle free-tier instance works) the same §1/§2/§3 A steps apply, with
two differences: open TCP 80/443 in the provider's security list or NSG as
well as the host firewall, and Oracle's images ship strict iptables, so use
`iptables -I INPUT ... && netfilter-persistent save` rather than `ufw`. A
fixed public IP means no dynamic DNS.

## Native distribution (no browser)

`scripts/package.sh` stages a `dist/fps-<os>-<arch>.zip` (client + server +
assets); native players connect to the host's IP on UDP 7777, which must be
forwarded and must not be behind CGNAT.
