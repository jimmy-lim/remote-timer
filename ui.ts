const PORT = Number(process.env.PORT || 3000);
const API_BASE_URL = process.env.API_BASE_URL || "http://127.0.0.1:3001";
const TIMERS_URL =
  process.env.TIMERS_URL || `${API_BASE_URL.replace(/\/$/, "")}/webhook/timers`;
const TIMER_URL =
  process.env.TIMER_URL ||
  (TIMERS_URL.endsWith("/timers")
    ? `${TIMERS_URL.slice(0, -"/timers".length)}/timer`
    : `${TIMERS_URL.replace(/\/$/, "")}/timer`);
const LABELS_URL =
  process.env.LABELS_URL || `${API_BASE_URL.replace(/\/$/, "")}/api/labels`;

const html = `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Remote Timers</title>
    <style>
      :root {
        color-scheme: light;
        --bg: #f6f4ef;
        --panel: #fffdf7;
        --line: #d9d2c3;
        --text: #1f1d18;
        --muted: #6f6657;
        --accent: #bf5b04;
        --accent-soft: #fff1df;
        --error: #a32121;
      }

      * {
        box-sizing: border-box;
      }

      body {
        margin: 0;
        font-family: Georgia, "Times New Roman", serif;
        background:
          radial-gradient(circle at top left, #fff6e8, transparent 26%),
          linear-gradient(180deg, #f7f2e8 0%, var(--bg) 100%);
        color: var(--text);
      }

      main {
        max-width: 980px;
        margin: 0 auto;
        padding: 32px 20px 48px;
      }

      h1 {
        margin: 0 0 8px;
        font-size: clamp(2rem, 4vw, 3.5rem);
        line-height: 1;
      }

      .lede {
        margin: 0 0 24px;
        color: var(--muted);
        font-size: 1.05rem;
      }

      .status-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
        gap: 12px;
        margin-bottom: 20px;
      }

      .card {
        background: var(--panel);
        border: 1px solid var(--line);
        border-radius: 16px;
        padding: 16px;
        box-shadow: 0 10px 30px rgba(80, 56, 24, 0.06);
      }

      .label {
        display: block;
        margin-bottom: 8px;
        color: var(--muted);
        font-size: 0.85rem;
        text-transform: uppercase;
        letter-spacing: 0.08em;
      }

      .value {
        font-size: 1.25rem;
        font-weight: 700;
      }

      .actions {
        display: flex;
        flex-wrap: wrap;
        gap: 12px;
        align-items: center;
        margin-bottom: 20px;
      }

      button {
        border: 0;
        border-radius: 999px;
        background: var(--accent);
        color: white;
        padding: 12px 18px;
        font: inherit;
        cursor: pointer;
      }

      button:disabled {
        opacity: 0.6;
        cursor: wait;
      }

      .hint {
        color: var(--muted);
        font-size: 0.95rem;
      }

      .error {
        min-height: 1.2em;
        margin-bottom: 16px;
        color: var(--error);
      }

      table {
        width: 100%;
        border-collapse: collapse;
        background: var(--panel);
        border: 1px solid var(--line);
        border-radius: 16px;
        overflow: hidden;
      }

      th,
      td {
        padding: 12px 14px;
        border-bottom: 1px solid var(--line);
        text-align: left;
        vertical-align: top;
      }

      th {
        background: var(--accent-soft);
        font-size: 0.9rem;
      }

      tr:last-child td {
        border-bottom: 0;
      }

      .mono {
        font-family: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
        font-size: 0.9rem;
      }

      .label-input {
        width: 100%;
        border: 1px solid var(--line);
        border-radius: 10px;
        background: #fff;
        color: var(--text);
        padding: 8px 10px;
        font: inherit;
      }

      .label-input::placeholder {
        color: #9b907e;
      }

      .label-cell {
        display: block;
      }

      .timer-delete {
        border: 1px solid #d7b7b7;
        border-radius: 10px;
        background: #fff5f5;
        color: #8c2a2a;
        padding: 8px 10px;
      }

      .danger {
        background: #8c2a2a;
      }

      @media (max-width: 700px) {
        main {
          padding: 16px 12px 24px;
        }

        .lede,
        .status-grid,
        .actions,
        .error,
        th.mobile-hide,
        td.mobile-hide {
          display: none;
        }

        td[data-label="Label"] .label-cell {
          min-width: 0;
        }

      th[data-col="label"],
      td[data-col="label"],
      th[data-col="age"],
      td[data-col="age"] {
        width: 50%;
      }

      td[data-col="age"] {
        vertical-align: middle;
      }
      }
    </style>
  </head>
  <body>
    <main>
      <h1>Remote Timers</h1>
      <p class="lede">Fetches timers on page load and refreshes them every 10 seconds.</p>

      <section class="status-grid">
        <article class="card">
          <span class="label">Now</span>
          <div class="value" id="now">-</div>
        </article>
        <article class="card">
          <span class="label">Last API Refresh</span>
          <div class="value" id="last-refresh">never</div>
        </article>
        <article class="card">
          <span class="label">Auto Refresh</span>
          <div class="value">every 10s</div>
        </article>
      </section>

      <div class="actions">
        <button id="refresh-button">Refresh Now</button>
        <button id="delete-all-button" class="danger" type="button">Delete All Timers</button>
        <span class="hint">Proxy target: <code>${TIMERS_URL}</code></span>
      </div>

      <div id="error" class="error"></div>

      <table>
        <thead>
          <tr>
            <th class="mobile-hide">ID</th>
            <th data-col="label">Label</th>
            <th class="mobile-hide">Updated At</th>
            <th data-col="age">Elapsed</th>
            <th class="mobile-hide">Timer</th>
          </tr>
        </thead>
        <tbody id="rows">
          <tr>
            <td colspan="5">Loading…</td>
          </tr>
        </tbody>
      </table>
    </main>

    <script type="module">
      const state = {
        rows: [],
        labels: {},
        uiNowMs: Date.now(),
        lastRefreshMs: null,
        loading: false
      };

      const nowEl = document.querySelector("#now");
      const lastRefreshEl = document.querySelector("#last-refresh");
      const errorEl = document.querySelector("#error");
      const rowsEl = document.querySelector("#rows");
      const refreshButton = document.querySelector("#refresh-button");
      const deleteAllButton = document.querySelector("#delete-all-button");

      function normalize(raw) {
        if (Array.isArray(raw)) {
          if (raw.length === 1 && raw[0] && typeof raw[0] === "object" && !Array.isArray(raw[0])) {
            return Object.entries(raw[0]).map(([id, timestamp]) => ({ id, timestamp }));
          }
          return raw;
        }

        if (raw && Array.isArray(raw.timers)) return raw.timers;
        if (raw && Array.isArray(raw.data)) return raw.data;

        if (raw && typeof raw === "object") {
          return Object.entries(raw).map(([id, timestamp]) => ({ id, timestamp }));
        }

        return [];
      }

      function formatNow(ms) {
        const d = new Date(ms);
        const yyyy = d.getFullYear();
        const MM = String(d.getMonth() + 1).padStart(2, "0");
        const dd = String(d.getDate()).padStart(2, "0");
        const hh = String(d.getHours()).padStart(2, "0");
        const mm = String(d.getMinutes()).padStart(2, "0");
        const ss = String(d.getSeconds()).padStart(2, "0");
        return \`\${yyyy}/\${MM}/\${dd} \${hh}:\${mm}:\${ss}\`;
      }

      function humanAge(ageSec) {
        if (ageSec === null || ageSec === undefined) return "";
        if (ageSec <= 2) return "just now";
        if (ageSec < 60) return \`\${ageSec}s ago\`;

        const min = Math.floor(ageSec / 60);
        if (min < 60) return \`\${min}m ago\`;

        const hrs = Math.floor(min / 60);
        const remMin = min % 60;
        if (hrs < 24) return remMin > 0 ? \`\${hrs}h \${remMin}m ago\` : \`\${hrs}h ago\`;

        const days = Math.floor(hrs / 24);
        const remHrs = hrs % 24;
        return remHrs > 0 ? \`\${days}d \${remHrs}h ago\` : \`\${days}d ago\`;
      }

      function renderStatus() {
        nowEl.textContent = formatNow(state.uiNowMs);
        lastRefreshEl.textContent = state.lastRefreshMs
          ? new Date(state.lastRefreshMs).toLocaleTimeString()
          : "never";
      }

      function getDisplayRows() {
        return state.rows.map((row, index) => {
          const id = row.id ?? row.timerId ?? \`row-\${index + 1}\`;
          const ts = Number(row.timestamp ?? row.ts ?? row.lastPress ?? 0);
          const validTs = Number.isFinite(ts) ? ts : 0;
          const ageSec = validTs > 0 ? Math.max(0, Math.floor((state.uiNowMs - validTs) / 1000)) : null;

          return {
            id,
            label: state.labels[id] || "",
            timestampMs: validTs,
            updatedAt: validTs > 0 ? new Date(validTs).toLocaleString() : "",
            age: humanAge(ageSec)
          };
        });
      }

      function renderRows() {
        const focusedInput =
          document.activeElement && document.activeElement.classList?.contains("label-input")
            ? document.activeElement
            : null;
        const focusedTimerId = focusedInput?.dataset?.timerId || "";
        const focusedValue = focusedInput?.value || "";
        const selectionStart = focusedInput?.selectionStart ?? null;
        const selectionEnd = focusedInput?.selectionEnd ?? null;

        if (focusedTimerId) {
          state.labels[focusedTimerId] = focusedValue;
        }

        const displayRows = getDisplayRows();

        if (displayRows.length === 0) {
          rowsEl.innerHTML = '<tr><td colspan="5">No timers returned.</td></tr>';
          return;
        }

        rowsEl.innerHTML = displayRows.map((row) => \`
          <tr>
            <td class="mobile-hide" data-label="ID">\${escapeHtml(row.id)}</td>
            <td data-col="label" data-label="Label">
              <div class="label-cell">
                <input
                  class="label-input"
                  data-timer-id="\${escapeHtml(row.id)}"
                  type="text"
                  value="\${escapeHtml(row.label)}"
                  placeholder="Set label"
                />
              </div>
            </td>
            <td class="mobile-hide" data-label="Updated At">\${escapeHtml(row.updatedAt)}</td>
            <td data-col="age" data-label="Age">
              <span
                data-age-timer-id="\${escapeHtml(row.id)}"
                data-timestamp-ms="\${escapeHtml(String(row.timestampMs || 0))}"
              >\${escapeHtml(row.age)}</span>
            </td>
            <td class="mobile-hide" data-label="Timer">
              <button
                class="timer-delete"
                data-delete-single-timer-id="\${escapeHtml(row.id)}"
                type="button"
              >
                Delete Timer
              </button>
            </td>
          </tr>
        \`).join("");

        rowsEl.querySelectorAll(".label-input").forEach((input) => {
          input.addEventListener("change", async (event) => {
            const target = event.currentTarget;
            const timerId = target.dataset.timerId;
            if (!timerId) return;

            const nextLabel = target.value;
            try {
              const response = await fetch("/api/labels", {
                method: "POST",
                headers: {
                  "content-type": "application/json"
                },
                body: JSON.stringify([
                  {
                    id: timerId,
                    label: nextLabel
                  }
                ])
              });

              if (!response.ok) throw new Error(\`HTTP \${response.status}\`);
              const json = await response.json();
              state.labels = Array.isArray(json)
                ? Object.fromEntries(
                    json
                      .filter((entry) => entry && typeof entry.id === "string")
                      .map((entry) => [entry.id, typeof entry.label === "string" ? entry.label : ""])
                  )
                : state.labels;
              const activeInput = rowsEl.querySelector(\`.label-input[data-timer-id="\${CSS.escape(timerId)}"]\`);
              if (activeInput) activeInput.value = state.labels[timerId] || "";
              errorEl.textContent = "";
            } catch (error) {
              errorEl.textContent = String(error?.message || error);
            }
          });
        });

        rowsEl.querySelectorAll(".timer-delete").forEach((button) => {
          button.addEventListener("click", async (event) => {
            const target = event.currentTarget;
            const timerId = target.dataset.deleteSingleTimerId;
            if (!timerId) return;

            try {
              const response = await fetch(\`/api/timer?id=\${encodeURIComponent(timerId)}\`, {
                method: "DELETE"
              });

              if (!response.ok) throw new Error(\`HTTP \${response.status}\`);
              state.rows = state.rows.filter((row, index) => {
                const id = row.id ?? row.timerId ?? \`row-\${index + 1}\`;
                return id !== timerId;
              });
              delete state.labels[timerId];
              renderRows();
              errorEl.textContent = "";
            } catch (error) {
              errorEl.textContent = String(error?.message || error);
            }
          });
        });

        if (focusedTimerId) {
          const restoredInput = rowsEl.querySelector(
            \`.label-input[data-timer-id="\${CSS.escape(focusedTimerId)}"]\`
          );

          if (restoredInput) {
            restoredInput.focus();
            restoredInput.value = focusedValue;
            if (selectionStart !== null && selectionEnd !== null) {
              restoredInput.setSelectionRange(selectionStart, selectionEnd);
            }
          }
        }
      }

      function updateAgeCells() {
        rowsEl.querySelectorAll("[data-age-timer-id]").forEach((node) => {
          const timestampMs = Number(node.dataset.timestampMs || 0);
          const ageSec = timestampMs > 0
            ? Math.max(0, Math.floor((state.uiNowMs - timestampMs) / 1000))
            : null;
          node.textContent = humanAge(ageSec);
        });
      }

      function escapeHtml(value) {
        return String(value)
          .replaceAll("&", "&amp;")
          .replaceAll("<", "&lt;")
          .replaceAll(">", "&gt;")
          .replaceAll('"', "&quot;")
          .replaceAll("'", "&#39;");
      }

      async function loadTimers(options = {}) {
        const { manual = false } = options;
        if (state.loading) return;

        state.loading = true;
        if (manual) {
          refreshButton.disabled = true;
        }
        errorEl.textContent = "";

        try {
          const response = await fetch("/api/timers");
          if (!response.ok) throw new Error(\`HTTP \${response.status}\`);

          const json = await response.json();
          state.rows = normalize(json);
          state.lastRefreshMs = Date.now();
          renderStatus();
          renderRows();
        } catch (error) {
          errorEl.textContent = String(error?.message || error);
        } finally {
          state.loading = false;
          if (manual) {
            refreshButton.disabled = false;
          }
        }
      }

      async function loadLabels() {
        try {
          const response = await fetch("/api/labels");
          if (!response.ok) throw new Error(\`HTTP \${response.status}\`);

          const json = await response.json();
          state.labels = Array.isArray(json)
            ? Object.fromEntries(
                json
                  .filter((entry) => entry && typeof entry.id === "string")
                  .map((entry) => [entry.id, typeof entry.label === "string" ? entry.label : ""])
              )
            : {};
          renderRows();
        } catch (error) {
          errorEl.textContent = String(error?.message || error);
        }
      }

      refreshButton.addEventListener("click", () => {
        loadTimers({ manual: true });
      });

      deleteAllButton.addEventListener("click", async () => {
        try {
          const response = await fetch("/api/timers", {
            method: "DELETE"
          });

          if (!response.ok) throw new Error(\`HTTP \${response.status}\`);
          state.rows = [];
          renderRows();
          errorEl.textContent = "";
        } catch (error) {
          errorEl.textContent = String(error?.message || error);
        }
      });

      setInterval(() => {
        state.uiNowMs = Date.now();
        renderStatus();
        updateAgeCells();
      }, 1000);

      setInterval(() => {
        loadTimers();
      }, 10000);

      state.uiNowMs = Date.now();
      renderStatus();
      renderRows();
      loadLabels();
      loadTimers();
    </script>
  </body>
</html>`;

