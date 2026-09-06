# Home network first, and the phone's camera stream mode

Branch `feat/phone-lan-fallback`, 2026-09-06. Written after the work, describing what is in the
branch, what it cannot do, and why.

## 1. The problem, in the user's words

Two complaints, one cause.

- *"The Devices page also fails when Tailscale is off."* Every link the phone was ever handed - the
  QR code that was scanned, the Web Push notification that was tapped, the home-screen icon that
  was installed - is the Tailscale one, `https://<machine>.<tailnet>.ts.net/r/<token>/`. The hub
  has always served the same page on the LAN as well, at `http://<lan ip>:13640/r/<token>/`, but
  nothing on the phone ever pointed there. So a phone standing next to the PC went out to the
  tailnet and back, and when Tailscale was off it went nowhere at all.
- *"The stream constantly pauses and buffers."* On the remote origin, iOS Safari rendered stream
  card U1 with an `MSE` badge, its own native video controls, and a 15-second finite timeline
  (`00:10 / -00:05`) with a play button. That is not a live stream; it is a movie the phone keeps
  running out of.

Both are consequences of a single design fact, which §2 states, so this note states it once.

## 2. The one fact: two origins that share nothing

The hub answers on two origins:

| | Home | Away |
|---|---|---|
| URL | `http://<lan ip>:13640/r/<token>/` | `https://<machine>.<tailnet>.ts.net/r/<token>/` |
| Secure context | no | yes |
| Reaches the hub | only on the LAN | from anywhere, through `tailscale serve` |

Scheme, host and port all differ, so by definition these are two origins. Two cookie jars, two
`localStorage` buckets, two service-worker registrations, two push subscriptions, two permission
grants. Nothing crosses. A preference stored on one is invisible on the other, and *that is the
correct behaviour here*: "when I land on the remote page, take me home" is a statement about the
remote origin, and only the remote origin has to remember it.

The consequence for the code: the page has to be told **both** URLs by the hub, and the only thing
that can move a phone between them is a top-level navigation.

## 3. What was built

### 3.1 The hub says where it can be reached

- `GET /hub/info` (loopback control plane) gained `lan_url`, `remote_url` and already carried
  `ips`. `lan_url` is `http://<first LAN ip>:<port>/r/<token>/`, the same string `url` has always
  been; `remote_url` is `https://<dns name>/r/<token>/`, empty while remote access is off.
- `GET /r/<token>/state` - the phone's own info endpoint, which the page already polls every 15 s -
  gained the same three fields. A visitor here already holds the token (and, through Serve, an
  allow-listed tailnet login), so the PC's LAN address tells them nothing the link they used did
  not.
- Both are computed by one new function, `HubServer::phone_links()`, which is also what the
  notification relays are fed. There is now exactly one place that decides what the two links are.

### 3.2 The Connection control

A persistent control in the phone page's header, next to the `Remote · <login>` indicator:

- it names where you are - **Home** or **Away** - decided by comparing `location.origin` against
  the two URLs the hub reported, not by guessing from the scheme;
- on the Away origin, with a LAN URL known, it offers **Switch to home network**; on the Home
  origin it offers **Switch to remote**;
- a switch is a same-tab navigation that keeps the path, the query and the fragment, and takes the
  token from the URL the hub handed over, so nothing has to be carried across by hand;
- the choice is written to `localStorage['snorca_conn_pref']` (per origin, necessarily) and
  mirrored into the Cache API for the service worker - see §3.4;
- every request-failure state that used to end in a sentence now ends in a **Try the home network**
  button as well, when a LAN URL exists and we are on the remote origin: the top bar's failure
  note, the Prepare tab's *Cannot reach the slicer*, and the Devices tab's.

### 3.3 The automatic hop, and the mixed-content limit

On load, on the Away origin, with the preference set to `home`, the page probes the LAN hub and
navigates there if the probe succeeds. The probe is an `<img>` beacon against a new hub route,
`GET /r/<token>/ping.gif` - 43 bytes of transparent GIF, `Cache-Control: no-store`,
`Pragma: no-cache`, `Expires: 0`, no caching anywhere - with a ~1.5 s timeout. A probe that has
been attempted writes a `sessionStorage` flag *before* it runs, so the hop is attempted at most
once per tab per address and can never loop.

**The constraint, stated plainly.** The remote origin is `https:`. Any *request* it makes to
`http://192.168.x.x:13640` is active mixed content: the browser blocks it before a packet leaves
and rejects with an opaque `TypeError`, so `fetch()` cannot even be used as a reachability test.
An `<img>` is *passive* mixed content and gets further - but Chromium 81+ and Firefox silently
auto-upgrade image loads to `https`, which the hub does not speak, so on those browsers the beacon
reports failure even when the phone is standing on the LAN. WebKit still attempts the plain `http`
load, which is why the beacon is worth having on the phone this was written for.

