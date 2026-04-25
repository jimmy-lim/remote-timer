export {};

const PORT = Number(process.env.PORT || 3000);
const API_BASE_URL = process.env.API_BASE_URL || "http://127.0.0.1:3001";
const TIMERS_URL =
  process.env.TIMERS_URL || `${API_BASE_URL.replace(/\/$/, "")}/api/timers`;
const TIMER_URL =
  process.env.TIMER_URL ||
  (TIMERS_URL.endsWith("/timers")
    ? `${TIMERS_URL.slice(0, -"/timers".length)}/timer`
    : `${TIMERS_URL.replace(/\/$/, "")}/timer`);
const ENRICH_URL =
  process.env.ENRICH_URL ||
  `${API_BASE_URL.replace(/\/$/, "")}/api/enrich`;

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
        justify-content: flex-start;
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
        margin-right: auto;
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
        padding: 3px 4px;
        font: inherit;
      }

      table.edit-mode .label-input {
        padding: 4px 5px;
      }

      table.edit-mode td {
        padding-top: 14px;
        padding-bottom: 14px;
      }

      table.edit-mode th.non-edit-col,
      table.edit-mode td.non-edit-col {
        display: none;
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

      tr.timer-overdue td {
        background: #ffd1c7;
        color: #7f1d1d;
      }

      @media (max-width: 700px) {
        main {
          position: relative;
          padding: 16px 12px 24px;
        }

        h1 {
          margin-right: 160px;
        }

        .lede,
        .status-grid,
        .error,
        th.mobile-hide,
        td.mobile-hide {
          display: none;
        }

        .actions {
          display: flex;
          gap: 6px;
          align-items: center;
          position: absolute;
          top: 14px;
          right: 12px;
          margin: 0;
        }

        .actions .hint {
          display: none;
        }

        #delete-all-button {
          display: none !important;
        }

        .actions button {
          padding: 7px 9px;
          font-size: 0.8rem;
        }

        .label-input {
          padding: 7px 9px;
          font-size: 0.88rem;
        }

        th[data-col="label"],
        td[data-col="label"] {
          width: 68%;
        }

        th[data-col="elapsed"],
        td[data-col="elapsed"] {
          width: 32%;
          text-align: right;
        }
      }
    </style>
  </head>
  <body>
    <main>
      <h1>Remote Timers</h1>

      <section class="status-grid">
        <article class="card">
          <span class="label">Now</span>
          <div class="value" id="now">-</div>
        </article>
        <article class="card">
          <span class="label">Last API Refresh (10s)</span>
          <div class="value" id="last-refresh">never</div>
        </article>
      </section>

      <div class="actions">
        <span class="hint">Proxy target: <code>${TIMERS_URL}</code></span>
        <button id="delete-all-button" class="danger" type="button">Delete All Timers</button>
        <button id="edit-button" type="button">Edit</button>
        <button id="save-button" type="button" style="display:none">Save</button>
        <button id="cancel-button" type="button" style="display:none">Cancel</button>
      </div>

      <table id="timers-table">
        <thead>
          <tr>
            <th class="mobile-hide">ID</th>
            <th class="mobile-hide">Order</th>
            <th data-col="label">Label</th>
            <th class="mobile-hide">Notes</th>
            <th class="mobile-hide">Alert After</th>
            <th class="mobile-hide">Alert Interval</th>
            <th class="mobile-hide">Muted</th>
            <th class="mobile-hide non-edit-col">Updated At</th>
            <th class="non-edit-col" data-col="elapsed">Elapsed</th>
            <th class="mobile-hide non-edit-col">Timer</th>
          </tr>
        </thead>
        <tbody id="rows">
          <tr>
            <td colspan="10">Loading…</td>
          </tr>
        </tbody>
      </table>

      <div id="error" class="error"></div>
    </main>

    <script type="module">
      const state = {
        rows: [],
        enrich: {},
        editDraft: {},
        editMode: false,
        savingEdits: false,
        uiNowMs: Date.now(),
        lastRefreshMs: null,
        loading: false
      };

      const nowEl = document.querySelector("#now");
      const lastRefreshEl = document.querySelector("#last-refresh");
      const errorEl = document.querySelector("#error");
      const rowsEl = document.querySelector("#rows");
      const tableEl = document.querySelector("#timers-table");
      const editButton = document.querySelector("#edit-button");
      const saveButton = document.querySelector("#save-button");
      const cancelButton = document.querySelector("#cancel-button");
      const deleteAllButton = document.querySelector("#delete-all-button");

      function normalize(raw) {
        if (Array.isArray(raw)) {
          return raw;
        }

        if (raw && Array.isArray(raw.timers)) return raw.timers;
        if (raw && Array.isArray(raw.data)) return raw.data;

        if (raw && typeof raw === "object") {
          return Object.entries(raw).map(([id, timestamp]) => ({ id, timestamp }));
        }

        return [];
      }

      function parseDurationToMs(value) {
        if (typeof value !== "string") return null;
        const trimmed = value.trim();
        const match = trimmed.match(/^([0-9]+)\s*([smhdw]|mo)$/i);
        if (!match) return null;
        const amount = Number.parseInt(match[1], 10);
        if (!Number.isFinite(amount) || amount <= 0) return null;
        const unit = match[2].toLowerCase();
        const multipliers = {
          s: 1000,
          m: 60 * 1000,
          h: 60 * 60 * 1000,
          d: 24 * 60 * 60 * 1000,
          w: 7 * 24 * 60 * 60 * 1000,
          mo: 30 * 24 * 60 * 60 * 1000
        };
        return amount * multipliers[unit];
      }

      function parseOrder(value) {
        if (typeof value === "number" && Number.isFinite(value)) return Math.trunc(value);
        if (typeof value !== "string") return null;
        const trimmed = value.trim();
        if (!trimmed || !/^[0-9]+$/.test(trimmed)) return null;
        return Number.parseInt(trimmed, 10);
      }

      function normalizeEnrichMap(raw) {
        if (!Array.isArray(raw)) return {};
        return Object.fromEntries(
          raw
            .filter((entry) => entry && typeof entry === "object" && typeof entry.id === "string")
            .map((entry) => {
              const normalized = {};
              if (typeof entry.label === "string") normalized.label = entry.label;
              if (typeof entry.notes === "string") normalized.notes = entry.notes;
              if (typeof entry.alertAfter === "string") normalized.alertAfter = entry.alertAfter;
              if (typeof entry.alertInterval === "string") normalized.alertInterval = entry.alertInterval;
              if (typeof entry.muted === "boolean") normalized.muted = entry.muted;
              if (typeof entry.order === "string" || typeof entry.order === "number") {
                normalized.order = String(entry.order);
              }
              return [entry.id, normalized];
            })
        );
      }

      function buildDraftFromEnrich() {
        return Object.fromEntries(
          Object.entries(state.enrich).map(([id, value]) => [
            id,
            {
              label: typeof value.label === "string" ? value.label : "",
              notes: typeof value.notes === "string" ? value.notes : "",
              alertAfter: typeof value.alertAfter === "string" ? value.alertAfter : "",
              alertInterval: typeof value.alertInterval === "string" ? value.alertInterval : "",
              muted: typeof value.muted === "boolean" ? value.muted : false,
              order: typeof value.order === "string" ? value.order : ""
            }
          ])
        );
      }

      function ensureDraftRows() {
        state.rows.forEach((row, index) => {
          const id = row.id ?? row.timerId ?? \`row-\${index + 1}\`;
          if (!state.editDraft[id]) {
            const current = state.enrich[id] || {};
            state.editDraft[id] = {
              label: typeof current.label === "string" ? current.label : "",
              notes: typeof current.notes === "string" ? current.notes : "",
              alertAfter: typeof current.alertAfter === "string" ? current.alertAfter : "",
              alertInterval: typeof current.alertInterval === "string" ? current.alertInterval : "",
              muted: typeof current.muted === "boolean" ? current.muted : false,
              order: typeof current.order === "string" ? current.order : ""
            };
          }
        });
      }

      function mergeEnrichForId(id) {
        return {
          ...(state.enrich[id] || {}),
          ...(state.editMode ? state.editDraft[id] || {} : {})
        };
      }

      function updateEditButtons() {
        tableEl?.classList.toggle("edit-mode", state.editMode);
        editButton.style.display = state.editMode ? "none" : "";
        saveButton.style.display = state.editMode ? "" : "none";
        cancelButton.style.display = state.editMode ? "" : "none";
        saveButton.disabled = state.savingEdits;
        cancelButton.disabled = state.savingEdits;
        deleteAllButton.disabled = state.editMode;
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
          const enrich = mergeEnrichForId(id);
          const ts = Number(row.timestamp ?? row.ts ?? row.lastPress ?? 0);
          const validTs = Number.isFinite(ts) ? ts : 0;
          const ageSec = validTs > 0 ? Math.max(0, Math.floor((state.uiNowMs - validTs) / 1000)) : null;
          const alertAfterMs = parseDurationToMs(enrich.alertAfter);
          const elapsedMs = validTs > 0 ? Math.max(0, state.uiNowMs - validTs) : 0;

          return {
            id,
            label: typeof enrich.label === "string" ? enrich.label : "",
            notes: typeof enrich.notes === "string" ? enrich.notes : "",
            alertAfter: typeof enrich.alertAfter === "string" ? enrich.alertAfter : "",
            alertInterval: typeof enrich.alertInterval === "string" ? enrich.alertInterval : "",
            muted: typeof enrich.muted === "boolean" ? enrich.muted : false,
            order: typeof enrich.order === "string" ? enrich.order : "",
            alertAfterMs,
            isOverdue: alertAfterMs !== null && elapsedMs > alertAfterMs,
            timestampMs: validTs,
            updatedAt: validTs > 0 ? new Date(validTs).toLocaleString() : "",
            age: humanAge(ageSec)
          };
        }).sort((a, b) => {
          const aOrder = parseOrder(a.order);
          const bOrder = parseOrder(b.order);
          if (aOrder !== null && bOrder !== null && aOrder !== bOrder) return aOrder - bOrder;
          if (aOrder !== null && bOrder === null) return -1;
          if (aOrder === null && bOrder !== null) return 1;
          return String(a.id).localeCompare(String(b.id), undefined, { numeric: true, sensitivity: "base" });
        });
      }

      function renderRows() {
        const displayRows = getDisplayRows();
        updateEditButtons();

        if (displayRows.length === 0) {
          rowsEl.innerHTML = '<tr><td colspan="10">No timers returned.</td></tr>';
          return;
        }

        rowsEl.innerHTML = displayRows.map((row) => \`
          <tr class="\${row.isOverdue ? "timer-overdue" : ""}" data-alert-after-ms="\${row.alertAfterMs ?? 0}">
            <td class="mobile-hide" data-label="ID">\${escapeHtml(row.id)}</td>
            <td class="mobile-hide" data-label="Order">
              \${state.editMode
                ? \`<input class="label-input enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="order" type="text" value="\${escapeHtml(state.editDraft[row.id]?.order ?? row.order ?? "")}" placeholder="1,2,3..." />\`
                : escapeHtml(row.order || "-")}
            </td>
            <td data-col="label" data-label="Label">
              <div class="label-cell">
                \${state.editMode ? \`
                  <input class="label-input enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="label" type="text" value="\${escapeHtml(state.editDraft[row.id]?.label ?? row.label ?? "")}" placeholder="Label" />
                \` : \`<span>\${escapeHtml(row.label || "-")}</span>\`}
              </div>
            </td>
            <td class="mobile-hide" data-label="Notes">
              \${state.editMode
                ? \`<input class="label-input enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="notes" type="text" value="\${escapeHtml(state.editDraft[row.id]?.notes ?? row.notes ?? "")}" placeholder="Notes" />\`
                : escapeHtml(row.notes || "-")}
            </td>
            <td class="mobile-hide" data-label="Alert After">
              \${state.editMode
                ? \`<input class="label-input enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="alertAfter" type="text" value="\${escapeHtml(state.editDraft[row.id]?.alertAfter ?? row.alertAfter ?? "")}" placeholder="5m, 3h, 1d, 2mo" />\`
                : escapeHtml(row.alertAfter || "-")}
            </td>
            <td class="mobile-hide" data-label="Alert Interval">
              \${state.editMode
                ? \`<input class="label-input enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="alertInterval" type="text" value="\${escapeHtml(state.editDraft[row.id]?.alertInterval ?? row.alertInterval ?? "")}" placeholder="5m, 1h, 1d" />\`
                : escapeHtml(row.alertInterval || "-")}
            </td>
            <td class="mobile-hide" data-label="Muted">
              \${state.editMode
                ? \`<label><input class="enrich-input" data-timer-id="\${escapeHtml(row.id)}" data-field="muted" type="checkbox" \${(state.editDraft[row.id]?.muted ?? row.muted) ? "checked" : ""} /> muted</label>\`
                : escapeHtml(row.muted ? "true" : "false")}
            </td>
            <td class="mobile-hide non-edit-col" data-label="Updated At">\${escapeHtml(row.updatedAt)}</td>
            <td class="non-edit-col" data-col="elapsed" data-label="Elapsed">
              <span
                data-age-timer-id="\${escapeHtml(row.id)}"
                data-timestamp-ms="\${escapeHtml(String(row.timestampMs || 0))}"
              >\${escapeHtml(row.age)}</span>
            </td>
            <td class="mobile-hide non-edit-col" data-label="Timer">
              <button
                class="timer-delete"
                data-delete-single-timer-id="\${escapeHtml(row.id)}"
                type="button"
                \${state.editMode ? "disabled" : ""}
              >
                Delete
              </button>
            </td>
          </tr>
        \`).join("");

        rowsEl.querySelectorAll(".enrich-input").forEach((input) => {
          input.addEventListener("input", (event) => {
            const target = event.currentTarget;
            const timerId = target.dataset.timerId;
            const field = target.dataset.field;
            if (!timerId || !field) return;
            state.editDraft[timerId] = {
              ...(state.editDraft[timerId] || {}),
              [field]: field === "muted" ? Boolean(target.checked) : target.value
            };
          });
        });

        rowsEl.querySelectorAll(".timer-delete").forEach((button) => {
          button.addEventListener("click", async (event) => {
            const target = event.currentTarget;
            const timerId = target.dataset.deleteSingleTimerId;
            if (!timerId || state.editMode) return;

            try {
              const response = await fetch(\`/api/timer?id=\${encodeURIComponent(timerId)}\`, {
                method: "DELETE"
              });

              if (!response.ok) throw new Error(\`HTTP \${response.status}\`);
              state.rows = state.rows.filter((row, index) => {
                const id = row.id ?? row.timerId ?? \`row-\${index + 1}\`;
                return id !== timerId;
              });
              delete state.enrich[timerId];
              delete state.editDraft[timerId];
              renderRows();
              errorEl.textContent = "";
            } catch (error) {
              errorEl.textContent = String(error?.message || error);
            }
          });
        });
      }
      function updateAgeCells() {
        rowsEl.querySelectorAll("[data-age-timer-id]").forEach((node) => {
          const timestampMs = Number(node.dataset.timestampMs || 0);
          const row = node.closest("tr");
          const alertAfterMs = Number(row?.dataset?.alertAfterMs || 0);
          const ageSec = timestampMs > 0
            ? Math.max(0, Math.floor((state.uiNowMs - timestampMs) / 1000))
            : null;
          node.textContent = humanAge(ageSec);
          if (row) {
            const elapsedMs = timestampMs > 0 ? Math.max(0, state.uiNowMs - timestampMs) : 0;
            const isOverdue = alertAfterMs > 0 && timestampMs > 0 && elapsedMs > alertAfterMs;
            row.classList.toggle("timer-overdue", isOverdue);
          }
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

      async function loadTimers() {
        if (state.editMode) return;
        if (state.loading) return;

        state.loading = true;
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
          updateEditButtons();
        }
      }

      async function loadEnrich() {
        try {
          const response = await fetch("/api/enrich");
          if (!response.ok) throw new Error(\`HTTP \${response.status}\`);

          const json = await response.json();
          state.enrich = normalizeEnrichMap(json);
          renderRows();
        } catch (error) {
          errorEl.textContent = String(error?.message || error);
        }
      }

      async function saveEditDraft() {
        const payload = Object.entries(state.editDraft).map(([id, value]) => {
          return {
            id,
            label: value.label ?? "",
            notes: value.notes ?? "",
            alertAfter: value.alertAfter ?? "",
            alertInterval: value.alertInterval ?? "",
            muted: Boolean(value.muted),
            order: value.order ?? ""
          };
        });

        state.savingEdits = true;
        updateEditButtons();
        errorEl.textContent = "";
        try {
          const response = await fetch("/api/enrich", {
            method: "POST",
            headers: {
              "content-type": "application/json"
            },
            body: JSON.stringify(payload)
          });
          if (!response.ok) throw new Error(\`HTTP \${response.status}\`);
          const json = await response.json();
          state.enrich = normalizeEnrichMap(json);
          state.editDraft = {};
          state.editMode = false;
          renderRows();
          errorEl.textContent = "";
        } catch (error) {
          errorEl.textContent = String(error?.message || error);
        } finally {
          state.savingEdits = false;
          updateEditButtons();
        }
      }

      editButton.addEventListener("click", () => {
        state.editMode = true;
        state.editDraft = buildDraftFromEnrich();
        ensureDraftRows();
        renderRows();
      });

      cancelButton.addEventListener("click", () => {
        if (state.savingEdits) return;
        state.editMode = false;
        state.editDraft = {};
        renderRows();
      });

      saveButton.addEventListener("click", () => {
        if (state.savingEdits) return;
        saveEditDraft();
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
        if (!state.editMode && !state.savingEdits) {
          loadTimers();
        }
      }, 10000);

      state.uiNowMs = Date.now();
      renderStatus();
      renderRows();
      loadEnrich();
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

    if (url.pathname === "/api/enrich") {
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

        const upstream = await fetch(ENRICH_URL, upstreamInit);
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
          { error: "Failed to proxy enrich data", detail: String(error) },
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
console.log(`Proxying enrich requests to ${ENRICH_URL}`);
