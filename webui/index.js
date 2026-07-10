/* SPDX-License-Identifier: MIT */
/*
 * YukiZygisk - Static WebUI control surface.
 * Derived from KOWX712/ksu-webui-demo and Kagami's static WebUI.
 * License: MIT
 * Authors: KOWX712 and Anatdx
 */

import { DEFAULT_CONFIG, DEFAULT_STATUS, PATHS, api, getRuntimeMode } from "./api.js";
import { enableEdgeToEdge, toast as nativeToast } from "./assets/kernelsu.js";
import { LANGUAGE_OPTIONS, getNavigatorLanguage, translations } from "./i18n.js";

const TABS = [
  { id: "overview", label: "nav.overview", icon: "pulse" },
  { id: "modules", label: "nav.modules", icon: "modules" },
  { id: "denylist", label: "nav.denylist", icon: "shield" },
  { id: "settings", label: "nav.settings", icon: "tune" },
  { id: "logs", label: "nav.logs", icon: "terminal" },
  { id: "about", label: "nav.about", icon: "info" },
];

const params = new URLSearchParams(globalThis.location?.search || "");
const requestedTab = params.get("tab");
const storedLanguage = localStorage.getItem("yukizygisk_language");

const state = {
  tab: TABS.some((tab) => tab.id === requestedTab) ? requestedTab : "overview",
  language: LANGUAGE_OPTIONS.some((option) => option.value === storedLanguage) ? storedLanguage : getNavigatorLanguage(),
  theme: localStorage.getItem("yukizygisk_theme") || "system",
  runtimeMode: getRuntimeMode(),
  status: { ...DEFAULT_STATUS },
  config: { ...DEFAULT_CONFIG },
  system: { kernel: "…", selinux: "…" },
  meta: { name: "YukiZygisk", version: "dev", author: "Anatdx" },
  packages: [],
  packagesLoaded: false,
  packageSearch: "",
  logs: "",
  logType: "system",
  logSearch: "",
  loading: true,
  refreshing: false,
  error: "",
  lastUpdated: "",
};

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function format(template, values = {}) {
  return String(template).replace(/\{(\w+)\}/g, (_match, key) => String(values[key] ?? `{${key}}`));
}

function t(key, fallback = key, values) {
  return format(translations[state.language]?.[key] ?? translations.en[key] ?? fallback, values);
}

function icon(name, className = "") {
  const paths = {
    pulse: '<path d="M3 12h4l2.2-6 4.1 12 2.3-6H21"/>',
    modules: '<path d="m12 2 8 4.5v9L12 20l-8-4.5v-9L12 2Zm0 0v18M4 6.5l8 4.5 8-4.5"/>',
    shield: '<path d="M12 3 4.5 6v5.3c0 4.7 3 8.1 7.5 9.7 4.5-1.6 7.5-5 7.5-9.7V6L12 3Zm-3 9 2 2 4-4"/>',
    tune: '<path d="M4 7h10m4 0h2M4 17h2m4 0h10M14 4v6M8 14v6"/>',
    terminal: '<path d="m5 7 4 4-4 4m7 0h7"/>',
    info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v.2"/>',
    refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7M20 5v6h-6"/>',
    theme: '<path d="M20 15.5A8.5 8.5 0 0 1 8.5 4 8.5 8.5 0 1 0 20 15.5Z"/>',
    search: '<circle cx="11" cy="11" r="6"/><path d="m16 16 4 4"/>',
    reload: '<path d="M5 8a8 8 0 0 1 13.5-1.5L21 9M19 16A8 8 0 0 1 5.5 17.5L3 15"/><path d="M21 4v5h-5M3 20v-5h5"/>',
    copy: '<rect x="8" y="8" width="11" height="11" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>',
    trash: '<path d="M4 7h16M9 7V4h6v3m3 0-1 13H7L6 7m4 4v5m4-5v5"/>',
    external: '<path d="M14 4h6v6m0-6-9 9"/><path d="M18 13v6H5V6h6"/>',
  };
  return `<svg class="icon ${className}" viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${paths[name] || paths.info}</svg>`;
}

function showToast(message, tone = "success") {
  nativeToast(message);
  const stack = document.getElementById("toast-stack");
  if (!stack)
    return;
  const item = document.createElement("div");
  item.className = `toast ${tone}`;
  item.textContent = message;
  stack.appendChild(item);
  requestAnimationFrame(() => item.classList.add("visible"));
  setTimeout(() => {
    item.classList.remove("visible");
    setTimeout(() => item.remove(), 220);
  }, 2600);
}

