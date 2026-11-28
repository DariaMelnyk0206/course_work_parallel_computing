"use strict";

// Web UI for the search engine. All server data reaches the DOM through
// textContent only (setText); HTML injection APIs are never used.

const HEALTH_POLL_MS = 5000;
const HEALTH_TIMEOUT_MS = 3000;

const dom = {};

function setText(element, text) {
  element.textContent = String(text);
}

function setIndicator(indicator, valueElement, state, text) {
  indicator.dataset.state = state;
  setText(valueElement, text);
}

// Extracts the server's `error` field from a failed response, if present.
async function readErrorText(response) {
  try {
    const body = await response.json();
    if (body && typeof body.error === "string") {
      return body.error;
    }
  } catch (e) {
    // Body is not JSON; fall through to the generic text.
  }
  return "HTTP " + response.status;
}

async function pollHealth() {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), HEALTH_TIMEOUT_MS);
  try {
    const response = await fetch("/api/health", { signal: controller.signal });
    if (!response.ok) {
      throw new Error("HTTP " + response.status);
    }
    const body = await response.json();
    setIndicator(dom.serverIndicator, dom.serverStatus, "ok", "працює");
    if (body.ollama === true) {
      setIndicator(dom.ollamaIndicator, dom.ollamaStatus, "ok", "доступна");
    } else {
      setIndicator(dom.ollamaIndicator, dom.ollamaStatus, "fail",
                   "недоступна");
    }
  } catch (e) {
    setIndicator(dom.serverIndicator, dom.serverStatus, "fail", "недоступний");
    setIndicator(dom.ollamaIndicator, dom.ollamaStatus, "unknown", "невідомо");
  } finally {
    clearTimeout(timer);
  }
}

async function refreshStats() {
  try {
    const response = await fetch("/api/stats");
    if (!response.ok) {
      return;
    }
    const body = await response.json();
    setText(dom.statDocuments, body.documents);
    setText(dom.statBuildTime, body.index_build_time_ms);
    setText(dom.statThreads, body.threads_used);
  } catch (e) {
    // Keep the previous values; the health indicator reports connectivity.
  }
}

function showIndexResult(text, isError) {
  setText(dom.indexResult, text);
  dom.indexResult.classList.toggle("error-text", isError);
}

// POST /api/index without any client-side timeout: indexing is synchronous
// and may take minutes on the full dataset.
async function runIndex() {
  const threads = Number(dom.indexThreads.value);
  dom.indexButton.disabled = true;
  showIndexResult("Індексація виконується...", false);
  try {
    const response = await fetch("/api/index", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ threads: threads }),
    });
    if (response.ok) {
      const body = await response.json();
      showIndexResult(
          "Проіндексовано документів: " + body.documents_indexed +
              ", час: " + body.time_ms + " мс",
          false);
      await refreshStats();
    } else {
      showIndexResult(await readErrorText(response), true);
    }
  } catch (e) {
    showIndexResult("Помилка з'єднання з сервером", true);
  } finally {
    dom.indexButton.disabled = false;
  }
}

// ---- Search over WebSocket with pipeline stage visualization ----

const STAGES = ["query_expansion", "keyword_search", "done"];
const STATE_LABELS = {
  pending: "очікування",
  running: "виконується",
  done: "завершено",
  error: "помилка",
};

// State of the current search; `socket` is null when no search is active.
const search = { socket: null, sentAt: 0, finished: false };

function stageElement(name) {
  return document.getElementById("stage-" + name);
}

function getStageState(name) {
  return stageElement(name).dataset.state;
}

function setStage(name, state, ms) {
  const element = stageElement(name);
  element.dataset.state = state;
  setText(element.querySelector(".stage-state"), STATE_LABELS[state]);
  setText(element.querySelector(".stage-time"),
          ms === undefined ? "" : ms + " мс");
}

function showError(text) {
  setText(dom.errorArea, text);
}

function renderTerms(terms) {
  dom.termsList.replaceChildren();
  for (const term of terms) {
    const item = document.createElement("li");
    setText(item, term);
    dom.termsList.appendChild(item);
  }
}

// Shows "Показано N з M" above the results; empty when there is no summary.
function renderSummary(shown, total) {
  if (shown === null || total === null) {
    setText(dom.resultsSummary, "");
    return;
  }
  setText(dom.resultsSummary, "Показано " + shown + " з " + total);
}
function renderResults(results) {
  dom.resultsList.replaceChildren();
  for (const result of results) {
    const item = document.createElement("li");
    const id = document.createElement("span");
    id.className = "doc-id";
    setText(id, result.doc_id);
    const score = document.createElement("span");
    score.className = "score";
    setText(score, "score: " + Number(result.score).toFixed(4));
    const snippet = document.createElement("p");
    snippet.className = "snippet";
    setText(snippet, result.snippet);
    item.append(id, score, snippet);
    dom.resultsList.appendChild(item);
  }
}

