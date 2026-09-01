const fontNotes = {
  nixie: "Nixie One — warm, characterful tube face. The signature Nowtube look.",
  space: "Space Mono — clean technical mono face. Compact and deliberately quiet.",
  atkinson: "Atkinson Hyperlegible — maximum at-a-glance readability.",
  aldrich: "Aldrich — geometric display face with a calm sci-fi edge.",
};

const font = document.getElementById("font");
const time = document.getElementById("time");
const brightness = document.getElementById("brightness");
const weather = document.getElementById("show-weather");
const row = document.getElementById("tube-row");

function render() {
  const [hours, minutes] = time.value.split(":").map(Number);
  const h12 = (hours % 12 || 12).toString().padStart(2, "0");
  document.getElementById("digit-0").textContent = h12[0];
  document.getElementById("digit-1").textContent = h12[1];
  document.getElementById("digit-2").textContent = minutes.toString().padStart(2, "0")[0];
  document.getElementById("digit-3").textContent = minutes.toString().padStart(2, "0")[1];
  document.getElementById("ampm").textContent = hours < 12 ? "AM" : "PM";
  document.getElementById("weather-icon").hidden = !weather.checked || h12[0] !== "0";
  document.getElementById("digit-0").classList.toggle("icon-visible", weather.checked && h12[0] === "0");
  row.className = `tube-row font-${font.value}`;
  row.style.setProperty("--glow", brightness.value / 100);
  document.getElementById("brightness-value").value = `${brightness.value}%`;
  document.getElementById("font-note").textContent = fontNotes[font.value];
}

[font, time, brightness, weather].forEach((control) => control.addEventListener("input", render));
render();
