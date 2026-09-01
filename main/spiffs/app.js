async function fetchJson(url, options) {
  const response = await fetch(url, options);
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response.json();
}

let savedWifiSsid = "";

function setValue(id, value) {
  const el = document.getElementById(id);
  if (el) {
    el.value = value ?? "";
  }
}

function setChecked(id, value) {
  const el = document.getElementById(id);
  if (el) {
    el.checked = Boolean(value);
  }
}

function populateClockFonts(fonts, selected) {
  const select = document.getElementById("clock-font");
  const description = document.getElementById("clock-font-description");
  if (!select || !Array.isArray(fonts) || fonts.length === 0) return;

  select.replaceChildren();
  for (const font of fonts) {
    const option = document.createElement("option");
    option.value = String(font.value);
    option.textContent = font.label;
    option.dataset.description = font.description ?? "";
    select.append(option);
  }
  select.value = String(selected);
  if (select.value !== String(selected)) select.selectedIndex = 0;
  const updateDescription = () => {
    const option = select.options[select.selectedIndex];
    if (description) description.textContent = option?.dataset.description ?? "";
  };
  select.onchange = updateDescription;
  updateDescription();
}

function fmtUptime(s) {
  if (s < 60)   return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

function fmtTime(unix) {
  if (!unix) return "—";
  return new Date(unix * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

function statusRow(label, value, cls) {
  const v = cls ? `<span class="status-value ${cls}">${value}</span>` : `<span class="status-value">${value}</span>`;
  return `<div class="status-row"><span class="status-label">${label}</span>${v}</div>`;
}

async function loadStatus() {
  const s = await fetchJson("/api/status");
  const b = s.build ?? {};
  const w = s.weather;
  const failCount = w.fetch_fail ?? 0;
  const okCount   = w.fetch_ok   ?? 0;
  const lastErr   = w.last_error;
  const version   = b.version ?? s.firmware ?? "unknown";
  const gitSha    = b.git_sha ?? "unknown";
  const assetRev  = b.asset_rev ?? "unknown";
  const buildTime = b.build_time_utc ?? "unknown";
  const dirtyNote = b.git_dirty ? " dirty" : "";

  const wifiVal = s.wifi.connected
    ? `Connected &middot; ${s.wifi.ip}`
    : "Disconnected";
  const wifiCls = s.wifi.connected ? "ok" : "error";

  const weatherVal = w.available ? `Updated ${fmtTime(w.last_success_unix)}` : "No data";
  const fetchVal   = failCount > 0
    ? `${okCount} ok &middot; <span class="status-value error">${failCount} failed</span>`
    : `${okCount} ok`;

  let rows = [
    statusRow("Firmware",  `v${version}`),
    statusRow("Build",     `${gitSha}${dirtyNote} &middot; ${assetRev}`),
    statusRow("Built",     buildTime),
    statusRow("Uptime",    fmtUptime(s.uptime_s)),
    statusRow("Mode",      `${s.display.mode} &middot; ${s.display.brightness_pct}% brightness`),
    statusRow("Wi-Fi",     wifiVal, wifiCls),
    statusRow("Weather",   weatherVal, w.available ? "ok" : "warn"),
    statusRow("Fetches",   fetchVal),
  ];
  if (!w.available && lastErr && lastErr !== "ok" && lastErr !== "none") {
    rows.push(statusRow("Last error", lastErr, "error"));
  }

  document.getElementById("status").innerHTML = rows.join("");
}

function setTimezone(tz) {
  const sel = document.getElementById("timezone-select");
  const customLabel = document.getElementById("timezone-custom-label");
  const customInput = document.getElementById("timezone");
  // try to match a known option
  const opt = Array.from(sel.options).find(o => o.value === tz);
  if (opt) {
    sel.value = tz;
    customLabel.hidden = true;
  } else {
    sel.value = "custom";
    customInput.value = tz ?? "";
    customLabel.hidden = false;
  }
}

function getTimezone() {
  const sel = document.getElementById("timezone-select");
  if (sel.value === "custom") {
    return document.getElementById("timezone").value;
  }
  return sel.value;
}

async function loadConfig() {
  const config = await fetchJson("/api/config");
  setTimezone(config.timezone);
  setValue("wifi-ssid", config.wifi?.ssid);
  savedWifiSsid = config.wifi?.ssid ?? "";
  setValue("weather-units", config.weather?.units);
  setValue("weather-lat", config.weather?.lat);
  setValue("weather-lon", config.weather?.lon);
  setValue("weather-city", config.weather?.city ?? "");
  setValue("display-brightness", config.display?.brightness_pct);
  document.getElementById("display-brightness-val").textContent =
    (config.display?.brightness_pct ?? "") + "%";
  populateClockFonts(config.display?.clock_fonts, config.display?.clock_font ?? 0);
  setValue("panel-humidity-metric", config.display?.panel_humidity_metric ?? "humidity");
  setValue("cycle-clock-s",    config.display?.cycle?.clock_s);
  setValue("cycle-today-s",    config.display?.cycle?.today_s);
  setValue("cycle-forecast-s", config.display?.cycle?.forecast_s);

  document.getElementById("wifi-psk").placeholder =
    config.wifi?.has_psk ? "Stored in device; leave blank to keep" : "Set Wi-Fi password";
}

async function saveConfig(event) {
  event.preventDefault();
  const payload = {
    timezone: getTimezone(),
    wifi: {
      ssid: document.getElementById("wifi-ssid").value,
    },
    weather: {
      units: document.getElementById("weather-units").value,
      lat: Number(document.getElementById("weather-lat").value),
      lon: Number(document.getElementById("weather-lon").value),
      city: document.getElementById("weather-city").value,
    },
    display: {
      brightness_pct: Number(document.getElementById("display-brightness").value),
      clock_font: Number(document.getElementById("clock-font").value),
      panel_humidity_metric: document.getElementById("panel-humidity-metric").value,
      cycle: {
        clock_s:    Number(document.getElementById("cycle-clock-s").value),
        today_s:    Number(document.getElementById("cycle-today-s").value),
        forecast_s: Number(document.getElementById("cycle-forecast-s").value),
      },
    },
  };

  const wifiSsid = document.getElementById("wifi-ssid").value;
  const wifiPsk = document.getElementById("wifi-psk").value;
  const wifiWillRestart = wifiPsk !== "" || wifiSsid !== savedWifiSsid;
  if (wifiPsk) {
    payload.wifi.psk = wifiPsk;
  }
  if (!Number.isFinite(payload.weather.lat)) {
    delete payload.weather.lat;
  }
  if (!Number.isFinite(payload.weather.lon)) {
    delete payload.weather.lon;
  }

  let result;
  try {
    result = await fetchJson("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
  } catch (error) {
    // The setup AP deliberately vanishes during a successful Wi-Fi restart.
    // A browser can report that as a network failure before it receives the
    // acknowledgement, so give the user the useful next step instead.
    if (wifiWillRestart) {
      document.getElementById("save-result").textContent =
        "Credentials sent. Nowtube is restarting; reconnect your phone to home Wi-Fi.";
      document.getElementById("wifi-psk").value = "";
      return;
    }
    throw error;
  }

  document.getElementById("save-result").textContent = result.restarting
    ? "Saved. Restarting to join Wi-Fi; reconnect your phone to home Wi-Fi…"
    : "Saved.";
  document.getElementById("wifi-psk").value = "";
  if (result.restarting) return;
  await loadStatus();
  await loadConfig();
}

// ---- City lookup ------------------------------------------------------------

async function lookupCity() {
  const city = document.getElementById("weather-city").value.trim();
  const result = document.getElementById("city-lookup-result");
  if (!city) { result.textContent = "Enter a city name first."; return; }

  result.textContent = "Looking up…";
  try {
    const res = await fetch(
      `https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(city)}&count=5&language=en&format=json`
    );
    const data = await res.json();
    if (!data.results?.length) { result.textContent = "No results found."; return; }
    const r = data.results[0];
    document.getElementById("weather-lat").value = r.latitude.toFixed(6);
    document.getElementById("weather-lon").value = r.longitude.toFixed(6);
    const label = [r.name, r.admin1, r.country].filter(Boolean).join(", ");
    document.getElementById("weather-city").value = label;
    result.textContent = `Set to ${label} (${r.latitude.toFixed(4)}, ${r.longitude.toFixed(4)})`;
  } catch (e) {
    result.textContent = `Lookup failed: ${e.message}`;
  }
}

// ---- Backlight --------------------------------------------------------------

async function loadBacklight() {
  const bl = await fetchJson("/api/backlight");
  setValue("backlight-mode", bl.mode);
  setValue("backlight-color", bl.led_color);
}

async function saveBacklight(event) {
  event.preventDefault();
  const payload = {
    mode: document.getElementById("backlight-mode").value,
    led_color: document.getElementById("backlight-color").value,
  };
  await fetchJson("/api/backlight", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  document.getElementById("backlight-result").textContent = "Applied.";
}

// ---- Actions ----------------------------------------------------------------

async function refreshWeather() {
  const result = document.getElementById("action-result");
  result.textContent = "Triggering fetch…";
  try {
    await fetchJson("/api/weather/refresh", { method: "POST" });
    result.textContent = "Weather fetch triggered.";
    setTimeout(() => loadStatus(), 3000);
  } catch (e) {
    result.textContent = `Error: ${e.message}`;
  }
}

async function rebootDevice() {
  if (!confirm("Reboot the device now?")) return;
  const result = document.getElementById("action-result");
  result.textContent = "Rebooting…";
  try {
    await fetchJson("/api/reboot", { method: "POST" });
  } catch {
    // fetch may fail if device closes connection — that's expected
  }
  result.textContent = "Device is rebooting. Reconnect in a few seconds.";
}

// ---- OTA --------------------------------------------------------------------

function resetOtaUi() {
  document.getElementById("ota-progress-wrap").hidden = true;
  document.getElementById("ota-progress").value = 0;
  document.getElementById("ota-state-text").textContent = "";
  document.getElementById("ota-file-btn").disabled = false;
  document.getElementById("ota-file").value = "";
}

async function uploadFirmware() {
  const file = document.getElementById("ota-file").files[0];
  if (!file) {
    document.getElementById("ota-result").textContent = "Select a .bin file first.";
    return;
  }

  document.getElementById("ota-file-btn").disabled = true;
  document.getElementById("ota-result").textContent = "";
  document.getElementById("ota-progress-wrap").hidden = false;
  document.getElementById("ota-progress").value = 0;
  document.getElementById("ota-state-text").textContent = "Uploading…";

  await new Promise((resolve) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota/upload");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");

    xhr.upload.addEventListener("progress", (e) => {
      if (e.lengthComputable) {
        const pct = Math.round((e.loaded / e.total) * 95);
        document.getElementById("ota-progress").value = pct;
        document.getElementById("ota-state-text").textContent = `Uploading… ${pct}%`;
      }
    });

    xhr.addEventListener("load", () => {
      if (xhr.status === 200) {
        document.getElementById("ota-progress").value = 100;
        document.getElementById("ota-state-text").textContent =
          "Update complete — device is rebooting";
        setTimeout(resetOtaUi, 5000);
      } else {
        document.getElementById("ota-result").textContent =
          `Error ${xhr.status}: ${xhr.responseText}`;
        document.getElementById("ota-file-btn").disabled = false;
      }
      resolve();
    });

    xhr.addEventListener("error", () => {
      // Device may have rebooted and closed the connection — treat as success.
      document.getElementById("ota-progress").value = 100;
      document.getElementById("ota-state-text").textContent =
        "Device rebooted. Reconnect to verify the new firmware.";
      setTimeout(resetOtaUi, 5000);
      resolve();
    });

    xhr.send(file);
  });
}

// ---- Boot -------------------------------------------------------------------

async function boot() {
  try {
    await Promise.all([loadStatus(), loadConfig(), loadBacklight()]);
  } catch (error) {
    document.getElementById("status").innerHTML =
      `<div class="status-row"><span class="status-label">Error</span><span class="status-value error">${error.message}</span></div>`;
  }
  setInterval(() => loadStatus().catch(() => {}), 30000);

  document.getElementById("config-form").addEventListener("submit", async (event) => {
    document.getElementById("save-result").textContent = "Saving…";
    try {
      await saveConfig(event);
    } catch (error) {
      document.getElementById("save-result").textContent = `Save failed: ${error.message}`;
    }
  });

  document.getElementById("backlight-form").addEventListener("submit", async (event) => {
    document.getElementById("backlight-result").textContent = "Applying…";
    try {
      await saveBacklight(event);
    } catch (error) {
      document.getElementById("backlight-result").textContent = `Failed: ${error.message}`;
    }
  });

  document.getElementById("display-brightness").addEventListener("input", (e) => {
    document.getElementById("display-brightness-val").textContent = e.target.value + "%";
  });
  document.getElementById("timezone-select").addEventListener("change", () => {
    const isCustom = document.getElementById("timezone-select").value === "custom";
    document.getElementById("timezone-custom-label").hidden = !isCustom;
  });
  document.getElementById("city-lookup-btn").addEventListener("click", lookupCity);
  ["weather-lat", "weather-lon"].forEach(id => {
    document.getElementById(id).addEventListener("input", () => {
      document.getElementById("weather-city").value = "";
      document.getElementById("city-lookup-result").textContent = "";
    });
  });
  document.getElementById("weather-city").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); lookupCity(); }
  });
  document.getElementById("weather-refresh-btn").addEventListener("click", refreshWeather);
  document.getElementById("reboot-btn").addEventListener("click", rebootDevice);
  document.getElementById("ota-file-btn").addEventListener("click", uploadFirmware);
}

boot();