So: **the probe can say "yes, go" and can never say "no, stay."** That asymmetry is why the manual
button is always visible rather than appearing only after a successful probe - a top-level
navigation from `https` to `http` is not mixed content at all, so the button always works even
where the probe cannot run. This is written into the code, above `connProbe`, so nobody later
"fixes" the beacon into a `fetch`.

The only way to remove the asymmetry would be to give the LAN hub a certificate a phone trusts,
which means a real domain, a real ACME flow and a name that resolves to a private address - a much
larger piece of work than this, and one a native app makes unnecessary (§6).

### 3.4 Notifications carry both links

- **Web Push**: the payload JSON gained `lan_url` and `remote_url` alongside the existing `url`
  (which stays remote-first). `sw.js`'s `notificationclick` now opens the preferred one.
  A service worker cannot read `localStorage` - workers have no access to it - and a notification
  is tapped precisely when no page of ours is open to ask. So the page mirrors the preference into
  the Cache API (`snorca-conn`, key `/__conn_pref`), which both contexts can reach, and the worker
  reads it there. Order: an already-open window of ours wins (the person is looking at a working
  page; moving them would drop it), then the preferred origin, then remote, then whatever single
  link the payload had, then the worker's own scope.
- **ntfy / Pushover / webhook**: unchanged in shape. The single `Click` / `url` / `link` stays the
  remote link, because a relay's one link has to work from wherever the phone is. The LAN link is
  appended to the body - one line, `On your home network: <url>`, and only when the two differ -
  and a webhook additionally gets a `lan_link` field.

### 3.5 `hub.html`

The two QR sections are now **Home (this network)** and **Away (Tailscale)**, and each says that
the phone page's Connection control switches between them and remembers the choice. Scanning both
is now the intended setup, not a curiosity.

## 4. The stream mode decision

### 4.1 How `video-rtc.js` actually chooses

`video-rtc.js` is a verbatim copy of go2rtc 1.9.14's player. Its `onopen()`:

1. if the mode string **contains** `mse` and the browser has `MediaSource` *or*
   `ManagedMediaSource` → MSE;
2. else if it contains `hls` and the `<video>` can play HLS → HLS;
3. else if it contains `mp4` → mp4;
4. independently, if it contains `webrtc` and `RTCPeerConnection` exists → WebRTC is started as
   well, and takes over afterwards if its priority beats MSE's;
5. `mjpeg`, if present, is a fallback armed to fire when the first mode reports an error.

The test is `includes()`, not order. **The order inside the mode string is never read.** Choosing a
mode therefore means choosing which words appear in the string at all - which is why the old
`&mode=mse` gave MSE and nothing else, on every device.

### 4.2 Why MSE is the wrong mode on iOS, and worse over Tailscale

In the MSE path the player keeps a ~5 s sliding window, trims the source buffer behind it, calls
`setLiveSeekableRange(start, end)`, nudges `currentTime` and varies `playbackRate` to chase the
live edge - and `oninit()` sets `video.controls = true` unconditionally. On iOS that combination
renders as an ordinary movie: native transport controls, a short finite timeline, a play button.
That is exactly the screenshot.

On top of that, iOS's MediaSource *is* `ManagedMediaSource`: the system decides when the element
may buffer and is free to starve it under memory or network pressure. Over Tailscale the added
latency makes the sliding window underrun constantly, so it stalls, shows the play button, and the
person taps it again.

