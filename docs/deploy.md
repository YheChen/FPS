# Deployment

Two pieces (see [ADR 0005](decisions/0005-web-client.md)):

- **Client** → static WebAssembly hosted on **Vercel**.
- **Server** → the native `fps_server` on the **Oracle VM**, with **Caddy**
  terminating TLS so the browser can reach it over `wss://`.

```
browser ──https──▶ Vercel (static .wasm/.js/.data)
   │
   └──wss://fps.yanzhenchen.ca──▶ Caddy (TLS) ──ws://localhost:7778──▶ fps_server
```

Why the proxy: a page served over HTTPS may only open secure (`wss://`)
WebSockets, and Let's Encrypt needs a domain (not a bare IP). Caddy handles
the certificate and keeps the C++ server as plain `ws://` on localhost.

---

## 1. Server on the Oracle VM

Ubuntu 24.04 assumed (24.04 has CMake 3.28 + GCC 13; 22.04's are too old).

### Build

```sh
sudo apt update && sudo apt install -y git cmake ninja-build build-essential
sudo git clone https://github.com/YheChen/FPS.git /opt/fps
cd /opt/fps
cmake --preset release
cmake --build --preset release --target fps_server --parallel
```

Building only `fps_server` skips SDL/GL/audio (the server is headless), so no
graphics packages are needed.

### Run as a service

```sh
sudo useradd --system --home /opt/fps --shell /usr/sbin/nologin fps
sudo chown -R fps:fps /opt/fps
sudo cp deploy/fps-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now fps-server
systemctl status fps-server      # should be active; listening on ws 7778
```

The unit runs `--ws-port 7778` (browser players) and keeps ENet on UDP 7777
(native players). For browser-only, add `--no-enet` to `ExecStart` and skip
the UDP firewall rule below.

### TLS proxy (Caddy)

```sh
# Install Caddy (Debian/Ubuntu official repo)
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy

sudo cp /opt/fps/deploy/Caddyfile /etc/caddy/Caddyfile   # edit the domain first
sudo systemctl reload caddy
```

Edit the domain in `deploy/Caddyfile` if it is not `fps.yanzhenchen.ca`.

### DNS + firewall

- **DNS:** add an `A` record `fps.yanzhenchen.ca` → the VM's public IP.
  (No domain? Use a free DuckDNS host or `<ip-with-dashes>.sslip.io`, and put
  that name in the Caddyfile instead.)
- **OCI security list / NSG** (cloud firewall) — add Ingress rules, source
  `0.0.0.0/0`:
  - TCP **80** and **443** (Caddy: ACME challenge + wss)
  - UDP **7777** — only if serving native players
- **Host firewall** (Oracle images ship strict iptables):

```sh
sudo iptables -I INPUT -p tcp --dport 80 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 443 -j ACCEPT
sudo iptables -I INPUT -p udp --dport 7777 -j ACCEPT   # native players only
sudo netfilter-persistent save
```

Verify: `curl -I https://fps.yanzhenchen.ca` returns a Caddy response with a
valid certificate.

---

## 2. Client on Vercel

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
Vercel dashboard). Opening it connects straight to the VM — no download, no
IP to type.

To rebuild after changes, re-run the two commands above.

### Notes

- The `.data` bundle embeds `assets/`; rebuild it whenever assets change.
- No download step for players — share the Vercel URL.
- This is unauthenticated and unencrypted-at-rest game state; it is a
  prototype deathmatch, not a hardened service. See
  [networking.md](networking.md) for the (non-)security posture.

---

## Native distribution (no browser)

For native players, `scripts/package.sh` still stages a
`dist/fps-<os>-<arch>.zip` (client + server + assets); they connect to the
VM's IP on UDP 7777.
