# Deployment

Two pieces (see [ADR 0005](decisions/0005-web-client.md)):

- **Client** → static WebAssembly on **Vercel**.
- **Server** → the native `fps_server` on a **ThinkPad T14 at home**, with
  **Caddy** terminating TLS so the browser can reach it over `wss://`.

```
browser ──https──▶ Vercel (static .wasm/.js/.data)
   │
   └──wss://fps.yanzhenchen.ca──▶ Caddy (TLS) ──ws://localhost:7778──▶ fps_server
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
| Same | You have a real public IP; port forwarding will work | §3 **A** or **B** |
| Different | You are behind **CGNAT** — no port forwarding can ever work | §3 **C** (tunnel) |

**Distro:** the build needs **CMake ≥ 3.25** and a **C++23** compiler.
Ubuntu 24.04 (CMake 3.28, GCC 13) and Debian 13 are fine. Ubuntu 22.04 is
**not** — its CMake 3.22 and GCC 11 are both too old, and you would be
building toolchains before you build the game. Check with
`cmake --version && g++ --version`.

**DNS:** if the nameservers say `cloudflare.com`, you can use the DNS-01
certificate path (§3 B) and Cloudflare's API for dynamic DNS (§4), and you
never need inbound port 80. If they say something else, you are on the HTTP-01
path (§3 A) and need port 80 reachable.

---

## 1. Build the server on the T14

```sh
sudo apt update && sudo apt install -y git cmake ninja-build build-essential ca-certificates
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
exists now so the claim is checked rather than asserted: a bare `ubuntu:24.04`
container, exactly the `apt` line above, `fps_server` only, then start it — and
it asserts SDL/ImGui/miniaudio were not fetched.)

Sanity check it before wiring anything up:

```sh
./build/release/game/fps_server --ws-port 7778 --no-enet --bots 2 --run-seconds 10
```

You should see the map load, a tick rate near 60/s, and two bots in the player
count.

## 2. Run it as a service

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

### Keep the laptop awake

A closed lid suspends the machine on almost every desktop install, which takes
the server with it:

```sh
sudo tee /etc/systemd/logind.conf.d/99-fps.conf <<'EOF'
[Login]
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore
EOF
sudo systemctl restart systemd-logind
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

Leave it plugged in. A T14 running a headless server draws very little, but a
battery held at 100% ages faster — if the BIOS or `tlp` offers a charge
threshold, cap it around 80%.

---

## 3. TLS — pick one path

### A. HTTP-01 (public IP, port 80 reachable)

The simple case. Stock Caddy from the Debian/Ubuntu repo:

```sh
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy

sudo cp /opt/fps/deploy/Caddyfile /etc/caddy/Caddyfile   # edit the domain first
sudo systemctl reload caddy
```

Forward **TCP 80 and 443** on the router to the T14 (§4).

Many residential ISPs block inbound 80. If Caddy's log shows the ACME
challenge timing out, that is what happened — switch to **B**.

### B. DNS-01 via Cloudflare (port 80 blocked, or you just prefer it)

Caddy proves domain control by writing a DNS record instead of answering on
port 80, so **only 443 ever needs to be open**. Stock Caddy cannot do this;
the Cloudflare DNS module is not compiled in. Build one that has it:

```sh
sudo apt install -y golang-go
go install github.com/caddyserver/xcaddy/cmd/xcaddy@latest
~/go/bin/xcaddy build --with github.com/caddy-dns/cloudflare
sudo install -m 0755 ./caddy /usr/bin/caddy      # replaces the packaged binary
caddy list-modules | grep dns.providers.cloudflare
```

Create a Cloudflare API token (dashboard → My Profile → API Tokens) with
**Zone → DNS → Edit** on `yanzhenchen.ca` only. Keep it out of the Caddyfile:

```sh
sudo mkdir -p /etc/systemd/system/caddy.service.d
sudo tee /etc/systemd/system/caddy.service.d/10-cloudflare.conf <<'EOF'
[Service]
Environment=CF_API_TOKEN=paste-the-token-here
EOF
sudo chmod 600 /etc/systemd/system/caddy.service.d/10-cloudflare.conf
sudo systemctl daemon-reload
```

Then use the DNS-01 stanza in [`deploy/Caddyfile`](../deploy/Caddyfile) —
uncomment the `tls` block — and `sudo systemctl restart caddy`.

Forward **TCP 443** only.

### C. Cloudflare Tunnel (behind CGNAT)

If your public IP differs from the router's WAN IP, no inbound port can reach
you. A tunnel dials *out* from the T14 and Cloudflare accepts connections on
its edge, so **nothing needs forwarding and Caddy is not needed at all** —
Cloudflare terminates TLS.

```sh
# cloudflared, from Cloudflare's apt repo (see their docs for the current key)
sudo apt install -y cloudflared
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

---

## 4. Router, firewall, and dynamic DNS

**Port forward** on the router to the T14's LAN address — give it a DHCP
reservation first, or the forward breaks the next time it renews:

| Port | Needed for |
|---|---|
| TCP 443 | `wss://` (all paths except C) |
| TCP 80 | ACME HTTP-01 only (path A) |
| UDP 7777 | native ENet players only — skip it if you run `--no-enet` |

**Host firewall**, if `ufw` is active:

```sh
sudo ufw allow 443/tcp
sudo ufw allow 80/tcp      # path A only
sudo ufw allow 7777/udp    # native players only
```

**DNS.** Add an `A` record `fps.yanzhenchen.ca` → your public IP. Residential
IPs rotate, so automate it. On Cloudflare, reuse the token from §3 B with
`ddclient` or a cron one-liner against their API; otherwise use DuckDNS and
point the Caddyfile at the DuckDNS name instead. Path C needs none of this —
the tunnel handles it.

**Verify:**

```sh
curl -I https://fps.yanzhenchen.ca     # valid cert, a Caddy or Cloudflare response
```

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