function applyTheme() {
  if (state.theme === "system")
    delete document.documentElement.dataset.theme;
  else
    document.documentElement.dataset.theme = state.theme;
}

function cycleTheme() {
  state.theme = state.theme === "system" ? "light" : state.theme === "light" ? "dark" : "system";
  localStorage.setItem("yukizygisk_theme", state.theme);
  applyTheme();
}

function statusPresentation() {
  if (state.status.safe_mode) {
    return { tone: "warning", title: t("status.safeMode"), description: t("status.safeModeDesc") };
  }
  if (state.status.available && state.status.kernel_alive) {
    return { tone: "success", title: t("status.running"), description: t("status.runningDesc") };
  }
  return { tone: "danger", title: t("status.unavailable"), description: state.status.error || t("status.unavailableDesc") };
}

function stateBadge(value) {
  const tones = { injected: "success", failed: "danger", crashed: "warning", unsupported32: "muted" };
  return `<span class="badge ${tones[value] || "muted"}">${escapeHtml(t(`state.${value}`, value))}</span>`;
}

function emptyState(message) {
  return `<div class="empty-state">${escapeHtml(message)}</div>`;
}

function metric(label, value, detail = "") {
  return `<div class="metric"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong>${detail ? `<small>${escapeHtml(detail)}</small>` : ""}</div>`;
}

function sectionHeader(title, description = "", action = "") {
  return `<div class="section-heading"><div><h2>${escapeHtml(title)}</h2>${description ? `<p>${escapeHtml(description)}</p>` : ""}</div>${action}</div>`;
}

function renderOverview() {
  const presentation = statusPresentation();
  const zygotes = state.status.zygote_monitor || [];
  const recent = state.status.recent || [];
  return `
    <div class="page-grid">
      <section class="hero-card ${presentation.tone}">
        <div class="hero-copy">
          <div class="eyebrow"><span class="status-dot"></span>${escapeHtml(t("status.runtime"))} · ${escapeHtml(state.status.abi || "arm64-v8a")}</div>
          <h1>${escapeHtml(presentation.title)}</h1>
          <p>${escapeHtml(presentation.description)}</p>
          <div class="hero-meta"><span>${escapeHtml(t("status.lastUpdate"))} ${escapeHtml(state.lastUpdated || "—")}</span><span>PID ${escapeHtml(state.status.daemon_pid || "—")}</span></div>
        </div>
        <div class="hero-actions">
          <button class="button secondary" data-action="reload">${icon("reload")} ${escapeHtml(t("common.reload"))}</button>
        </div>
      </section>

      ${state.status.safe_mode ? `<section class="alert-card warning"><strong>${escapeHtml(t("status.safeModeTitle"))}</strong><p>${escapeHtml(t("status.safeModeBody"))}</p><span>${escapeHtml(state.status.safe_mode_zygote)} · ${state.status.zygote_crashes} restart(s)</span></section>` : ""}

      <section class="metrics-grid">
        ${metric(t("status.injectCount"), Number(state.status.count || 0).toLocaleString())}
        ${metric(t("status.zygiskModules"), String(state.status.modules.length))}
        ${metric(t("status.nativeModules"), String(state.status.native_modules.length))}
        ${metric(t("status.zygotes"), String(zygotes.length))}
      </section>

      <section class="panel span-2">
        ${sectionHeader(t("status.zygoteMonitor"), t("status.zygoteMonitorDesc"))}
        <div class="data-list">
          ${zygotes.length ? zygotes.map((item) => `<div class="data-row"><div class="identity"><strong>${escapeHtml(item.name)}</strong><span>PID ${escapeHtml(item.pid)}</span></div><code>${escapeHtml(item.abi)}</code>${stateBadge(item.state)}</div>`).join("") : emptyState(t("common.none"))}
        </div>
      </section>

      <section class="panel">
        ${sectionHeader(t("status.recentApps"), t("status.recentAppsDesc"))}
        <div class="chip-cloud">${recent.length ? recent.map((appid) => `<span class="chip">${escapeHtml(appid)}</span>`).join("") : emptyState(t("common.none"))}</div>
      </section>
    </div>`;
}

