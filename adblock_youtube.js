// YouTube ad handling. Injected into every frame at document creation, in the
// page's main world, before any page script runs. It does nothing outside
// youtube.com / youtube-nocookie.com.
//
// It does by hand what uBlock Origin's YouTube rules do, because the scriptlets
// those rules call are not in the resources.json the engine loads (the filter
// engine silently drops rules whose scriptlet it does not have):
//
//   1. pruneAdData          remove ad descriptors from the player data
//   2. hideAdContainers     hide ad slots that are still rendered
//   3. skipServedAds        if an ad plays anyway: mute, fast-forward, skip
//   4. avoidStartupBackoff  the current, server-side start-up delay
//
// Every step is independent and guarded: one that stops working (YouTube
// changes things often) never takes the others down.
(function () {
  "use strict";
  if (!/(^|\.)youtube(-nocookie)?\.com$/.test(location.hostname)) return;

  // YouTube's classic ad-blocker probe: static.doubleclick.net/instream/
  // ad_status.js sets this flag when it loads. That request is blocked, and
  // uBO answers it with a stub that sets the same flag, so do the same.
  try {
    window.google_ad_status = 1;
  } catch (e) {}

  [pruneAdData, hideAdContainers, skipServedAds, avoidStartupBackoff].forEach(
    function (step) {
      try {
        step();
      } catch (e) {}
    },
  );

  // ---------------------------------------------------------------------
  // 1. Remove the client-side ad descriptors from the player data before the
  // player sees them (what uBO's json-prune rules for YouTube do).
  // ---------------------------------------------------------------------
  function pruneAdData() {
    var AD_KEYS = ["adPlacements", "playerAds", "adSlots"];
    var API = /\/youtubei\/v1\/(player|next|get_watch|reel)/;

    // Ad keys sit at the top level (/player) and under .playerResponse (the
    // shape /next uses).
    function prune(o) {
      if (o === null || typeof o !== "object") return o;
      var targets = [o];
      if (o.playerResponse !== null && typeof o.playerResponse === "object")
        targets.push(o.playerResponse);
      targets.forEach(function (t) {
        AD_KEYS.forEach(function (k) {
          if (k in t) {
            try {
              delete t[k];
            } catch (e) {}
          }
        });
      });
      return o;
    }

    // The inline <script> that seeds the first video of a page load.
    var initial;
    Object.defineProperty(window, "ytInitialPlayerResponse", {
      configurable: true,
      enumerable: true,
      get: function () {
        return initial;
      },
      set: function (v) {
        initial = prune(v);
      },
    });

    // Anything parsed from text (XHR, response.text(), embedded JSON).
    JSON.parse = new Proxy(JSON.parse, {
      apply: function (target, thisArg, args) {
        return prune(Reflect.apply(target, thisArg, args));
      },
    });

    // fetch() responses read with response.json(), which never touches
    // JSON.parse. This is how the page fetches the next video's player data.
    Response.prototype.json = new Proxy(Response.prototype.json, {
      apply: function (target, thisArg, args) {
        var p = Reflect.apply(target, thisArg, args);
        var url = "";
        try {
          url = thisArg.url || "";
        } catch (e) {}
        return API.test(url) ? p.then(prune) : p;
      },
    });
  }

  // ---------------------------------------------------------------------
  // 2. Hide the ad containers that are still rendered (feed and sidebar ads),
  // and the picture of an ad that is waited out in step 3.
  // ---------------------------------------------------------------------
  function hideAdContainers() {
    var HIDE = [
      "ytd-ad-slot-renderer",
      "ytd-in-feed-ad-layout-renderer",
      "ytd-promoted-sparkles-web-renderer",
      "ytd-promoted-video-renderer",
      "ytd-display-ad-renderer",
      "ytd-companion-slot-renderer",
      "ytd-action-companion-ad-renderer",
      "ytd-player-legacy-desktop-watch-ads-renderer",
      "#masthead-ad",
      "#player-ads",
      "ytd-rich-item-renderer:has(ytd-ad-slot-renderer)",
      "ytd-rich-section-renderer:has(ytd-ad-slot-renderer)",
    ];
    // One rule per selector, so a selector this Chromium cannot parse only
    // drops itself.
    var css =
      HIDE.map(function (s) {
        return s + "{display:none!important}";
      }).join("\n") +
      // An ad that is really served (step 3) is waited out unseen: picture
      // hidden, muted, with a small note instead.
      "\n#movie_player.ad-showing .html5-video-container," +
      "#movie_player.ad-showing .ytp-ad-image-overlay," +
      "#movie_player.ad-showing .ytp-ad-overlay-container{opacity:0!important}" +
      "\n#movie_player.ad-showing{background:#000!important}" +
      '\n#movie_player.ad-showing::after{content:"Waiting out an ad (muted)";' +
      "position:absolute;left:0;right:0;bottom:46%;text-align:center;color:#888;" +
      "font:14px/1.4 sans-serif;pointer-events:none;z-index:1}";

    function addStyle() {
      var parent = document.head || document.documentElement;
      if (!parent) return false;
      var style = document.createElement("style");
      style.textContent = css;
      parent.appendChild(style);
      return true;
    }
    // documentElement does not exist yet at document creation.
    if (!addStyle()) {
      var observer = new MutationObserver(function () {
        if (addStyle()) observer.disconnect();
      });
      observer.observe(document, { childList: true, subtree: true });
    }
  }

  // ---------------------------------------------------------------------
  // 3. An ad that is really served (one the pruning missed). YouTube's media
  // server paces such an ad on its own clock, so jumping into data it has not
  // sent yet only leaves the player stuck. Instead: mute it, play it at the
  // maximum speed, jump to its end only once all of it is buffered, press
  // "Skip" as soon as there is one, and nudge a player that sits at the end of
  // an ad without moving on. The user's mute state and speed are put back
  // afterwards.
  // ---------------------------------------------------------------------
  function skipServedAds() {
    var SKIP =
      ".ytp-skip-ad-button,.ytp-ad-skip-button,.ytp-ad-skip-button-modern," +
      ".ytp-ad-skip-button-container button,.ytp-skip-ad button,.ytp-ad-skip-button-slot button";
    var saved = null,
      atEndSince = 0,
      endedSent = false;

    // pointer + mouse + click, like a real press (a bare .click() is not
    // always enough for YouTube's handlers)
    function press(button) {
      ["pointerdown", "mousedown", "pointerup", "mouseup", "click"].forEach(
        function (type) {
          try {
            var Ev =
              type.indexOf("pointer") === 0 && window.PointerEvent
                ? PointerEvent
                : MouseEvent;
            button.dispatchEvent(
              new Ev(type, {
                bubbles: true,
                cancelable: true,
                composed: true,
                view: window,
                button: 0,
              }),
            );
          } catch (e) {}
        },
      );
    }

    setInterval(function () {
      try {
        var player = document.getElementById("movie_player");
        if (!player) return;
        var video = player.querySelector("video");
        if (!video) return;
        var now = Date.now();

        if (!player.classList.contains("ad-showing")) {
          if (saved) {
            video.muted = saved.muted;
            video.playbackRate = saved.rate;
            saved = null;
          }
          return;
        }

        if (!saved) {
          saved = { muted: video.muted, rate: video.playbackRate };
          atEndSince = 0;
          endedSent = false;
        }
        video.muted = true;
        if (video.playbackRate !== 16) video.playbackRate = 16;
        var duration = video.duration,
          time = video.currentTime;
        var buffered = video.buffered;
        var bufferedEnd =
          buffered && buffered.length ? buffered.end(buffered.length - 1) : 0;
        var known = isFinite(duration) && duration > 0;
        var atEnd = video.ended || (known && time >= duration - 0.3);
        // only into data that is already there
        if (known && !atEnd && bufferedEnd >= duration - 0.3)
          video.currentTime = duration;
        if (video.paused && !atEnd) {
          try {
            video.play();
          } catch (e) {}
        }

        var button = player.querySelector(SKIP);
        if (
          button &&
          !button.disabled &&
          button.getAttribute("aria-disabled") !== "true"
        )
          press(button);

        // sitting at the end of the ad: nudge the player after 1.5 s
        if (!atEnd) {
          atEndSince = 0;
        } else {
          if (!atEndSince) atEndSince = now;
          if (!endedSent && now - atEndSince > 1500) {
            endedSent = true;
            try {
              video.dispatchEvent(new Event("ended"));
            } catch (e) {}
          }
        }
      } catch (e) {}
    }, 250);
  }

  // ---------------------------------------------------------------------
  // 4. YouTube's current start-up delay ("fake buffering").
  //
  // The media server puts a backoff into the first /videoplayback (SABR) reply
  // of a video: no media, only a NEXT_REQUEST_POLICY whose backoffTimeMs is
  // about 80% of the length of the ads the video was going to get (16 s and
  // more). The server measures that wait with its own clock (a reply that had
  // the backoff cut out was answered with the remainder), so it cannot be
  // shortened on the page, and pruning the ad data (step 1) does not change
  // what the server already decided.
  //
  // What works is asking for the video in a request context for which no ads
  // are scheduled. This is a port of what uBlock Origin's quick-fixes list does
  // (the "trusted-edit-inbound-object, JSON.stringify" rules in its YouTube
  // section): when a start-up backoff arrives and the player is stalled with
  // nothing buffered, the video is reloaded from the same position with
  // player.loadVideoById(), and the data request is tagged. The tag travels in
  // the client's userAgent; a JSON.stringify hook turns it into request
  // fields. The four tags are tried in order, a tag that does not help is
  // dropped when the reloaded video stalls too, and a tag that works stays in
  // place for the rest of the session.
  //
  // This is the fragile part: the tags and fields are what uBO's maintainers
  // found to work, and YouTube changes the rules every few months. If the
  // start-up wait comes back, compare with that section of quick-fixes.txt.
  // When every tag fails the player simply waits as it would without this.
  // ---------------------------------------------------------------------
  function avoidStartupBackoff() {
    var TAGS = ["channel", "lactmilli", "inline", "yahi"];
    var MIN_BACKOFF = 2000; // ms; shorter ones are ordinary pacing
    var left = TAGS.slice(); // tags not given up on yet
    var armed = false; // a start-up backoff arrived while the player was stalled
    var originalUA = null;
    var reloadedId = null; // video that was reloaded by this step

    // ---- tagging the request ----

    function ytClient() {
      try {
        return window.ytcfg.data_.INNERTUBE_CONTEXT.client;
      } catch (e) {
        return null;
      }
    }

    // The tag is added to the platform part of the userAgent, as in uBO:
    // "Mozilla/5.0 (X11; Linux x86_64; channel) ...". An empty tag restores it.
    function setTag(tag) {
      var client = ytClient();
      if (!client) return;
      if (originalUA === null) originalUA = client.userAgent;
      var platform = tag
        ? String(originalUA).match(/Mozilla\/5\.0 \([^)]+/)
        : null;
      var ua = platform
        ? String(originalUA).replace(platform[0], platform[0] + "; " + tag)
        : originalUA;
      if (client.userAgent !== ua) client.userAgent = ua;
    }

    // The player builds its request bodies with JSON.stringify(object). A body
    // that carries a tag gets the fields that go with it.
    JSON.stringify = new Proxy(JSON.stringify, {
      apply: function (target, thisArg, args) {
        try {
          var body = args[0];
          if (
            body !== null &&
            typeof body === "object" &&
            body.attestationRequest &&
            body.context &&
            body.context.client
          ) {
            var ua = String(body.context.client.userAgent || "");
            if (/channel|lactmilli|inline|yahi/.test(ua)) {
              var tagged = function (tag) {
                return ua.indexOf(tag) !== -1;
              };
              var playback =
                body.playbackContext &&
                body.playbackContext.contentPlaybackContext;
              if (tagged("channel") && body.context.client.clientName === "WEB")
                body.context.client.clientScreen = "CHANNEL";
              if (tagged("lactmilli")) body.params = "8AUB";
              if (tagged("yahi")) body.params = "YAHI";
              if (playback) {
                if (tagged("channel") || tagged("lactmilli"))
                  playback.lactMilliseconds = String(Date.now());
                // Marks the request as a reload; the reloaded video's playback
                // tracking URL then contains "reloadxhr" (see onStartupBackoff).
                playback.referer = String(playback.referer || "").replace(
                  /(?:#reloadxhr)?$/,
                  "#reloadxhr",
                );
              }
            }
          }
        } catch (e) {}
        return Reflect.apply(target, thisArg, args);
      },
    });

    // ---- the player ----

    function playerState(withStats) {
      var player = document.getElementById("movie_player");
      if (!player) return null;
      var state = { player: player };
      try {
        state.response = player.getPlayerResponse && player.getPlayerResponse();
      } catch (e) {}
      try {
        state.progress = player.getProgressState && player.getProgressState();
      } catch (e) {}
      if (withStats) {
        try {
          state.stats = player.getStatsForNerds && player.getStatsForNerds();
        } catch (e) {}
        try {
          state.buffering =
            player.getPlayerStateObject &&
            player.getPlayerStateObject().isBuffering;
        } catch (e) {}
      }
      return state;
    }

    // Buffering, nothing buffered, nothing decoded yet.
    function stalled(state) {
      return !!(
        state &&
        state.buffering &&
        state.stats &&
        state.stats.buffer_health_seconds === "0.00 s" &&
        state.stats.resolution === "0x0"
      );
    }

    // YouTube's "ad blockers are not allowed" error page.
    function blockedPage(response) {
      try {
        var status = response.playabilityStatus,
          screen = status && status.errorScreen;
        if (
          !status ||
          status.status !== "UNPLAYABLE" ||
          (screen &&
            screen.playerErrorMessageRenderer &&
            screen.playerErrorMessageRenderer.playerCaptchaViewModel)
        )
          return false;
        var runs = JSON.stringify(
          (screen &&
            screen.playerErrorMessageRenderer &&
            screen.playerErrorMessageRenderer.subreason &&
            screen.playerErrorMessageRenderer.subreason.runs) ||
            (screen &&
              screen.playerInterstitialRenderer &&
              screen.playerInterstitialRenderer.content &&
              screen.playerInterstitialRenderer.content.interstitialViewModel &&
              screen.playerInterstitialRenderer.content.interstitialViewModel
                .description &&
              screen.playerInterstitialRenderer.content.interstitialViewModel
                .description.commandRuns),
        );
        return (
          !!runs &&
          runs.indexOf("WEB_PAGE_TYPE_UNKNOWN") !== -1 &&
          runs.indexOf("https://support.google.com/youtube/answer/3037019") !==
            -1
        );
      } catch (e) {
        return false;
      }
    }

    // A start-up backoff was seen. If the player really is stalled, arm the reload.
    function onStartupBackoff() {
      var state = playerState(true);
      if (!stalled(state) || !left.length) return;
      var tracking = state.response && state.response.playbackTracking;
      var url =
        tracking &&
        tracking.videostatsPlaybackUrl &&
        tracking.videostatsPlaybackUrl.baseUrl;
      // This load was already a retry with left[0], and it stalled too.
      if (url && String(url).indexOf("reloadxhr") !== -1) left = left.slice(1);
      armed = true;
    }

    // Runs four times a second: reloads the video once the reload is armed.
    function tick() {
      try {
        var href = location.href;
        if (href.indexOf("/watch?") === -1 && href.indexOf("/embed/") === -1) {
          left = TAGS.slice();
          armed = false;
          return;
        }
        var state = playerState(false);
        if (!state || !state.response || !state.response.videoDetails) return;
        var response = state.response,
          progress = state.progress,
          id = response.videoDetails.videoId;
        var unfinished =
          (progress &&
            progress.duration > 0 &&
            (progress.loaded < progress.duration ||
              progress.duration - progress.current > 1)) ||
          response.videoDetails.isLive;
        if (!unfinished || !id) return;
        var start =
          (response.playerConfig &&
            response.playerConfig.playbackStartConfig &&
            response.playerConfig.playbackStartConfig.startSeconds) ||
          parseInt(new URLSearchParams(location.search).get("t"), 10) ||
          0;

        if (blockedPage(response)) {
          left = left.slice(1);
          setTag(left[0] || "");
          armed = false;
          reloadedId = id;
          state.player.loadVideoById(id, start);
          return;
        }
        if (!left.length) {
          armed = false;
          setTag("");
          return;
        }
        if (armed) {
          if (!stalled(playerState(true))) return;
          setTag(left[0]);
          armed = false;
          reloadedId = id;
          state.player.loadVideoById(id, start);
          return;
        }
        // loadVideoById() drops the playlist the page was in: put it back.
        if (
          reloadedId === id &&
          progress &&
          progress.current - start > 0.1 &&
          progress.current - start < 5 &&
          href.indexOf("&list=") !== -1 &&
          state.player.getPlaylistId &&
          state.player.getPlaylistId() === null
        ) {
          var manager = document.querySelector("yt-playlist-manager");
          var data =
            manager && manager.getPlaylistData && manager.getPlaylistData();
          if (data) {
            if (manager.setPlaylistData) manager.setPlaylistData(data);
            if (manager.setPlayerPlaybackControlData)
              manager.setPlayerPlaybackControlData({
                playlistPanelRenderer: data,
              });
          }
        }
      } catch (e) {}
    }
    setInterval(tick, 250);

    // ---- seeing the start-up backoff: SABR replies are watched, never changed ----
    //
    // A SABR reply is a UMP stream: [type varint][size varint][payload]...
    // A reply too small to hold media that carries a NEXT_REQUEST_POLICY (type
    // 35) with a long backoffTimeMs (protobuf field 4) is the start-up delay.

    var SABR_URL = /\/videoplayback[^#]*[?&]sabr=1/;

    // UMP varint -> [value, next index], null if cut off
    function umpAt(b, i) {
      var b0 = b[i],
        n = b0 < 128 ? 1 : b0 < 192 ? 2 : b0 < 224 ? 3 : b0 < 240 ? 4 : 5;
      if (i + n > b.length) return null;
      if (n === 1) return [b0, i + 1];
      if (n === 2) return [(b0 & 0x3f) + 64 * b[i + 1], i + 2];
      if (n === 3)
        return [(b0 & 0x1f) + 32 * (b[i + 1] + 256 * b[i + 2]), i + 3];
      if (n === 4)
        return [
          (b0 & 0x0f) + 16 * (b[i + 1] + 256 * (b[i + 2] + 256 * b[i + 3])),
          i + 4,
        ];
      return [
        b[i + 1] + 256 * (b[i + 2] + 256 * (b[i + 3] + 256 * b[i + 4])),
        i + 5,
      ];
    }

    // protobuf varint -> [value, next index], null if cut off
    function pbVarint(b, k, end) {
      var v = 0,
        m = 1,
        x;
      do {
        if (k >= end || m > 1e15) return null;
        x = b[k++];
        v += (x & 0x7f) * m;
        m *= 128;
      } while (x & 0x80);
      return [v, k];
    }

    // backoffTimeMs (field 4) of a NEXT_REQUEST_POLICY payload, 0 if absent
    function policyBackoff(b, k, end) {
      var backoff = 0;
      while (k < end) {
        var key = pbVarint(b, k, end);
        if (!key) return 0;
        k = key[1];
        var field = Math.floor(key[0] / 8),
          wireType = key[0] % 8;
        if (wireType === 0) {
          var value = pbVarint(b, k, end);
          if (!value) return 0;
          k = value[1];
          if (field === 4) backoff = value[0];
        } else if (wireType === 2) {
          var length = pbVarint(b, k, end);
          if (!length) return 0;
          k = length[1] + length[0];
        } else if (wireType === 1) {
          k += 8;
        } else if (wireType === 5) {
          k += 4;
        } else {
          return 0;
        }
      }
      return backoff;
    }

    // Is this a complete, media-free reply that tells the player to wait long?
    function isStartupBackoff(b) {
      var i = 0,
        backoff = 0;
      while (i < b.length) {
        var type = umpAt(b, i);
        if (!type) return false;
        var size = umpAt(b, type[1]);
        if (!size) return false;
        var end = size[1] + size[0];
        if (end > b.length) return false;
        if (type[0] === 20 || type[0] === 21 || type[0] === 22) return false; // media header / media / media end
        if (type[0] === 35)
          backoff = Math.max(backoff, policyBackoff(b, size[1], end));
        i = end;
      }
      return backoff >= MIN_BACKOFF;
    }

    function watchReply(clone) {
      var reader = clone.body.getReader(),
        chunks = [],
        length = 0;
      (function pump() {
        reader.read().then(
          function (chunk) {
            if (chunk.done) {
              var bytes = new Uint8Array(length),
                offset = 0;
              chunks.forEach(function (c) {
                bytes.set(c, offset);
                offset += c.length;
              });
              if (isStartupBackoff(bytes)) onStartupBackoff();
              return;
            }
            chunks.push(chunk.value);
            length += chunk.value.length;
            if (length > 1024) {
              reader.cancel();
              return;
            } // too big for a policy-only reply: it is media
            pump();
          },
          function () {},
        );
      })();
    }

    // The page gets the real response object back; only a clone is read.
    window.fetch = new Proxy(window.fetch, {
      apply: function (target, thisArg, args) {
        var promise = Reflect.apply(target, thisArg, args);
        var url = "";
        try {
          url =
            typeof args[0] === "string"
              ? args[0]
              : String((args[0] && args[0].url) || args[0]);
        } catch (e) {}
        if (!SABR_URL.test(url)) return promise;
        return promise.then(function (response) {
          try {
            if (response.status === 200 && response.body)
              watchReply(response.clone());
          } catch (e) {}
          return response;
        });
      },
    });
  }
})();
