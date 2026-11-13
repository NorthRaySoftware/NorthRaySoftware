# Setting up CJ Radio for real

> **What works today, with no server:** CJ Radio shows your Twitch stream in its own window (Listen page) and goes ON AIR when Twitch reports you're live. To add a recording, put the MP3 in `apps/cj-radio/Recordings` (named like `2026-11-14 The CJ Philip Show.mp3`) and rebuild; see the README in that folder. Everything below adds the optional audio-only server (AzuraCast), which also gives background listening and automatic recordings.

This is the one-time setup that turns CJ Radio from "off air, server not set up" into a working station. Everything here is free. The only thing you need besides your PC is a debit or credit card for Oracle's identity check.

**How it fits together**

```
OBS + SE.Live ──video──▶ Twitch / YouTube          (as you do now)
      │
      └─audio─▶ AzuraCast on your free Oracle server ──▶ CJ Radio app + website
                       │                                   (live status, listeners,
                       └── records each live show            recordings)

Callers ──VDO.Ninja──▶ OBS browser source  (you approve, mute or remove them)
```

CJ Radio only shows **Live** when AzuraCast says your audio feed is connected *and* the station is online. Streaming to Twitch on its own doesn't make CJ Radio live.

---

## 1. Get the free server (about 30 minutes)

1. Sign up at Oracle Cloud's Free Tier page. Choose **UK South (London)** as your home region. You can't change it later. The card is used to check your identity; Oracle says Always Free resources aren't charged.
2. Create a compute instance:
   - Image: **Ubuntu 22.04**
   - Shape: **Ampere (VM.Standard.A1.Flex)** with **2 OCPUs and 12 GB memory** (the 2026 Always Free limit)
   - Boot volume: 100 GB (the free allowance is 200 GB in total)
   - Save the SSH key it offers you.
   If you see "out of host capacity", try another availability domain or try again later. This is common.
3. In the instance's subnet **security list**, allow incoming TCP **80**, **443**, and **8000–8010** (listener streams and the DJ connection).
4. Give the server a name: either a domain you own (point an `A` record at the server's public IP, for example in Cloudflare's free DNS) or a free DuckDNS subdomain.

> Oracle may reclaim Always Free servers that sit nearly idle for a week. Keeping AzuraCast running (step 3) helps, but keep backups of your station settings and recordings somewhere else.

## 2. Install AzuraCast (about 20 minutes)

Connect to the server with SSH, then run AzuraCast's Docker installer:

```
mkdir -p /var/azuracast && cd /var/azuracast
curl -fsSL https://raw.githubusercontent.com/AzuraCast/AzuraCast/main/docker.sh > docker.sh
chmod a+x docker.sh
sudo ./docker.sh install
```

Ubuntu images on Oracle also have a host firewall. If the web page doesn't load, open the same ports there too:

```
sudo iptables -I INPUT -p tcp -m multiport --dports 80,443,8000:8010 -j ACCEPT
sudo netfilter-persistent save
```