`mp4` and `mjpeg` have neither problem. Both set `video.controls = false` and paint each frame as
it arrives (as the element's poster), so there is no timeline to fall behind and nothing to pause.
The cost is honest and worth writing down: **both are frame-at-a-time paths, not smooth video.**
`mp4` decodes each fragment through a hidden `<video>` onto a canvas; `mjpeg` needs the source to
be MJPEG or transcoded, and no ffmpeg is bundled, so for the H.264 printer cameras here `mjpeg` is
a last-ditch fallback that will usually fail and is ordered accordingly. For a camera wall that
exists to answer "is it still printing", a reliable low-rate picture beats a stalling movie.

### 4.3 Why not HLS

iOS Safari would take branch 2 happily - `canPlayType('application/vnd.apple.mpegurl')` says yes -
and it is the one mode that would give real smooth live video on an iPhone. It is deliberately
never in the string, because the player then fetches `/hls/...` straight off the page origin, and
the hub tunnels exactly one go2rtc path, `/api/ws`. Enabling it needs **two** changes: a second
tunnel route in `HubServer::serve`, and `/api/hls` added to `allow_paths` in the generated
`go2rtc.yaml`. That widens the hub's attack surface and the credential-only go2rtc API's, for a
mode nothing else needs. `mp4` rides the tunnel that already exists. If the low frame rate turns
out to matter in practice, HLS is the first thing to reach for, and this paragraph is the recipe.

### 4.4 Why not WebRTC

This is the answer to "check how the relay is configured". The hub's generated `go2rtc.yaml`
(`HubServer::start_go2rtc`) contains:

```
rtsp:   listen: ""
webrtc: listen: ""
srtp:   listen: ""
```

With no WebRTC listener go2rtc has no host candidate to put in an answer, so a browser offer is
never usably answered - **not even on the LAN**. WebRTC is therefore not "the LAN default that
breaks when remote"; it has never worked in this build, which is why both callers hardcoded
`mode=mse` in the first place.

And enabling it would not fix the remote path anyway: Tailscale Serve is an HTTPS reverse proxy
that forwards TCP to `127.0.0.1:<port>`. WebRTC media is UDP. Serve cannot carry it. Over the
remote path WebRTC could only work if the phone were on the tailnet *itself* (the Tailscale app
routing directly to the PC's tailnet address) rather than behind Serve, and even then go2rtc would
need a listener bound and a candidate advertising an address the phone can reach.

Nothing about go2rtc was changed. The generator now carries a comment saying all of the above, so
the next person does not have to re-derive it. `stream_center.html` has one line,
`var WEBRTC_RELAY = false;`, to flip if somebody enables the listener; WebRTC is already in the
manual override list, with `mp4,mjpeg` behind it so a dead offer is a picture rather than a black
square.

### 4.5 The order that shipped

| where | mode string |
|---|---|
| Home origin, desktop browser | `mse` - byte for byte what it was |
| Home origin, iOS | `mp4,mjpeg` |
| Away origin, any device | `mp4,mjpeg` |
| ... with `WEBRTC_RELAY` on | `webrtc,mp4,mjpeg` |
| manual override | `webrtc` (→ `webrtc,mp4,mjpeg`), `mp4`, `mjpeg`, `mse` |

The override is a per-card picker stored in `localStorage['snorca_stream_mode']`, keyed by camera
id. Its closed state doubles as the badge - it reads `Auto · MP4` - and the player's own corner
badge inside the iframe still shows the mode it actually settled on, which is the check on this
one. Changing the picker rebuilds only that card.

## 5. What is verified, and what is not

Verified by the gate (`test_phone_ui2.py`, an isolated data dir and a hub on a scratch port):
both URLs on `/hub/info` and on `/r/<token>/state`; `ping.gif`'s status, size, type and no-cache
headers, and its 404 under a wrong token; the page markup for the Connection control, the beacon,
the session guard and the per-card override; `sw.js`'s payload fields, cache read and click order;
`hub.html`'s two section titles; and a real Web Push notification whose decrypted payload carries
`lan_url` and `remote_url` equal to what the hub reports for itself.

**Not verified, and it cannot be here:** there is no iPhone on this machine. The stream-mode
reasoning is read off `video-rtc.js`'s source and go2rtc's protocol, not observed on the device.
What an iPhone would settle is: whether `mp4`'s frame rate is tolerable for a print in progress,
whether the `<img>` beacon really crosses the mixed-content line in the current Safari, and whether
the automatic hop feels instant or merely quick. Nothing here is *worse* than today on iOS if it
does not - the manual switch and the manual override are both one tap.

Also unverified: `remote_url` was exercised only in its empty form, because turning Tailscale
Serve on or off is the user's setting and no gate may touch it. The gate asserts that the phone's
value equals the hub's own, whichever that is.

## 6. What a native app would remove

Most of this note.

- No two origins. An app talks to whichever address answers and has one settings store, so there
  is no preference to mirror, no `localStorage` split, and no service worker in the middle.
- No mixed content. An app can try the LAN address directly, with a real timeout and a real error,
  and fall back to the tailnet in a few hundred milliseconds - the probe becomes ordinary code.
  It can also pin the hub's own certificate and stop needing anyone else's.
- No `video.controls` and no `ManagedMediaSource`. A native player decodes H.264 from the hub with
  a live policy of its own, so the stream is smooth and the mode picker disappears.
- Notification taps route inside the app, so `lan_url` / `remote_url` in the push payload collapse
  into "open the printer screen".

Until then, the split above is the honest shape of the problem, and the Connection control is the
smallest thing that makes it navigable.