function renderModules() {
  const standard = state.status.modules || [];
  const nativeModules = state.status.native_modules || [];
  const injections = state.status.native_injections || [];
  return `<div class="page-grid">
    <section class="panel span-2">
      ${sectionHeader(t("modules.standard"), t("modules.standardDesc"))}
      <div class="module-grid">${standard.length ? standard.map((name) => `<article class="module-card"><div class="module-mark">Z</div><div><strong>${escapeHtml(name)}</strong><span>Zygisk API</span></div><span class="badge success">${escapeHtml(t("state.injected"))}</span></article>`).join("") : emptyState(t("modules.noStandard"))}</div>
    </section>
    <section class="panel span-2">
      ${sectionHeader(t("modules.native"), t("modules.nativeDesc"))}
      <div class="module-grid">${nativeModules.length ? nativeModules.map((module) => `<article class="module-card native"><div class="module-mark">N</div><div><strong>${escapeHtml(module.id)}</strong><span>${escapeHtml(module.target_type)}=${escapeHtml(module.target)}</span>${module.companion ? `<small>${escapeHtml(t("modules.companion"))}</small>` : ""}</div>${stateBadge(module.state)}</article>`).join("") : emptyState(t("modules.noNative"))}</div>
    </section>
    <section class="panel span-2">
      ${sectionHeader(t("modules.injections"), t("modules.injectionsDesc"))}
      <div class="data-list">${injections.length ? injections.map((item) => `<div class="data-row native-row"><div class="identity"><strong>${escapeHtml(item.process)}</strong><span>${escapeHtml(item.module)} · ${escapeHtml(item.target_type)}=${escapeHtml(item.target)}</span></div><code>PID ${escapeHtml(item.pid)}</code>${stateBadge(item.state)}</div>`).join("") : emptyState(t("modules.noInjections"))}</div>
    </section>
  </div>`;
}

function filteredPackages() {
  const needle = state.packageSearch.trim().toLowerCase();
  if (!needle)
    return state.packages;
  return state.packages.filter((item) => item.packageName.toLowerCase().includes(needle) || String(item.appId).includes(needle));
}

function renderPackageList() {
  if (!state.packagesLoaded)
    return emptyState(t("denylist.noApps"));
  const packages = filteredPackages();
  if (!packages.length)
    return emptyState(t("denylist.noMatch"));
  const selected = new Set(state.config.denylist_app_ids.map(Number));
  const visible = packages.slice(0, 250);
  return `${visible.map((item) => `<label class="package-row"><input type="checkbox" data-appid="${item.appId}" ${selected.has(item.appId) ? "checked" : ""}><span class="package-check"></span><span class="package-identity"><strong>${escapeHtml(item.packageName)}</strong><small>UID ${item.uid} · App ID ${item.appId}</small></span></label>`).join("")}${packages.length > visible.length ? `<div class="list-note">Showing 250 of ${packages.length}; refine the search.</div>` : ""}`;
}

function denylistModeDescription() {
  if (state.config.denylist_mode === 1)
    return t("denylist.skipDesc");
  if (state.config.denylist_mode === 2)
    return t("denylist.revertDesc");
  return t("denylist.desc");
}

function renderDenylist() {
  return `<div class="page-grid">
    <section class="panel span-2">
      ${sectionHeader(t("denylist.title"), denylistModeDescription())}
      <div class="field-row">
        <label class="field grow"><span>${escapeHtml(t("denylist.mode"))}</span><select id="denylist-mode"><option value="0" ${state.config.denylist_mode === 0 ? "selected" : ""}>${escapeHtml(t("denylist.off"))}</option><option value="1" ${state.config.denylist_mode === 1 ? "selected" : ""}>${escapeHtml(t("denylist.skip"))}</option><option value="2" ${state.config.denylist_mode === 2 ? "selected" : ""}>${escapeHtml(t("denylist.revert"))}</option></select></label>
        <div class="selection-counter" id="selection-counter">${escapeHtml(t("denylist.selected", "", { count: state.config.denylist_app_ids.length }))}</div>
      </div>
    </section>
    <section class="panel span-2">
      ${sectionHeader(t("denylist.apps"), t("denylist.appsDesc"), `<button class="button secondary compact" data-action="load-packages">${escapeHtml(t("denylist.loadApps"))}</button>`)}
      <div class="search-box">${icon("search")}<input id="package-search" type="search" value="${escapeHtml(state.packageSearch)}" placeholder="${escapeHtml(t("denylist.search"))}"></div>
      <p class="hint">${escapeHtml(t("denylist.sharedUid"))}</p>
      <div class="package-list" id="package-list">${renderPackageList()}</div>
      <div class="panel-actions"><button class="button primary" data-action="save-config">${escapeHtml(t("common.save"))}</button></div>
    </section>
  </div>`;
}