Open `http://<your server>` in a browser, create the admin account, and turn on HTTPS (Let's Encrypt) in the system settings using your domain.

## 3. Create the CJ Radio station

In AzuraCast:

1. **Add station**: name "CJ Radio". Note its short name (for example `cj_radio`).
2. **Mount points**: keep one MP3 mount at 128 kbps, named `/radio.mp3`. CJ Radio plays MP3.
3. **Streamers/DJs**: switch on "Allow Streamers / DJs", then add a DJ account called "CJ Philip". Note the DJ username, password and the **DJ port** shown on that page (often 8005).
4. Switch on **Record live broadcasts** (MP3).
5. Optional: give AutoDJ a short holding loop (your own jingle, or silence). Don't use commercial music: on your own stream you're the broadcaster, and UK music licences (PRS/PPL) aren't free. CJ Radio still shows Off Air while AutoDJ plays.

## 4. Point the apps at the station

Edit `shared/metadata/radio/station.json` (or ask me to) and rebuild:

```json
{
  "nowPlayingUrl": "https://<your server>/api/nowplaying/<short name>",
  "streamUrl": "https://<your server>/listen/<short name>/radio.mp3",
  "twitchUrl": "https://www.twitch.tv/<your channel>",
  "youtubeUrl": "https://www.youtube.com/@<your channel>/live"
}
```

Open CJ Radio's **Presenter** page (step 6 switches it on). "Server reachable" and "Station online" should both turn green.

## 5. Send your OBS audio to CJ Radio

Keep SE.Live streaming to Twitch/YouTube as normal. Add a second output for CJ Radio:

**Option A: OBS on its own (try first)**

1. OBS → Settings → Output → Output Mode: **Advanced** → **Recording** tab.
2. Type: **Custom Output (FFmpeg)**. FFmpeg Output Type: **Output to URL**.
3. File path or URL: `icecast://<dj username>:<dj password>@<your server>:<DJ port>/`
   (URL-encode any special characters in the password).
4. Container format: **mp3**. Audio encoder: **libmp3lame**, bitrate 128 kbps, sample rate 44.1 kHz.
5. Start Recording. On CJ Radio's Presenter page, "Your audio feed" should turn green within a few seconds.

If AzuraCast never sees the feed, the commonly reported workaround is container **ogg**, muxer settings `content_type=video/ogg`, and the libtheora video encoder. This setup uses OBS's Recording button, so OBS can't record locally at the same time. AzuraCast records the show instead.

**Option B: BUTT (most reliable fallback)**

Install BUTT and a free virtual audio cable. Set OBS's monitoring device to the cable, choose the cable as BUTT's input, and enter the DJ account details in BUTT's server settings. BUTT reconnects by itself if the connection drops.

**Rehearse for 30 minutes before your first real show**, using an unlisted YouTube target, so nothing goes out publicly.

## 6. Call-ins on Discord

Callers join a **Stage channel** on your Discord server. Discord's Stage has a built-in queue: listeners press **Request to Speak**, and you choose who comes up.

1. In your Discord server, create a Stage channel (for example "CJ Radio call-ins"). Under its permissions, make sure ordinary members can't speak without being invited.
2. Create an invite link to the server that doesn't expire, and put it in `station.json`:
   ```json
   "callIn": { "enabled": false, "guestUrl": "https://discord.gg/<your invite>", "minimumAge": 16 }
   ```
3. In OBS, add an **Application Audio Capture** source for Discord, so callers are heard on Twitch, YouTube and CJ Radio. Wear headphones, so callers don't hear themselves back.
4. For a show that takes calls, set `"enabled": true`. Listeners see **Call in on Discord** only while you're live, after confirming they're 16 or over and agree to be broadcast and recorded.
5. Take someone off air with **Move to Audience**, or mute them. Rehearse with two friends first.

Discord's own minimum age is 13; CJ Radio's call-in age (16 by default) is stricter, and it's your choice. Presenter mode (`presenter.json` with `"enabled": true`) adds the Presenter page with these steps and a button that opens your Discord.

## Listening options in the app

The Listen page has **Video and sound** or **Sound only**. Sound only uses Twitch's own *Audio Only* quality: the player stays on screen (smaller), but only audio is streamed, which saves data. Some streams don't offer Audio Only; then the app says so and keeps playing the video.

## 7. After each show

1. Download the recording from AzuraCast (Streamers/DJs → Broadcasts).
2. Check it, especially any caller segments. Remove anything a caller asks you to.
3. Upload it to a free Cloudflare R2 bucket (10 GB free, no download charges) and add an entry to the recordings index:

```json
{"recordings": [
  {"id": "2026-11-14", "title": "The CJ Philip Show", "recordedUtc": "2026-11-14T19:00:00Z",
   "durationMinutes": 120, "presenter": "CJ Philip", "url": "https://<your bucket>/2026-11-14.mp3",
   "description": "Listener questions and Alvora news."}
]}
```

Set `recordingsUrl` in `station.json` to that index's address. This step can be automated later.

## Going live, every Saturday

1. Start your stream with SE.Live as usual.
2. Start the CJ Radio feed (OBS Start Recording, or BUTT).
3. Wait for **LIVE ON CJ RADIO** on the Presenter page.
4. To finish, stop the CJ Radio feed first, then end the stream. CJ Radio switches itself to Off Air and stops listeners' players with a "broadcast has ended" message.

---

Not yet verified on the real server: AzuraCast's exact API and listen paths for your station, the DJ port number, and which OBS audio-only settings your OBS version accepts. Check them in step 4's Presenter page and the 30-minute rehearsal.

## The schedule: Google Calendar

CJ Radio's schedule comes from a Google Calendar (set in `shared/metadata/radio/station.json`, under `calendar`). Add, move or cancel shows there, and CJ Radio picks the change up within 15 minutes (or straight away with F5). The website's CJ Radio page links listeners to the same calendar.

**One-time setup:** in Google Calendar, open the calendar's **Settings and sharing**, then **Access permissions for events**. Tick **Make available to public** and choose **See all event details**. Until then the feed is private and CJ Radio says "The calendar isn't public yet". Only put shows in this calendar: everything in it is public.

**How events become programmes:**
- **Title** is the programme name. A title containing "The CJ Philip Show" is listed as CJ Philip's show (presenter CJ Philip, Talk).
- **Description** is shown under the title, as written. To name a guest, add a line such as `Guest: Name`. CJ Radio is CJ Philip's own station, so every programme is listed as his.
- **Repeating events** work, so you can make the Saturday show weekly. Deleting one week, or moving one week, is reflected.
- **Cancelling a show:** delete that event, or that week of the repeating event, to remove it from the schedule.
- **Not shown:** all-day events (they have no start time), and events marked Private.
- **Times** are read in UK time and shown to each listener in their own time zone.
- Being in the calendar never makes CJ Radio show ON AIR. Only the Twitch player saying the channel is live does that.

CJ Radio keeps the last copy it downloaded, so the schedule still shows offline (with the time it was saved).

## Recordings: the GitHub repository

CJ Radio lists recordings from `calebjphilip08-sudo/The-CJ-Philip-Show` (set in `station.json` as `recordingsGithub`). The repository must stay public.

- **Short files (under 100 MB):** add MP3s at the top of the repository or in a `Recordings` folder. Name them like `2026-11-14 The CJ Philip Show.mp3`: the date and title come from the name.
- **Full shows (any size up to 2 GB):** on GitHub choose **Releases › Draft a new release**, give it a tag (`ep1`) and a name (*Episode 1: Pilot*), write the notes, attach the MP3 and publish. The release name, notes and date become the recording's title, description and date.
- **Extra details (optional):** a `recordings.json` at the top of the repository: `{"recordings": [{"file": "2026-11-14 The CJ Philip Show.mp3", "title": "...", "description": "...", "presenter": "CJ Philip", "durationMinutes": 120}]}`.
- MP3s in CJ Radio's own `Recordings` folder on a PC are listed too. The last list from GitHub is saved, so recordings still show offline.