function jsonResponse(data: unknown, init?: ResponseInit) {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: {
      "content-type": "application/json; charset=utf-8",
      ...(init?.headers || {})
    }
  });
}

Bun.serve({
  port: PORT,
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/api/timers") {
      if (request.method === "DELETE") {
        try {
          const upstream = await fetch(TIMERS_URL, { method: "DELETE" });
          return new Response(null, {
            status: upstream.status || 204
          });
        } catch (error) {
          return jsonResponse(
            { error: "Failed to delete all timers", detail: String(error) },
            { status: 502 }
          );
        }
      }

      try {
        const upstream = await fetch(TIMERS_URL, { method: "GET" });
        const body = await upstream.text();

        return new Response(body, {
          status: upstream.status,
          headers: {
            "content-type":
              upstream.headers.get("content-type") || "application/json; charset=utf-8"
          }
        });
      } catch (error) {
        return jsonResponse(
          { error: "Failed to fetch timers", detail: String(error) },
          { status: 502 }
        );
      }
    }

    if (url.pathname === "/api/timer" && request.method === "DELETE") {
      try {
        const timerId = url.searchParams.get("id");
        if (!timerId) {
          return jsonResponse({ error: "Missing id query parameter" }, { status: 400 });
        }

        const upstreamUrl = new URL(TIMER_URL);
        upstreamUrl.searchParams.set("id", timerId);
        const upstream = await fetch(upstreamUrl, { method: "DELETE" });

        return new Response(null, {
          status: upstream.status || 204
        });
      } catch (error) {
        return jsonResponse(
          { error: "Failed to delete timer", detail: String(error) },
          { status: 502 }
        );
      }
    }

    if (url.pathname === "/api/labels") {
      try {
        const upstreamInit: RequestInit = {
          method: request.method,
          headers: {}
        };

        if (request.method !== "GET" && request.method !== "HEAD") {
          upstreamInit.body = await request.text();
          upstreamInit.headers = {
            "content-type": request.headers.get("content-type") || "application/json"
          };
        }

        const upstream = await fetch(LABELS_URL, upstreamInit);
        const body = await upstream.text();

        return new Response(body, {
          status: upstream.status,
          headers: {
            "content-type":
              upstream.headers.get("content-type") || "application/json; charset=utf-8"
          }
        });
      } catch (error) {
        return jsonResponse(
          { error: "Failed to proxy labels", detail: String(error) },
          { status: 502 }
        );
      }
    }

    if (url.pathname === "/") {
      return new Response(html, {
        headers: {
          "content-type": "text/html; charset=utf-8"
        }
      });
    }

    return new Response("Not found", { status: 404 });
  }
});

console.log(`Remote Timers app running on http://localhost:${PORT}`);
console.log(`Proxying timer requests to ${TIMERS_URL}`);
console.log(`Proxying label requests to ${LABELS_URL}`);
