export {};

const PORT = Number(process.env.PORT || 3001);
const DATA_PATH = process.env.DATA_PATH || "./timer-data.json";

type Store = {
  timers: Record<string, number>;
  enrich: Record<string, TimerEnrich>;
};

type TimerEnrich = {
  label?: string;
  notes?: string;
  alertAfter?: string;
  alertInterval?: string;
  muted?: boolean;
  order?: string;
};

type EnrichEntry = {
  id: string;
} & TimerEnrich;

function jsonResponse(data: unknown, init?: ResponseInit) {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: {
      "content-type": "application/json; charset=utf-8",
      ...(init?.headers || {})
    }
  });
}

async function readStore(): Promise<Store> {
  try {
    const file = Bun.file(DATA_PATH);
    if (!(await file.exists())) {
      return { timers: {}, enrich: {} };
    }

    const parsed = JSON.parse(await file.text());
    const timers =
      parsed && typeof parsed === "object" && parsed.timers && typeof parsed.timers === "object"
        ? parsed.timers
        : {};
    const enrichRaw =
      parsed && typeof parsed === "object" && parsed.enrich && typeof parsed.enrich === "object"
        ? parsed.enrich
        : {};

    return {
      timers: Object.fromEntries(
        Object.entries(timers)
          .filter(([id]) => typeof id === "string")
          .map(([id, value]) => [id, sanitizeTimerTimestamp(value)])
      ),
      enrich: Object.fromEntries(
        Object.entries(enrichRaw)
          .filter(([id]) => typeof id === "string")
          .map(([id, value]) => [
            id,
            sanitizeTimerEnrich(value)
          ])
      )
    };
  } catch {
    return { timers: {}, enrich: {} };
  }
}

async function writeStore(store: Store) {
  await Bun.write(DATA_PATH, JSON.stringify(store, null, 2) + "\n");
}

function sanitizeTimerTimestamp(value: unknown): number {
  if (typeof value === "number") {
    return Number(value) || 0;
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return 0;
  }
  const raw = value as Record<string, unknown>;
  return Number(raw.timestamp) || 0;
}

function sanitizeOrder(value: unknown): string | undefined {
  if (typeof value === "number" && Number.isFinite(value)) return String(Math.trunc(value));
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  if (!trimmed) return "";
  return /^[0-9]+$/.test(trimmed) ? trimmed : undefined;
}

function sanitizeTimerEnrich(value: unknown): TimerEnrich {
  if (!value || typeof value !== "object" || Array.isArray(value)) return {};

  const raw = value as Record<string, unknown>;
  const next: TimerEnrich = {};

  if (typeof raw.label === "string") next.label = raw.label;
  if (typeof raw.notes === "string") next.notes = raw.notes;
  if (typeof raw.alertAfter === "string") next.alertAfter = raw.alertAfter;
  if (typeof raw.alertInterval === "string") next.alertInterval = raw.alertInterval;
  if (typeof raw.muted === "boolean") next.muted = raw.muted;
  const order = sanitizeOrder(raw.order);
  if (order !== undefined) next.order = order;

  return next;
}

function timerEntries(timers: Record<string, number>) {
  return Object.entries(timers)
    .sort((a, b) => b[1] - a[1])
    .map(([id, timestamp]) => ({
      id,
      timestamp: String(timestamp)
    }));
}

function enrichEntries(enrich: Record<string, TimerEnrich>): EnrichEntry[] {
  return Object.entries(enrich).map(([id, value]) => ({ id, ...value }));
}

function normalizePostedEnrich(payload: unknown): EnrichEntry[] {
  if (!Array.isArray(payload)) return [];

  return payload.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];

    const item = { ...(entry as Record<string, unknown>) };
    if (typeof item.id !== "string") return [];
    const { id } = item;
    delete item.id;

    return [
      {
        id,
        ...sanitizeTimerEnrich(item)
      }
    ];
  });
}

function normalizeDeletedEnrich(payload: unknown): string[] {
  if (!Array.isArray(payload)) return [];

  return payload.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];

    const item = entry as Record<string, unknown>;
    return typeof item.id === "string" ? [item.id] : [];
  });
}

