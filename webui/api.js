/* SPDX-License-Identifier: MIT */
/*
 * YukiZygisk - WebUI control and telemetry adapter.
 * Derived from KOWX712/ksu-webui-demo and Kagami's static WebUI.
 * License: MIT
 * Authors: KOWX712 and Anatdx
 */

import { exec, hasKernelSU } from "./assets/kernelsu.js";

export const PATHS = {
  MODULE: "/data/adb/modules/yukizygisk",
  BINARY: "/data/adb/modules/yukizygisk/zygiskd",
  CONFIG: "/data/adb/yukizygisk/yzconfig.json",
  LOG: "/data/adb/yukizygisk/zygiskd.log",
};

export const DEFAULT_CONFIG = {
  yukilinker: true,
  denylist_mode: 0,
  dmesg_log: false,
  denylist_app_ids: [],
};

export const DEFAULT_STATUS = {
  available: false,
  kernel_alive: false,
  daemon_pid: 0,
  abi: "arm64-v8a",
  count: 0,
  safe_mode: false,
  zygote_crashes: 0,
  safe_mode_zygote: "zygote",
  yukilinker: true,
  denylist_mode: 0,
  dmesg_log: false,
  denylist_app_ids: [],
  recent: [],
  zygotes: [],
  zygote_monitor: [],
  modules: [],
  native_modules: [],
  native_injections: [],
  error: "",
};

const params = new URLSearchParams(globalThis.location?.search || "");
const runtimeMode = params.get("mock") === "1" || !hasKernelSU() ? "mock" : "live";