function resetPipeline() {
  for (const name of STAGES) {
    setStage(name, "pending");
  }
  renderTerms([]);
  renderResults([]);
  renderSummary(null, null);
  showError("");
}

// Marks the stage that is currently running as failed; falls back to
// query_expansion when no stage was running (e.g. connection never opened).
function failRunningStage() {
  const running = STAGES.find((name) => getStageState(name) === "running");
  setStage(running || STAGES[0], "error");
}

function closeSocket() {
  const socket = search.socket;
  search.socket = null;
  if (socket) {
    socket.close();
  }
}

function handleMessage(message) {
  const data = message.data || {};
  switch (message.stage) {
    case "query_expansion":
      setStage("query_expansion", "done", Date.now() - search.sentAt);
      setStage("keyword_search", "running");
      renderTerms(Array.isArray(data.expanded_terms) ? data.expanded_terms
                                                     : []);
      break;
    case "keyword_search":
      setStage("keyword_search", "done", data.time_ms);
      setStage("done", "running");
      {
        const results = Array.isArray(data.results) ? data.results : [];
        renderResults(results);
        const total = Number.isInteger(data.total_matches)
                          ? data.total_matches
                          : results.length;
        renderSummary(results.length, total);
      }
      break;
    case "done":
      setStage("done", "done", data.total_time_ms);
      search.finished = true;
      closeSocket();
      dom.searchButton.disabled = false;
      break;
    case "error":
      failRunningStage();
      showError(typeof data.message === "string" ? data.message
                                                 : "помилка сервера");
      search.finished = true;
      closeSocket();
      dom.searchButton.disabled = false;
      break;
    default:
      break;
  }
}

function openSocket(query) {
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(scheme + "//" + location.host + "/ws/search");
  search.socket = socket;
  search.finished = false;

  // A connection drop before `done`/`error` is reported once per socket;
  // events from superseded sockets are ignored.
  const onDrop = () => {
    if (search.socket !== socket || search.finished) {
      return;
    }
    search.finished = true;
    search.socket = null;
    failRunningStage();
    showError("з'єднання розірвано");
    dom.searchButton.disabled = false;
  };

  socket.onopen = () => {
    search.sentAt = Date.now();
    setStage("query_expansion", "running");
    socket.send(JSON.stringify({ query: query }));
  };
  socket.onmessage = (event) => {
    if (search.socket !== socket) {
      return;
    }
    let message;
    try {
      message = JSON.parse(event.data);
    } catch (e) {
      return;
    }
    if (message && typeof message === "object") {
      handleMessage(message);
    }
  };
  socket.onerror = onDrop;
  socket.onclose = onDrop;
}

function runSearch(query) {
  const trimmed = query.trim();
  if (trimmed === "") {
    resetPipeline();
    showError("Запит не може бути порожнім");
    return;
  }
  closeSocket();
  resetPipeline();
  dom.searchButton.disabled = true;
  openSocket(trimmed);
}

function init() {
  dom.serverIndicator = document.getElementById("server-indicator");
  dom.serverStatus = document.getElementById("server-status");
  dom.ollamaIndicator = document.getElementById("ollama-indicator");
  dom.ollamaStatus = document.getElementById("ollama-status");
  dom.statDocuments = document.getElementById("stat-documents");
  dom.statBuildTime = document.getElementById("stat-build-time");
  dom.statThreads = document.getElementById("stat-threads");
  dom.indexForm = document.getElementById("index-form");
  dom.indexThreads = document.getElementById("index-threads");
  dom.indexButton = document.getElementById("index-button");
  dom.indexResult = document.getElementById("index-result");

  dom.searchForm = document.getElementById("search-form");
  dom.searchQuery = document.getElementById("search-query");
  dom.searchButton = document.getElementById("search-button");
  dom.errorArea = document.getElementById("error-area");
  dom.termsList = document.getElementById("terms-list");
  dom.resultsList = document.getElementById("results-list");
  dom.resultsSummary = document.getElementById("results-summary");

  dom.searchForm.addEventListener("submit", (event) => {
    event.preventDefault();
    runSearch(dom.searchQuery.value);
  });

  dom.indexForm.addEventListener("submit", (event) => {
    event.preventDefault();
    runIndex();
  });

  pollHealth();
  setInterval(pollHealth, HEALTH_POLL_MS);
  refreshStats();
}

document.addEventListener("DOMContentLoaded", init);