function switchCard(id, title, description, checked) {
  return `<label class="switch-card"><span><strong>${escapeHtml(title)}</strong><small>${escapeHtml(description)}</small></span><input id="${id}" type="checkbox" ${checked ? "checked" : ""}><span class="switch"><i></i></span></label>`;
}

function renderSettings() {
  return `<div class="page-grid">
    <section class="panel span-2">
      ${sectionHeader(t("settings.title"), t("settings.desc"))}
      <div class="switch-list">
        ${switchCard("setting-yukilinker", t("settings.yukilinker"), t("settings.yukilinkerDesc"), state.config.yukilinker)}
        ${switchCard("setting-dmesg", t("settings.dmesg"), t("settings.dmesgDesc"), state.config.dmesg_log)}
      </div>
      <div class="panel-actions"><button class="button primary" data-action="save-config">${escapeHtml(t("common.save"))}</button></div>
    </section>
    <section class="panel span-2">
      ${sectionHeader(t("settings.runtimeInfo"))}
      <dl class="info-grid">
        <div><dt>${escapeHtml(t("settings.kernel"))}</dt><dd>${escapeHtml(state.system.kernel)}</dd></div>
        <div><dt>${escapeHtml(t("settings.selinux"))}</dt><dd>${escapeHtml(state.system.selinux)}</dd></div>
        <div><dt>${escapeHtml(t("settings.daemonPid"))}</dt><dd>${escapeHtml(state.status.daemon_pid || "—")}</dd></div>
        <div><dt>${escapeHtml(t("settings.configPath"))}</dt><dd><code>${escapeHtml(PATHS.CONFIG)}</code></dd></div>
      </dl>
    </section>
  </div>`;
}

function filteredLogLines() {
  const needle = state.logSearch.trim().toLowerCase();
  const lines = String(state.logs || "").split(/\r?\n/).filter(Boolean);
  return needle ? lines.filter((line) => line.toLowerCase().includes(needle)) : lines;
}

function renderLogLines() {
  const lines = filteredLogLines();
  if (!lines.length)
    return `<div class="terminal-empty">${escapeHtml(t("logs.empty"))}</div>`;
  return lines.map((line) => {
    const lower = line.toLowerCase();
    const tone = /error|fail|denied|crash/.test(lower) ? "error" : /warn|safe.?mode/.test(lower) ? "warning" : /inject|initialized|claimed|starting|status ok/.test(lower) ? "success" : "";
    return `<div class="log-line ${tone}">${escapeHtml(line)}</div>`;
  }).join("");
}

function renderLogs() {
  return `<div class="page-grid"><section class="panel span-2 log-panel">
    ${sectionHeader(t("logs.title"), t("logs.desc"))}
    <div class="log-toolbar">
      <div class="segmented"><button class="${state.logType === "system" ? "active" : ""}" data-log-type="system">${escapeHtml(t("logs.system"))}</button><button class="${state.logType === "kernel" ? "active" : ""}" data-log-type="kernel">${escapeHtml(t("logs.kernel"))}</button></div>
      <div class="toolbar-actions"><button class="icon-button" data-action="copy-logs" aria-label="${escapeHtml(t("common.copy"))}">${icon("copy")}</button>${state.logType === "system" ? `<button class="icon-button danger" data-action="clear-logs" aria-label="${escapeHtml(t("common.clear"))}">${icon("trash")}</button>` : ""}</div>
    </div>
    <div class="search-box">${icon("search")}<input id="log-search" type="search" value="${escapeHtml(state.logSearch)}" placeholder="${escapeHtml(t("common.search"))}"></div>
    <div class="terminal" id="terminal">${renderLogLines()}</div>
  </section></div>`;
}