export function getRuntimeMode() {
  return runtimeMode;
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function output(result) {
  return String(result?.stdout || result?.stderr || "").trim();
}

function shellEscape(value) {
  return String(value ?? "").replace(/'/g, "'\\''");
}

function normalizeConfig(value = {}) {
  const mode = Number(value.denylist_mode);
  const ids = Array.isArray(value.denylist_app_ids) ? value.denylist_app_ids : [];
  return {
    yukilinker: value.yukilinker !== false,
    denylist_mode: [0, 1, 2].includes(mode) ? mode : 0,
    dmesg_log: value.dmesg_log === true,
    denylist_app_ids: [...new Set(ids.map(Number).filter((id) => Number.isInteger(id) && id >= 0 && id < 100000))].sort(
      (a, b) => a - b
    ),
  };
}

function normalizeStatus(value = {}) {
  const status = { ...clone(DEFAULT_STATUS), ...value, available: value.kernel_alive === true };
  for (const key of ["recent", "zygotes", "zygote_monitor", "modules", "native_modules", "native_injections", "denylist_app_ids"]) {
    if (!Array.isArray(status[key]))
      status[key] = [];
  }
  return status;
}

function parseModuleProp(text) {
  const meta = { id: "yukizygisk", name: "YukiZygisk", version: "dev", versionCode: "", author: "Anatdx", description: "" };
  for (const line of String(text || "").split(/\r?\n/)) {
    const index = line.indexOf("=");
    if (index <= 0)
      continue;
    const key = line.slice(0, index).trim();
    const value = line.slice(index + 1).trim();
    if (key in meta)
      meta[key] = value;
  }
  return meta;
}

async function writeConfig(config) {
  const normalized = normalizeConfig(config);
  const payload = shellEscape(JSON.stringify(normalized, null, 2));
  const path = shellEscape(PATHS.CONFIG);
  const command = `mkdir -p /data/adb/yukizygisk && printf '%s\n' '${payload}' > '${path}' && chmod 0600 '${path}'`;
  const result = await exec(command);
  if (result.errno !== 0)
    throw new Error(output(result) || "failed to write yzconfig.json");
  return normalized;
}

const mockState = {
  config: {
    yukilinker: true,
    denylist_mode: 1,
    dmesg_log: true,
    denylist_app_ids: [10123, 10244],
  },
  status: normalizeStatus({
    kernel_alive: true,
    daemon_pid: 842,
    abi: "arm64-v8a",
    count: 184,
    yukilinker: true,
    denylist_mode: 1,
    dmesg_log: true,
    denylist_app_ids: [10123, 10244],
    recent: [10123, 10244, 10188, 10072, 10311],
    zygotes: [{ pid: 1771, name: "zygote64", abi: "arm64-v8a" }],
    zygote_monitor: [
      { pid: 1771, name: "zygote64", abi: "arm64-v8a", state: "injected" },
      { pid: 1772, name: "zygote_secondary", abi: "armeabi-v7a", state: "unsupported32" },
    ],
    modules: ["zygisk_lsposed", "playintegrityfix"],
    native_modules: [
      { id: "zn_audit", target_type: "name", target: "logd", companion: false, state: "injected" },
      { id: "native_guard", target_type: "path", target: "/system/bin/keystore2", companion: true, state: "failed" },
    ],
    native_injections: [
      {
        pid: 611,
        process: "logd",
        module: "zn_audit",
        target_type: "name",
        target: "logd",
        abi: "arm64-v8a",
        companion: false,
        state: "injected",
      },
    ],
  }),
  packages: [
    { packageName: "com.android.systemui", uid: 10123, appId: 10123 },
    { packageName: "com.google.android.gms", uid: 10244, appId: 10244 },
    { packageName: "com.android.vending", uid: 10188, appId: 10188 },
    { packageName: "com.example.wallet", uid: 10311, appId: 10311 },
    { packageName: "com.example.game", uid: 10420, appId: 10420 },
  ],
  logs: {
    system: "post-fs-data: loading yukizygisk.ko cookie=<redacted>\npost-fs-data: starting zygiskd\nboot-completed: zygiskd status ok",
    kernel:
      "[    4.921] yukizygisk: standalone LKM initialized\n[    5.083] zygiskd: claimed anonymous control fd\n[   12.447] zygiskd: zygote injected: pid=1771 name=zygote64 abi=arm64-v8a",
  },
};

const mockApi = {
  async getStatus() {
    return clone(mockState.status);
  },
  async loadConfig() {
    return clone(mockState.config);
  },
  async saveConfig(config) {
    mockState.config = normalizeConfig(config);
    Object.assign(mockState.status, mockState.config);
    return clone(mockState.config);
  },
  async reload() {
    return true;
  },
  async listPackages() {
    return clone(mockState.packages);
  },
  async readLogs(kind) {
    return mockState.logs[kind] || "";
  },
  async clearLogs() {
    mockState.logs.system = "";
  },
  async getSystemInfo() {
    return { kernel: "6.12.23-android16-gki", selinux: "Enforcing" };
  },
  async getModuleMeta() {
    return { id: "yukizygisk", name: "YukiZygisk", version: "v0.1.0-10008", versionCode: "10008", author: "Anatdx" };
  },
};

const realApi = {
  async getStatus() {
    const result = await exec(`'${shellEscape(PATHS.BINARY)}' --status`);
    const text = output(result);
    if (result.errno !== 0 || !text) {
      return normalizeStatus({ error: text || "zygiskd status unavailable" });
    }
    try {
      return normalizeStatus(JSON.parse(text));
    } catch (error) {
      return normalizeStatus({ error: `invalid status JSON: ${error.message}` });
    }
  },

  async loadConfig() {
    const result = await exec(`cat '${shellEscape(PATHS.CONFIG)}' 2>/dev/null`);
    if (result.errno !== 0 || !result.stdout.trim())
      return clone(DEFAULT_CONFIG);
    try {
      return normalizeConfig(JSON.parse(result.stdout));
    } catch (_error) {
      return clone(DEFAULT_CONFIG);
    }
  },

  async saveConfig(config) {
    const normalized = await writeConfig(config);
    const result = await exec(`'${shellEscape(PATHS.BINARY)}' --reload`);
    if (result.errno !== 0)
      throw new Error(output(result) || "zygiskd reload failed");
    return normalized;
  },

  async reload() {
    const result = await exec(`'${shellEscape(PATHS.BINARY)}' --reload`);
    if (result.errno !== 0)
      throw new Error(output(result) || "zygiskd reload failed");
    return true;
  },

  async listPackages() {
    const result = await exec("pm list packages -U 2>/dev/null");
    if (result.errno !== 0)
      throw new Error(output(result) || "pm list packages failed");
    const packages = [];
    const seen = new Set();
    for (const line of result.stdout.split(/\r?\n/)) {
      const name = line.match(/package:([^\s]+)/)?.[1];
      const uid = Number(line.match(/\buid:(\d+)/)?.[1]);
      if (!name || !Number.isInteger(uid) || seen.has(name))
        continue;
      seen.add(name);
      packages.push({ packageName: name, uid, appId: uid % 100000 });
    }
    return packages.sort((a, b) => a.packageName.localeCompare(b.packageName));
  },

  async readLogs(kind) {
    const command =
      kind === "kernel"
        ? "dmesg 2>/dev/null | grep -Ei 'yukizygisk|zygiskd|zygote_probe|zygote_orch|yukilinker|yukizncore' | tail -n 500"
        : `tail -n 500 '${shellEscape(PATHS.LOG)}' 2>/dev/null`;
    const result = await exec(command);
    return output(result);
  },

  async clearLogs() {
    const result = await exec(`: > '${shellEscape(PATHS.LOG)}'`);
    if (result.errno !== 0)
      throw new Error(output(result) || "failed to clear log");
  },

  async getSystemInfo() {
    const [kernel, selinux] = await Promise.all([exec("uname -r"), exec("getenforce 2>/dev/null || echo Unknown")]);
    return { kernel: output(kernel) || "Unknown", selinux: output(selinux) || "Unknown" };
  },

  async getModuleMeta() {
    const result = await exec(`cat '${shellEscape(PATHS.MODULE)}/module.prop' 2>/dev/null`);
    return parseModuleProp(result.stdout);
  },
};

export const api = runtimeMode === "mock" ? mockApi : realApi;
