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

  dom.indexForm.addEventListener("submit", (event) => {
    event.preventDefault();
    runIndex();
  });

  pollHealth();
  setInterval(pollHealth, HEALTH_POLL_MS);
  refreshStats();
}

document.addEventListener("DOMContentLoaded", init);