function renderAbout() {
  return `<div class="page-grid">
    <section class="about-hero span-2"><img src="./icon.svg" alt=""><div><h1>YukiZygisk</h1><p>${escapeHtml(t("about.description"))}</p><div class="chip-cloud"><span class="chip">${escapeHtml(state.meta.version)}</span><span class="chip">${escapeHtml(state.status.abi || "arm64-v8a")}</span><span class="chip">Apache-2.0 / GPL-2.0</span></div></div></section>
    <section class="panel">${sectionHeader(t("about.architecture"))}<p class="body-copy">${escapeHtml(t("about.architectureBody"))}</p><dl class="info-grid single"><div><dt>${escapeHtml(t("about.version"))}</dt><dd>${escapeHtml(state.meta.version)}</dd></div><div><dt>${escapeHtml(t("about.author"))}</dt><dd>${escapeHtml(state.meta.author)}</dd></div></dl></section>
    <section class="panel">${sectionHeader(t("about.credits"))}<p class="body-copy">${escapeHtml(t("about.creditsBody"))}</p><a class="link-card" href="https://github.com/KOWX712/ksu-webui-demo">${escapeHtml(t("about.source"))}${icon("external")}</a></section>
  </div>`;
}

function renderPage() {
  switch (state.tab) {
    case "modules": return renderModules();
    case "denylist": return renderDenylist();
    case "settings": return renderSettings();
    case "logs": return renderLogs();
    case "about": return renderAbout();
    default: return renderOverview();
  }
}

function renderApp() {
  const root = document.getElementById("app");
  if (!root)
    return;
  if (state.loading) {
    root.innerHTML = `<div class="boot-panel"><img src="./icon.svg" alt="" width="52" height="52"><div><div class="boot-mark">YUKIZYGISK</div><p>${escapeHtml(t("common.loading"))}</p></div></div>`;
    return;
  }
  const activeTab = TABS.find((tab) => tab.id === state.tab) || TABS[0];
  root.className = "app-shell";
  root.innerHTML = `
    <header class="topbar">
      <div class="brand"><img src="./icon.svg" alt=""><div><strong>YukiZygisk</strong><span>${escapeHtml(t("app.subtitle"))}</span></div></div>
      <div class="topbar-actions">
        <span class="runtime-pill ${state.runtimeMode}">${escapeHtml(state.runtimeMode === "mock" ? t("common.mock") : t("common.live"))}</span>
        <select id="language-select" class="language-select" aria-label="Language">${LANGUAGE_OPTIONS.map((option) => `<option value="${option.value}" ${option.value === state.language ? "selected" : ""}>${escapeHtml(option.shortLabel)}</option>`).join("")}</select>
        <button class="icon-button" data-action="theme" aria-label="Theme">${icon("theme")}</button>
        <button class="icon-button ${state.refreshing ? "spinning" : ""}" data-action="refresh" aria-label="${escapeHtml(t("common.refresh"))}">${icon("refresh")}</button>
      </div>
    </header>
    <div class="workspace">
      <aside class="side-nav">${TABS.map((tab) => `<button class="nav-item ${tab.id === state.tab ? "active" : ""}" data-tab="${tab.id}">${icon(tab.icon)}<span>${escapeHtml(t(tab.label))}</span></button>`).join("")}</aside>
      <main class="page"><div class="page-titlebar"><div><span>${escapeHtml(t(activeTab.label))}</span><small>${escapeHtml(state.meta.version)}</small></div></div>${renderPage()}</main>
    </div>
    <nav class="bottom-nav">${TABS.map((tab) => `<button class="nav-item ${tab.id === state.tab ? "active" : ""}" data-tab="${tab.id}">${icon(tab.icon)}<span>${escapeHtml(t(tab.label))}</span></button>`).join("")}</nav>`;
}

function syncTabUrl() {
  const url = new URL(globalThis.location.href);
  url.searchParams.set("tab", state.tab);
  history.replaceState(null, "", url);
}

async function refreshAll({ includeConfig = true } = {}) {
  state.refreshing = true;
  renderApp();
  try {
    const requests = [api.getStatus(), api.getSystemInfo(), api.getModuleMeta()];
    if (includeConfig)
      requests.push(api.loadConfig());
    const [status, system, meta, config] = await Promise.all(requests);
    state.status = status;
    state.system = system;
    state.meta = meta;
    if (config)
      state.config = config;
    state.lastUpdated = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    state.error = "";
    if (state.tab === "logs")
      state.logs = await api.readLogs(state.logType);
  } catch (error) {
    state.error = error.message;
    showToast(error.message, "danger");
  } finally {
    state.refreshing = false;
    renderApp();
  }
}

function collectConfigControls() {
  const yukilinker = document.getElementById("setting-yukilinker");
  const dmesg = document.getElementById("setting-dmesg");
  const mode = document.getElementById("denylist-mode");
  if (yukilinker)
    state.config.yukilinker = yukilinker.checked;
  if (dmesg)
    state.config.dmesg_log = dmesg.checked;
  if (mode)
    state.config.denylist_mode = Number(mode.value);
}

