// ==UserScript==
// @name         Screen Share Sound (Firefox)
// @namespace    sharesound
// @version      1.1.1
// @description  Adds system audio to screen sharing in Firefox, which cannot capture it itself
// @match        https://discord.com/*
// @match        https://*.discord.com/*
// @run-at       document-start
// @grant        none
// ==/UserScript==

(function () {
  'use strict';

  var BRIDGE = 'http://127.0.0.1:47823';
  var CONNECT_TIMEOUT_MS = 2500;
  var log = function (m, x) { try { console.log('[sharesound] ' + m, x === undefined ? '' : x); } catch (e) {} };

  // The worklet keeps a small jitter buffer: it waits for PREFILL before
  // starting, drops the oldest audio if the queue grows past MAX (a stalled
  // reader must not add permanent latency), and emits silence on underrun.
  function workletFactory() {
    class PCMPlayer extends AudioWorkletProcessor {
      constructor() {
        super();
        this.q = [];
        this.buffered = 0;
        this.cur = null;
        this.off = 0;
        this.prefill = 48000 * 0.10;
        this.max = 48000 * 0.35;
        this.started = false;
        this.port.onmessage = (e) => {
          const a = new Int16Array(e.data);
          this.q.push(a);
          this.buffered += a.length / 2;
          while (this.buffered > this.max && this.q.length > 1) {
            this.buffered -= this.q.shift().length / 2;
          }
        };
      }
      process(inputs, outputs) {
        const out = outputs[0];
        const L = out[0], R = out[1];
        const n = L.length;
        if (!this.started) {
          if (this.buffered < this.prefill) { L.fill(0); if (R) R.fill(0); return true; }
          this.started = true;
        }
        for (let i = 0; i < n; i++) {
          if (!this.cur || this.off >= this.cur.length) {
            this.cur = this.q.shift() || null;
            this.off = 0;
            if (!this.cur) {
              for (let j = i; j < n; j++) { L[j] = 0; if (R) R[j] = 0; }
              this.started = false;
              return true;
            }
          }
          L[i] = this.cur[this.off] / 32768;
          if (R) R[i] = this.cur[this.off + 1] / 32768;
          this.off += 2;
          this.buffered--;
        }
        return true;
      }
    }
    registerProcessor('sharesound-pcm', PCMPlayer);
  }

  var workletUrl = null;
  function getWorkletUrl() {
    if (!workletUrl) {
      var src = '(' + workletFactory.toString() + ')()';
      workletUrl = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
    }
    return workletUrl;
  }

  // Opens the bridge and returns a live audio track, or null if the helper is
  // not running - in that case the screen share proceeds silently, as before.
  async function openBridgeTrack() {
    var ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'playback' });
    var abort = new AbortController();
    var cleanup = function () {
      try { abort.abort(); } catch (e) {}
      try { ctx.close(); } catch (e) {}
    };
    try {
      await ctx.audioWorklet.addModule(getWorkletUrl());
      if (ctx.state === 'suspended') await ctx.resume();
      var node = new AudioWorkletNode(ctx, 'sharesound-pcm', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
      });
      var dest = ctx.createMediaStreamDestination();
      node.connect(dest);

      var firstChunk = null;
      var gotFirst = new Promise(function (resolve) { firstChunk = resolve; });

      (async function pump() {
        var backoff = 500;
        while (!abort.signal.aborted) {
          try {
            var r = await fetch(BRIDGE + '/pcm', { signal: abort.signal, cache: 'no-store' });
            if (!r.ok || !r.body) throw new Error('bridge http ' + r.status);
            var reader = r.body.getReader();
            backoff = 500;
            for (;;) {
              var res = await reader.read();
              if (res.done) break;
              if (firstChunk) { firstChunk(true); firstChunk = null; }
              node.port.postMessage(res.value.buffer, [res.value.buffer]);
            }
            log('bridge stream ended, reconnecting');
          } catch (e) {
            if (abort.signal.aborted) return;
            log('bridge error, retrying', String(e && e.message || e));
          }
          // the helper may be restarting; keep trying while the share is live
          await new Promise(function (r2) { setTimeout(r2, backoff); });
          backoff = Math.min(backoff * 2, 5000);
        }
      })();

      var ok = await Promise.race([
        gotFirst,
        new Promise(function (r3) { setTimeout(function () { r3(false); }, CONNECT_TIMEOUT_MS); })
      ]);
      if (!ok) { cleanup(); log('helper not reachable at ' + BRIDGE + ', sharing without sound'); return null; }

      var track = dest.stream.getAudioTracks()[0];
      track.addEventListener('ended', cleanup);
      return { track: track, cleanup: cleanup };
    } catch (e) {
      cleanup();
      log('bridge setup failed', String(e && e.message || e));
      return null;
    }
  }

  var MD = window.MediaDevices && window.MediaDevices.prototype;
  if (!MD || !MD.getDisplayMedia) return;

  var original = MD.getDisplayMedia;
  MD.getDisplayMedia = function (constraints) {
    var self = this;
    return original.call(self, constraints).then(async function (stream) {
      // Firefox returns video only; the app asked for audio and gets none.
      if (stream.getAudioTracks().length > 0) return stream;
      var bridge = await openBridgeTrack();
      if (!bridge) return stream;
      stream.addTrack(bridge.track);
      var video = stream.getVideoTracks()[0];
      if (video) video.addEventListener('ended', function () { bridge.cleanup(); });
      log('system audio attached to screen share');
      return stream;
    });
  };
  log('ready');
})();
