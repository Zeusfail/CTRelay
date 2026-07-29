# citra_room dedicated server - deployment (Lot 1)

Self-hosted **Azahar `citra_room`** dedicated server that relays 3DS
"Wi-Fi local" (ad-hoc) sessions between a real 3DS (CFW/Luma3DS), Azahar,
and/or other real 3DS units through a VPS.

This folder is the **deployable artifact**: copy it to a Debian VPS and run
`deploy.sh`. It is independent of the azahar source tree (only needed to
*build* the image).

- **Binary:** `azahar-room` (built with `-DENABLE_ROOM=ON -DENABLE_ROOM_STANDALONE=ON`)
- **Protocol / port:** UDP, default **24872** (`Network::DefaultRoomPort`)
- **Max players:** 254 (`MaxConcurrentConnections`), default 16
- **Mode:** **private** - no `--token` / `--web-api-url`, so the room is
  never announced publicly. Share `IP:port` out of band (Discord, etc.).

---

## Files

| File | Purpose |
|------|---------|
| `docker-compose.yml` | Runs the `azahar-room:latest` container, exposes one UDP port |
| `.env.example` | Room configuration template -> copy to `.env` |
| `entrypoint.sh` | Builds the CLI flags from `.env` (skips empty optional flags) |
| `citra-room.service` | systemd unit (boot + resilience) |
| `deploy.sh` | One-shot installer, runs **on the VPS** |
| `build-image.sh` | Builds `azahar-room:latest` from the azahar source |
| `Dockerfile.lowmem` | Build variant with a configurable `ninja -j` (low-RAM machines) |
| `push-to-vps.sh` | Ships the built image to the VPS over SSH and starts it |

---

## Deployment model (build machine + VPS)

There is **no prebuilt room image** on any public registry, so the image must
be compiled from source. Compiling is heavy (large base image + full C++
build), so do it on a capable machine - **not** on a small VPS.

Recommended setup (matches this project): the machine that holds the **VPS SSH
key** is the **build machine**. It builds the image and pushes it straight to
the VPS - no image file to shuffle around.

```
build machine (has SSH key)  --build-->  azahar-room:latest
        |
        `-- push-to-vps.sh --stream image over SSH + run deploy.sh-->  VPS
```

Build-machine requirements: **Docker**, **git**, ideally **>= 8 GB RAM** and
**~15 GB free disk**. Check first:

```bash
free -h ; df -h .          # Linux / macOS(~)
```

If RAM is tight, use the low-mem build below (do **not** run a full-parallel
build on a small box - ninja will spawn one compiler per core and OOM).

---

## 1. Build the image (on the build machine)

```bash
cd citra-room-deploy
./build-image.sh                 # auto-clones azahar into ./azahar, then builds

# Low-RAM machine: cap parallel compile jobs (~1 job per 1.5-2 GB of RAM):
./build-image.sh --jobs 2

# Point at an existing azahar checkout instead of auto-cloning:
./build-image.sh /path/to/azahar
```

The first build downloads the multi-GB `opensauce04/azahar-build-environment`
base image and compiles azahar - expect a long run. The **final** image is
small (based on `debian:trixie`).

---

## 2. Push to the VPS (from the build machine)

```bash
cp .env.example .env             # edit ROOM_NAME, PREFERRED_APP, ROOM_PORT, ...
./push-to-vps.sh user@vps-host   # add the ssh port as a 2nd arg if not 22
```

`push-to-vps.sh`:
1. copies the deploy files to `~/citra-room-deploy` on the VPS,
2. **streams** the image over SSH (`docker save | ssh docker load`) - no large
   intermediate file,
3. runs `deploy.sh` on the VPS, which installs into `/opt/citra-room`, opens
   **only** the room's UDP port (via `ufw`), and enables the systemd service.

> Copy your edited `.env` to the VPS too if you want your settings applied on
> first boot: `scp .env user@vps-host:~/citra-room-deploy/`. Otherwise the VPS
> starts from `.env.example` defaults, editable later in `/opt/citra-room/.env`.

### Manual alternative (already on the VPS)

If you prefer to work directly on the VPS (Docker installed there):

```bash
# Docker Engine, if missing:  curl -fsSL https://get.docker.com | sh
cd ~/citra-room-deploy
cp .env.example .env             # edit as needed
sudo ./deploy.sh                 # loads azahar-room.tar.gz if you scp'd one
# ...or without systemd:
cd /opt/citra-room && docker compose up -d
```

---

## 3. Configuration (`.env`)

| Variable | Required | Default | Notes |
|----------|:--------:|---------|-------|
| `ROOM_NAME` | yes | `My Azahar Room` | Refuses to start if empty |
| `PREFERRED_APP` | yes | `Homebrew` | Refuses to start if empty |
| `ROOM_DESCRIPTION` | | - | |
| `PREFERRED_APP_ID` | | - | 16 hex digits; helps clients match a game |
| `ROOM_PORT` | | `24872` | UDP |
| `MAX_MEMBERS` | | `16` | 2-254 |
| `ROOM_PASSWORD` | | - | Empty = open (still unlisted) |

After editing `.env`:

```bash
sudo systemctl restart citra-room
```

---

## 4. Firewall

Only the chosen **UDP** port must be open. `deploy.sh` does this with `ufw`:

```bash
ufw allow 24872/udp
ufw enable
```

nftables equivalent:

```bash
nft add rule inet filter input udp dport 24872 accept
```

At your VPS provider, also allow inbound **UDP 24872** in any external
security group / cloud firewall.

---

## 5. Operations

```bash
# Status / logs
systemctl status citra-room
docker logs -f citra-room          # live room log
journalctl -u citra-room           # service log

# Lifecycle
systemctl restart citra-room       # apply .env changes
systemctl stop citra-room
docker compose -f /opt/citra-room/docker-compose.yml ps

# Update the image (after rebuilding azahar-room:latest)
systemctl restart citra-room       # recreates the container on the new image
```

The room log and ban list persist in the `room-data` Docker volume
(`/data/citra-room.log`, `/data/ban-list.txt`).

### Resilience

Two layers:

- **Container crash** -> `restart: unless-stopped` in `docker-compose.yml`
  restarts it automatically.
- **VPS reboot** -> the `citra-room` systemd unit (enabled) brings the stack
  back up at boot.

---

## 6. Connecting a client

Share `IP:port` (and password, if set) out of band. In Azahar:
**Multiplayer -> Direct Connect ->** `<VPS_IP>:24872`. The real-3DS side is
**Lot 2** (out of scope here).

---

## Scope

This is **Lot 1** only (the VPS room server). The 3DS-side hook
(`nwm::UDS` interception) is **Lot 2** and is not addressed here.