async function saveConfig() {
  collectConfigControls();
  try {
    state.config = await api.saveConfig(state.config);
    state.status = await api.getStatus();
    showToast(t("settings.saved"));
    renderApp();
  } catch (error) {
    showToast(`${t("settings.saveFailed")}: ${error.message}`, "danger");
  }
}

async function reloadRuntime() {
  try {
    await api.reload();
    await refreshAll();
    showToast(t("settings.reloaded"));
  } catch (error) {
    showToast(`${t("settings.reloadFailed")}: ${error.message}`, "danger");
  }
}

async function loadPackages() {
  try {
    state.packages = await api.listPackages();
    state.packagesLoaded = true;
    renderApp();
  } catch (error) {
    showToast(error.message, "danger");
  }
}

async function loadLogs() {
  try {
    state.logs = await api.readLogs(state.logType);
    renderApp();
  } catch (error) {
    showToast(error.message, "danger");
  }
}

async function copyLogs() {
  try {
    await navigator.clipboard.writeText(filteredLogLines().join("\n"));
    showToast(t("common.copied"));
  } catch (error) {
    showToast(error.message, "danger");
  }
}

document.addEventListener("click", async (event) => {
  const tab = event.target.closest("[data-tab]");
  if (tab) {
    state.tab = tab.dataset.tab;
    syncTabUrl();
    if (state.tab === "logs" && !state.logs)
      await loadLogs();
    else
      renderApp();
    return;
  }

  const action = event.target.closest("[data-action]")?.dataset.action;
  if (!action)
    return;
  if (action === "refresh") await refreshAll();
  if (action === "reload") await reloadRuntime();
  if (action === "save-config") await saveConfig();
  if (action === "load-packages") await loadPackages();
  if (action === "copy-logs") await copyLogs();
  if (action === "clear-logs" && confirm(t("logs.clearConfirm"))) {
    try {
      await api.clearLogs();
      state.logs = "";
      showToast(t("logs.cleared"));
      renderApp();
    } catch (error) {
      showToast(error.message, "danger");
    }
  }
  if (action === "theme") cycleTheme();
});

document.addEventListener("change", async (event) => {
  if (event.target.id === "language-select") {
    state.language = event.target.value;
    localStorage.setItem("yukizygisk_language", state.language);
    document.documentElement.lang = state.language;
    renderApp();
    return;
  }
  if (event.target.id === "denylist-mode") {
    state.config.denylist_mode = Number(event.target.value);
    renderApp();
    return;
  }
  if (event.target.matches("[data-appid]")) {
    const appid = Number(event.target.dataset.appid);
    const selected = new Set(state.config.denylist_app_ids.map(Number));
    if (event.target.checked) selected.add(appid);
    else selected.delete(appid);
    state.config.denylist_app_ids = [...selected].sort((a, b) => a - b);
    const counter = document.getElementById("selection-counter");
    if (counter)
      counter.textContent = t("denylist.selected", "", { count: selected.size });
    for (const checkbox of document.querySelectorAll(`[data-appid="${appid}"]`))
      checkbox.checked = event.target.checked;
  }
});

document.addEventListener("input", (event) => {
  if (event.target.id === "package-search") {
    state.packageSearch = event.target.value;
    const list = document.getElementById("package-list");
    if (list)
      list.innerHTML = renderPackageList();
  }
  if (event.target.id === "log-search") {
    state.logSearch = event.target.value;
    const terminal = document.getElementById("terminal");
    if (terminal)
      terminal.innerHTML = renderLogLines();
  }
});

document.addEventListener("click", async (event) => {
  const type = event.target.closest("[data-log-type]")?.dataset.logType;
  if (!type || type === state.logType)
    return;
  state.logType = type;
  state.logSearch = "";
  state.logs = "";
  await loadLogs();
});

async function bootstrap() {
  applyTheme();
  document.documentElement.lang = state.language;
  await enableEdgeToEdge(true).catch(() => false);
  try {
    const [status, config, system, meta] = await Promise.all([
      api.getStatus(),
      api.loadConfig(),
      api.getSystemInfo(),
      api.getModuleMeta(),
    ]);
    state.status = status;
    state.config = config;
    state.system = system;
    state.meta = meta;
    state.lastUpdated = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  } catch (error) {
    state.error = error.message;
    state.status.error = error.message;
  } finally {
    state.loading = false;
    renderApp();
  }
}

bootstrap();
