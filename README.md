# Remote Timer (Bun)

Remote timer system with:

- Bun API (`api.ts`) for timer state + labels
- Bun UI (`ui.ts`) that proxies API calls
- ESP32-C3 firmware (`remote-timer.ino`) that sends timer events

## API Routes (port 3001)

- `POST /api/timer?id=<suffix>-<button>`
- `DELETE /api/timer?id=<suffix>-<button>`
- `GET /api/timers`
- `DELETE /api/timers`
- `GET /api/labels`
- `POST /api/labels`
- `DELETE /api/labels`
- `GET /health`

## Local Run (Bun)

Requirements:

- Bun installed

Run API:

```bash
bun run api.ts
```

Run UI (new terminal):

```bash
bun run ui.ts
```

Open:

- UI: `http://localhost:3000`
- API health: `http://localhost:3001/health`

## Docker / Portainer

Use `compose.yml`:

```bash
docker compose up -d --build
```

Open:

- UI: `http://<host-ip>:3000`
- API: `http://<host-ip>:3001`

Notes:

- API data persists in `./data/timer-data.json` via mounted volume.
- `data/` is excluded from git and Docker build context.

## ESP32 Firmware

File: `remote-timer.ino`

In config portal (`http://192.168.4.1`), set:

- Wi-Fi SSID/password
- API host (`host:port`), for example `192.168.1.42:3001`
- Optional bearer token

Firmware constructs endpoint automatically as:

- `http://<api-host>/api/timer?id=<suffix>-<button>`

Behavior:

- Short press: `POST`
- Long press: `DELETE`

## Repo Layout

- `api.ts` - Bun API server
- `ui.ts` - Bun UI + proxy server
- `remote-timer.ino` - ESP32 firmware
- `compose.yml` - Docker Compose deployment
- `Dockerfile` - Bun runtime image
