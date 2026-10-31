(function () {
  if (window.top !== window) return;
  if (
    location.hostname !== "www.youtube.com" &&
    location.hostname !== "m.youtube.com"
  )
    return;

  const API = "https://sponsor.ajay.app/api/skipSegments/";
  const CATS = [
    "sponsor",
    "selfpromo",
    "interaction",
    "intro",
    "outro",
    "preview",
  ];
  const COLORS = {
    sponsor: "#00d400",
    selfpromo: "#ffff00",
    interaction: "#cc00ff",
    intro: "#00ffff",
    outro: "#0202ed",
    preview: "#008fd6",
  };

  let videoId = null;
  let segments = [];

  function currentId() {
    const p = new URLSearchParams(location.search).get("v");
    if (p) return p;
    const m = location.pathname.match(/^\/shorts\/([\w-]{11})/);
    return m ? m[1] : null;
  }

  async function sha256Hex(s) {
    const buf = await crypto.subtle.digest(
      "SHA-256",
      new TextEncoder().encode(s),
    );
    return Array.from(new Uint8Array(buf), (b) =>
      b.toString(16).padStart(2, "0"),
    ).join("");
  }

  async function load(id) {
    try {
      const prefix = (await sha256Hex(id)).slice(0, 4);
      const r = await fetch(
        API +
          prefix +
          "?categories=" +
          encodeURIComponent(JSON.stringify(CATS)),
      );
      if (!r.ok) return; // 404 = nothing submitted for this prefix
      const data = await r.json();
      if (id !== videoId) return; // navigated away meanwhile
      const entry = data.find((e) => e.videoID === id);
      segments = entry
        ? entry.segments
            .filter((s) => s.actionType === "skip")
            .map((s) => ({
              start: s.segment[0],
              end: s.segment[1],
              category: s.category,
            }))
        : [];
    } catch (e) {
      console.warn("sponsorblock:", e);
    }
  }

  function drawMarkers(v, bar) {
    for (const s of segments) {
      const d = document.createElement("div");
      d.className = "fbb-sb";
      d.style.cssText =
        "position:absolute;top:0;height:100%;pointer-events:none;z-index:40;opacity:.8;" +
        "left:" +
        (s.start / v.duration) * 100 +
        "%;" +
        "width:" +
        ((s.end - s.start) / v.duration) * 100 +
        "%;" +
        "background:" +
        (COLORS[s.category] || "#0f0");
      bar.appendChild(d);
    }
  }

  setInterval(() => {
    const id = currentId();
    if (id !== videoId) {
      videoId = id;
      segments = [];
      document.querySelectorAll(".fbb-sb").forEach((n) => n.remove());
      if (id) load(id);
    }
    const v = document.querySelector("video.html5-main-video");
    if (
      !v ||
      !segments.length ||
      document.querySelector(".html5-video-player.ad-showing")
    )
      return;

    const bar = document.querySelector(".ytp-progress-list");
    if (bar && v.duration && !bar.querySelector(".fbb-sb")) drawMarkers(v, bar);

    if (v.paused) return;
    const t = v.currentTime;
    for (const s of segments)
      if (t >= s.start && t < s.end - 0.1) {
        v.currentTime = s.end;
        break;
      }
  }, 100);
})();