function parseDurationToMs(value: unknown): number | null {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  const match = trimmed.match(/^([0-9]+)\s*([smhdw]|mo)$/i);
  if (!match) return null;
  const amount = Number.parseInt(match[1], 10);
  if (!Number.isFinite(amount) || amount <= 0) return null;
  const unit = match[2].toLowerCase();
  const multipliers: Record<string, number> = {
    s: 1000,
    m: 60 * 1000,
    h: 60 * 60 * 1000,
    d: 24 * 60 * 60 * 1000,
    w: 7 * 24 * 60 * 60 * 1000,
    mo: 30 * 24 * 60 * 60 * 1000
  };
  return amount * multipliers[unit];
}

Bun.serve({
  port: PORT,
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/api/timer" && request.method === "POST") {
      const timerId = url.searchParams.get("id");
      if (!timerId) {
        return jsonResponse({ error: "Missing id query parameter" }, { status: 400 });
      }
      const store = await readStore();
      store.timers[timerId] = Date.now();
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/api/timer" && request.method === "DELETE") {
      const timerId = url.searchParams.get("id");
      if (!timerId) {
        return jsonResponse({ error: "Missing id query parameter" }, { status: 400 });
      }

      const store = await readStore();
      delete store.timers[timerId];
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/api/timers" && request.method === "GET") {
      const store = await readStore();
      return jsonResponse(timerEntries(store.timers));
    }

    if (url.pathname === "/api/timers" && request.method === "DELETE") {
      const store = await readStore();
      store.timers = {};
      store.enrich = {};
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/api/enrich" && request.method === "GET") {
      const store = await readStore();
      return jsonResponse(enrichEntries(store.enrich));
    }

    if (url.pathname === "/api/enrich" && request.method === "POST") {
      try {
        const payload = await request.json();
        const entries = normalizePostedEnrich(payload);

        if (entries.length === 0) {
          return jsonResponse(
            {
              error: "Expected an array like [{\"id\":\"timer-1\",\"label\":\"Kitchen\"}]"
            },
            { status: 400 }
          );
        }

        const store = await readStore();
        for (const entry of entries) {
          const { id, ...next } = entry;
          store.enrich[id] = {
            ...(store.enrich[id] || {}),
            ...next
          };
        }
        await writeStore(store);

        return jsonResponse(enrichEntries(store.enrich));
      } catch (error) {
        return jsonResponse(
          { error: "Failed to save enrich data", detail: String(error) },
          { status: 400 }
        );
      }
    }

    if (url.pathname === "/api/enrich" && request.method === "DELETE") {
      try {
        const payload = await request.json();
        const ids = normalizeDeletedEnrich(payload);

        if (ids.length === 0) {
          return jsonResponse(
            { error: "Expected an array of ids like [{\"id\":\"timer-1\"}]" },
            { status: 400 }
          );
        }

        const store = await readStore();
        for (const id of ids) {
          delete store.enrich[id];
        }
        await writeStore(store);

        return jsonResponse(enrichEntries(store.enrich));
      } catch (error) {
        return jsonResponse(
          { error: "Failed to delete enrich data", detail: String(error) },
          { status: 400 }
        );
      }
    }

    if (url.pathname === "/api/device-state" && request.method === "GET") {
      const suffix = (url.searchParams.get("id") || "").trim();
      if (!suffix || suffix.includes("/") || suffix.includes(" ")) {
        return new Response("Missing or invalid id query parameter\n", { status: 400 });
      }

      const store = await readStore();
      const nowMs = Date.now();
      const lines: string[] = [`now,${nowMs}`];

      for (let button = 1; button <= 4; button++) {
        const timerId = `${suffix}-${button}`;
        const timestampMs = Number(store.timers[timerId] || 0);
        const enrich = store.enrich[timerId];
        const muted = enrich?.muted === true ? 1 : 0;
        const alertAfterMs = parseDurationToMs(enrich?.alertAfter) ?? 0;
        const alertIntervalMs = parseDurationToMs(enrich?.alertInterval) ?? 0;
        lines.push(`${button},${timestampMs},${muted},${alertAfterMs},${alertIntervalMs}`);
      }

      return new Response(`${lines.join("\n")}\n`, {
        headers: { "content-type": "text/plain; charset=utf-8" }
      });
    }

    if (url.pathname === "/health") {
      return jsonResponse({ ok: true });
    }

    return new Response("Not found", { status: 404 });
  }
});

console.log(`Remote Timers API running on http://localhost:${PORT}`);
console.log(`Persisting timers and enrich data in ${DATA_PATH}`);
