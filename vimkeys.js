// vimkeys.js - minimal Vimium-style keyboard navigation for fbbrowser.
//
// Injected by MainWindow::installVimKeys() into the ApplicationWorld (an
// isolated JS world: page scripts can't see or tamper with it) at
// DocumentCreation, so its key listener is registered before any page script
// and can swallow keys first.
//
// Anything that needs the browser itself (tabs, URL bar, clipboard) is sent to
// C++ as console.log(TOKEN + command). MainWindow's page class intercepts
// those messages. TOKEN is random per run and only exists in this world, so
// web pages can't forge commands.
(() => {
  "use strict";
  if (window.top !== window) return;

  const TOKEN = "__FBB_TOKEN__";
  const DISABLED_HOSTS = []; // e.g. ["docs.google.com", "www.youtube.com"]
  const HINT_CHARS = "sadfjklewcmpgh";
  const SCROLL_STEP = 70;
  if (DISABLED_HOSTS.includes(location.hostname)) return;

  const send = (cmd) => console.log(TOKEN + cmd);

  let mode = "normal"; // normal | insert | hint | find
  let buf = ""; // pending multi-key sequence ("g", "y")
  let findQuery = "";
  const swallowed = new Set(); // e.code of keydowns we consumed (for keyup)

  // ---------- helpers -------------------------------------------------------
  function deepActive() {
    let a = document.activeElement;
    while (a && a.shadowRoot && a.shadowRoot.activeElement)
      a = a.shadowRoot.activeElement;
    return a;
  }

  const TEXT_TYPES =
    /^(text|search|url|tel|email|password|number|date|datetime-local|month|time|week|)$/i;
  function isTextInput(el) {
    if (!el) return false;
    if (el.isContentEditable) return true;
    if (el.tagName === "TEXTAREA") return true;
    if (el.tagName === "INPUT")
      return TEXT_TYPES.test(el.getAttribute("type") || "");
    return el.tagName === "SELECT";
  }

  function canScroll(el) {
    const s = getComputedStyle(el);
    return (
      (/(auto|scroll)/.test(s.overflowY) &&
        el.scrollHeight > el.clientHeight + 1) ||
      (/(auto|scroll)/.test(s.overflowX) && el.scrollWidth > el.clientWidth + 1)
    );
  }
  function pickScroller() {
    const root = document.scrollingElement || document.documentElement;
    let el = document.elementFromPoint(innerWidth / 2, innerHeight / 2);
    while (el && el !== document.body && el !== document.documentElement) {
      if (canScroll(el)) return el;
      el = el.parentElement;
    }
    return root;
  }
  function scrollBy(dx, dy) {
    const s = pickScroller();
    const root = document.scrollingElement || document.documentElement;
    if (s === root) window.scrollBy(dx, dy);
    else s.scrollBy(dx, dy);
  }
  function scrollTo(top) {
    const s = pickScroller();
    const t = top ? 0 : s.scrollHeight;
    const root = document.scrollingElement || document.documentElement;
    if (s === root) window.scrollTo(window.scrollX, t);
    else s.scrollTo(s.scrollLeft, t);
  }
  const vh = () => pickScroller().clientHeight || innerHeight;

  // ---------- overlay UI (closed shadow root, immune to page CSS) ----------
  function makeOverlay(css) {
    const host = document.createElement("div");
    host.style.cssText =
      "all:initial;position:fixed;inset:0;z-index:2147483647;pointer-events:none;";
    const root = host.attachShadow({ mode: "closed" });
    const style = document.createElement("style");
    style.textContent = css;
    root.appendChild(style);
    (document.documentElement || document).appendChild(host);
    return { host, root };
  }

  // ---------- hints ---------------------------------------------------------
  let hints = null; // { overlay, items:[{el,label,node}], typed, newTab }

  const CLICKABLE =
    "a[href],button,input:not([type=hidden]),select,textarea,summary,[onclick]," +
    "[role=button],[role=link],[role=tab],[role=menuitem],[role=checkbox]," +
    '[role=option],[tabindex]:not([tabindex="-1"]),[contenteditable=""],' +
    "[contenteditable=true]";

  function collectClickable() {
    const out = [];
    for (const el of document.querySelectorAll(CLICKABLE)) {
      if (el.disabled) continue;
      const r = el.getBoundingClientRect();
      if (
        r.width < 2 ||
        r.height < 2 ||
        r.bottom < 0 ||
        r.right < 0 ||
        r.top > innerHeight ||
        r.left > innerWidth
      )
        continue;
      const st = getComputedStyle(el);
      if (
        st.visibility === "hidden" ||
        st.display === "none" ||
        +st.opacity === 0
      )
        continue;
      const cx = Math.min(Math.max(r.left + r.width / 2, 0), innerWidth - 1);
      const cy = Math.min(Math.max(r.top + r.height / 2, 0), innerHeight - 1);
      const top = document.elementFromPoint(cx, cy);
      if (top && !(el.contains(top) || top.contains(el))) continue; // covered
      out.push({ el, r });
    }
    return out;
  }

  function makeLabels(n) {
    const base = HINT_CHARS.length;
    const len = Math.max(
      1,
      Math.ceil(Math.log(Math.max(n, 2)) / Math.log(base)),
    );
    const labels = [];
    for (let i = 0; i < n; i++) {
      let s = "",
        x = i;
      for (let k = 0; k < len; k++) {
        s = HINT_CHARS[x % base] + s;
        x = Math.floor(x / base);
      }
      labels.push(s);
    }
    return labels;
  }

  function startHints(newTab) {
    const found = collectClickable();
    if (!found.length) return;
    const overlay = makeOverlay(
      ".h{position:absolute;font:bold 11px/1 monospace;color:#000;background:#ffd54a;" +
        "border:1px solid #b8860b;border-radius:3px;padding:1px 3px;text-transform:uppercase;" +
        "box-shadow:0 1px 3px rgba(0,0,0,.4)}.t{color:#a06800}",
    );
    const labels = makeLabels(found.length);
    const items = found.map((f, i) => {
      const node = document.createElement("span");
      node.className = "h";
      node.style.left = Math.max(0, f.r.left) + "px";
      node.style.top = Math.max(0, f.r.top) + "px";
      node.textContent = labels[i];
      overlay.root.appendChild(node);
      return { el: f.el, label: labels[i], node };
    });
    hints = { overlay, items, typed: "", newTab };
    mode = "hint";
  }

  function exitHints() {
    if (hints) hints.overlay.host.remove();
    hints = null;
    mode = "normal";
  }

  function activate(el, newTab) {
    exitHints();
    const a = el.closest && el.closest("a[href]");
    if (a && (newTab || a.target === "_blank")) {
      send("open " + a.href);
      return;
    }
    if (isTextInput(el)) {
      el.focus();
      return;
    }
    el.focus && el.focus({ preventScroll: true });
    el.click();
  }

  function hintKey(e) {
    e.preventDefault();
    e.stopImmediatePropagation();
    swallowed.add(e.code);
    if (e.key === "Escape") return exitHints();
    if (e.key === "Backspace") hints.typed = hints.typed.slice(0, -1);
    else if (e.key.length === 1 && HINT_CHARS.includes(e.key.toLowerCase()))
      hints.typed += e.key.toLowerCase();
    else return;
    const t = hints.typed;
    let last = null,
      count = 0;
    for (const it of hints.items) {
      const ok = it.label.startsWith(t);
      it.node.style.display = ok ? "" : "none";
      if (ok) {
        count++;
        last = it;
        it.node.innerHTML = "";
        const typed = document.createElement("span");
        typed.className = "t";
        typed.textContent = t;
        it.node.append(typed, it.label.slice(t.length));
      }
    }
    if (count === 1 && last.label === t) activate(last.el, hints.newTab);
    else if (count === 0) exitHints();
  }

  // ---------- find ----------------------------------------------------------
  let findUi = null;
  function openFind() {
    const overlay = makeOverlay(
      "input{pointer-events:auto;position:fixed;left:8px;bottom:8px;width:320px;" +
        "font:14px monospace;padding:4px 6px;background:#fff;color:#000;border:2px solid #4a90e2;" +
        "border-radius:4px;outline:none}",
    );
    const input = document.createElement("input");
    input.placeholder = "find";
    overlay.root.appendChild(input);
    findUi = { overlay, input };
    mode = "find";
    input.focus();
  }
  function closeFind() {
    if (findUi) findUi.overlay.host.remove();
    findUi = null;
    mode = "normal";
  }
  function doFind(backward) {
    if (findQuery) window.find(findQuery, false, backward, true);
  }

  // ---------- help ----------------------------------------------------------
  let helpUi = null;
  function toggleHelp() {
    if (helpUi) {
      helpUi.host.remove();
      helpUi = null;
      return;
    }
    helpUi = makeOverlay(
      "pre{position:fixed;right:12px;bottom:12px;margin:0;padding:10px 14px;font:12px/1.5 monospace;" +
        "background:#1e1e1e;color:#ddd;border:1px solid #666;border-radius:6px}",
    );
    const pre = document.createElement("pre");
    pre.textContent =
      "j/k h/l  scroll        d/u  half page     gg/G  top/bottom\n" +
      "f / F    hint click / hint in new tab     gi    focus input\n" +
      "H / L    back / forward                   r     reload\n" +
      "J,gT / K,gt  prev / next tab              t / x new / close tab\n" +
      "o        focus URL bar                    p / P open clipboard (here / new tab)\n" +
      "yy       copy URL      / n N  find, next, prev      i  insert mode\n" +
      "Esc      leave mode    ?  toggle this help";
    helpUi.root.appendChild(pre);
  }

  // ---------- command table -------------------------------------------------
  const commands = {
    j: () => scrollBy(0, SCROLL_STEP),
    k: () => scrollBy(0, -SCROLL_STEP),
    h: () => scrollBy(-SCROLL_STEP, 0),
    l: () => scrollBy(SCROLL_STEP, 0),
    d: () => scrollBy(0, vh() / 2),
    u: () => scrollBy(0, -vh() / 2),
    gg: () => scrollTo(true),
    G: () => scrollTo(false),
    f: () => startHints(false),
    F: () => startHints(true),
    gi: () => {
      for (const el of document.querySelectorAll(
        "input,textarea,[contenteditable]",
      )) {
        const r = el.getBoundingClientRect();
        if (isTextInput(el) && r.width > 0 && r.height > 0) {
          el.focus();
          el.scrollIntoView({ block: "center" });
          return;
        }
      }
    },
    H: () => history.back(),
    L: () => history.forward(),
    r: () => location.reload(),
    J: () => send("tabprev"),
    gT: () => send("tabprev"),
    K: () => send("tabnext"),
    gt: () => send("tabnext"),
    t: () => send("tabnew"),
    x: () => send("tabclose"),
    o: () => send("urlfocus"),
    p: () => send("paste"),
    P: () => send("pastetab"),
    yy: () => send("yank " + location.href),
    "/": () => openFind(),
    n: () => doFind(false),
    N: () => doFind(true),
    i: () => {
      mode = "insert";
    },
    "?": () => toggleHelp(),
  };
  const prefixes = new Set();
  for (const k of Object.keys(commands))
    for (let i = 1; i < k.length; i++) prefixes.add(k.slice(0, i));

  // ---------- key handling --------------------------------------------------
  function consume(e) {
    e.preventDefault();
    e.stopImmediatePropagation();
    swallowed.add(e.code);
  }

  window.addEventListener(
    "keydown",
    (e) => {
      if (!e.isTrusted) return;

      if (mode === "hint") return hintKey(e);

      if (mode === "find") {
        e.stopImmediatePropagation(); // keep typing away from the page
        if (e.key === "Enter") {
          e.preventDefault();
          findQuery = findUi ? findUi.input.value : "";
          closeFind();
          doFind(false);
        } else if (e.key === "Escape") {
          e.preventDefault();
          closeFind();
        }
        return;
      }

      if (e.ctrlKey || e.metaKey || e.altKey) return; // Qt shortcuts / page
      if (
        e.key === "Shift" ||
        e.key === "Control" ||
        e.key === "Alt" ||
        e.key === "Meta"
      )
        return;

      if (mode === "insert" || isTextInput(deepActive())) {
        if (e.key === "Escape") {
          const a = deepActive();
          if (a && a.blur) a.blur();
          mode = "normal";
          consume(e);
        }
        return; // everything else goes to the field
      }

      if (e.key === "Escape") {
        buf = "";
        if (helpUi) toggleHelp();
        return; // let the page see Escape (modals etc.)
      }

      const seq = buf + e.key;
      if (commands[seq]) {
        buf = "";
        consume(e);
        commands[seq]();
      } else if (prefixes.has(seq)) {
        buf = seq;
        consume(e);
      } else {
        buf = "";
      }
    },
    true,
  );

  // Keep the page from seeing the keyup of keys we consumed.
  window.addEventListener(
    "keyup",
    (e) => {
      if (swallowed.delete(e.code)) e.stopImmediatePropagation();
    },
    true,
  );

  // Leave hint mode if the window itself loses focus (non-capture: element
  // blurs don't bubble, so only the window's own blur reaches this).
  window.addEventListener("blur", () => {
    if (mode === "hint") exitHints();
  });
})();
