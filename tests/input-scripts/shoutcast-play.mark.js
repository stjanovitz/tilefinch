// The focused element, its station row, and the player at each mark.
const active = document.activeElement;
const row = active && active.closest ? active.closest("#station-list tr") : null;
const cell = (name) => row ? ((row.querySelector("td." + name) || {}).textContent || null) : null;
const audio = document.querySelector("#jplayer audio, audio");
const station = typeof stream === "object" && stream ? stream : null;
const strip = (url) => String(url || "").replace(/\?.*$/, "");
return JSON.stringify({
  mark: __tfMark,
  active: active ? active.tagName + (active.closest && active.closest("td.play") ? ".play" : "") : null,
  name: cell("radio-name"), format: cell("encode"), bitrate: cell("bitrate"),
  streamName: station ? station.Name : null,
  streamUrl: station ? strip(station.mp3) : null,
  audio: audio ? {src: strip(audio.currentSrc || audio.src), paused: audio.paused,
                  readyState: audio.readyState,
                  currentTime: Math.round(audio.currentTime * 10) / 10,
                  error: audio.error ? audio.error.code : 0} : null
});
