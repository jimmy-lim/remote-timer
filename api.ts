const PORT = Number(process.env.PORT || 3001);
const DATA_PATH = process.env.DATA_PATH || "./timer-data.json";

type Store = {
  timers: Record<string, number>;
  labels: Record<string, string>;
};

type LabelEntry = {
  id: string;
  label: string;
};

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
      return { timers: {}, labels: {} };
    }

    const parsed = JSON.parse(await file.text());
    const timers =
      parsed && typeof parsed === "object" && parsed.timers && typeof parsed.timers === "object"
        ? parsed.timers
        : {};
    const labels =
      parsed && typeof parsed === "object" && parsed.labels && typeof parsed.labels === "object"
        ? parsed.labels
        : {};

    return {
      timers: Object.fromEntries(
        Object.entries(timers)
          .filter(([id]) => typeof id === "string")
          .map(([id, timestamp]) => [id, Number(timestamp) || 0])
      ),
      labels: Object.fromEntries(
        Object.entries(labels)
          .filter(([id]) => typeof id === "string")
          .map(([id, label]) => [id, typeof label === "string" ? label : ""])
      )
    };
  } catch {
    return { timers: {}, labels: {} };
  }
}

async function writeStore(store: Store) {
  await Bun.write(DATA_PATH, JSON.stringify(store, null, 2) + "\n");
}

function timerEntries(timers: Record<string, number>) {
  return Object.entries(timers)
    .sort((a, b) => b[1] - a[1])
    .map(([id, timestamp]) => ({
      id,
      timestamp: String(timestamp)
    }));
}

function labelEntries(labels: Record<string, string>): LabelEntry[] {
  return Object.entries(labels).map(([id, label]) => ({ id, label }));
}

function normalizePostedLabels(payload: unknown): LabelEntry[] {
  if (!Array.isArray(payload)) return [];

  if (
    payload.length === 2 &&
    payload[0] &&
    typeof payload[0] === "object" &&
    payload[1] &&
    typeof payload[1] === "object" &&
    typeof (payload[0] as Record<string, unknown>).id === "string" &&
    typeof (payload[1] as Record<string, unknown>).label === "string"
  ) {
    return [
      {
        id: (payload[0] as Record<string, string>).id,
        label: (payload[1] as Record<string, string>).label
      }
    ];
  }

  return payload.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];

    const item = entry as Record<string, unknown>;
    if (typeof item.id !== "string") return [];

    return [
      {
        id: item.id,
        label: typeof item.label === "string" ? item.label : ""
      }
    ];
  });
}

function normalizeDeletedLabels(payload: unknown): string[] {
  if (!Array.isArray(payload)) return [];

  return payload.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];

    const item = entry as Record<string, unknown>;
    return typeof item.id === "string" ? [item.id] : [];
  });
}

Bun.serve({
  port: PORT,
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/webhook/timer" && request.method === "POST") {
      const timerId = url.searchParams.get("id");
      if (!timerId) {
        return jsonResponse({ error: "Missing id query parameter" }, { status: 400 });
      }

      const store = await readStore();
      store.timers[timerId] = Date.now();
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/webhook/timer" && request.method === "DELETE") {
      const timerId = url.searchParams.get("id");
      if (!timerId) {
        return jsonResponse({ error: "Missing id query parameter" }, { status: 400 });
      }

      const store = await readStore();
      delete store.timers[timerId];
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/webhook/timers" && request.method === "GET") {
      const store = await readStore();
      return jsonResponse(timerEntries(store.timers));
    }

    if (url.pathname === "/webhook/timers" && request.method === "DELETE") {
      const store = await readStore();
      store.timers = {};
      await writeStore(store);

      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/api/labels" && request.method === "GET") {
      const store = await readStore();
      return jsonResponse(labelEntries(store.labels));
    }

    if (url.pathname === "/api/labels" && request.method === "POST") {
      try {
        const payload = await request.json();
        const entries = normalizePostedLabels(payload);

        if (entries.length === 0) {
          return jsonResponse(
            {
              error: "Expected an array of label mappings like [{\"id\":\"timer-1\",\"label\":\"Kitchen\"}]"
            },
            { status: 400 }
          );
        }

        const store = await readStore();
        for (const entry of entries) {
          store.labels[entry.id] = entry.label;
        }
        await writeStore(store);

        return jsonResponse(labelEntries(store.labels));
      } catch (error) {
        return jsonResponse(
          { error: "Failed to save labels", detail: String(error) },
          { status: 400 }
        );
      }
    }

    if (url.pathname === "/api/labels" && request.method === "DELETE") {
      try {
        const payload = await request.json();
        const ids = normalizeDeletedLabels(payload);

        if (ids.length === 0) {
          return jsonResponse(
            { error: "Expected an array of ids like [{\"id\":\"timer-1\"}]" },
            { status: 400 }
          );
        }

        const store = await readStore();
        for (const id of ids) {
          delete store.labels[id];
        }
        await writeStore(store);

        return jsonResponse(labelEntries(store.labels));
      } catch (error) {
        return jsonResponse(
          { error: "Failed to delete labels", detail: String(error) },
          { status: 400 }
        );
      }
    }

    if (url.pathname === "/health") {
      return jsonResponse({ ok: true });
    }

    return new Response("Not found", { status: 404 });
  }
});

console.log(`Remote Timers API running on http://localhost:${PORT}`);
console.log(`Persisting timers and labels in ${DATA_PATH}`);
