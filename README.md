<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=gradient&customColorList=6,11,19,26&height=220&section=header&text=AdvaitMusic&fontSize=60&fontColor=ffffff&animation=fadeIn&fontAlignY=38&desc=Next-Generation%20Telegram%20Voice%20Chat%20Music%20Engine&descAlignY=60&descAlign=50" width="100%"/>
</p>

<p align="center">
  <img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=600&size=20&pause=1000&color=00E5FF&center=true&vCenter=true&width=620&lines=High-Performance+C%2B%2B23+Streaming+Engine;Native+TDLib+Telegram+Client;Ultra-Low+Latency+Voice+Chat+Audio;Live+Message+Editing+%2B+Download+CANCEL;Multi-Language+YAML+Locales;Developed+%26+Maintained+by+Iota+coder" alt="Typing SVG" />
</p>

<p align="center">
  <a href="https://github.com/Iotatg/AdvaitMusic/stargazers">
    <img src="https://img.shields.io/badge/GitHub-Stars-FFD700?style=for-the-badge&logo=github&logoColor=white" alt="GitHub Stars">
  </a>
  <a href="https://github.com/Iotatg/AdvaitMusic/fork">
    <img src="https://img.shields.io/badge/GitHub-Forks-00C853?style=for-the-badge&logo=github&logoColor=white" alt="GitHub Forks">
  </a>
  <a href="https://isocpp.org/">
    <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++ Standard">
  </a>
  <a href="https://github.com/tdlib/td">
    <img src="https://img.shields.io/badge/TDLib-Native-2CA5E0?style=for-the-badge&logo=telegram&logoColor=white" alt="TDLib">
  </a>
  <a href="https://github.com/Iotatg/AdvaitMusic/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-blue?style=for-the-badge&logo=opensourceinitiative&logoColor=white" alt="License">
  </a>
  <a href="https://t.me/im_iota">
    <img src="https://img.shields.io/badge/Telegram-Support-26A69A?style=for-the-badge&logo=telegram&logoColor=white" alt="Support Chat">
  </a>
</p>

---

## ✨ Features & Highlights

- ⚡ **Ultra-Low Latency Streaming:** Pure C++23 core with native `ntgcalls` integration, delivering crystal-clear voice-chat playback with zero runtime overhead.
- 🧵 **Single Static Binary:** Entire engine compiles into one `advait_bot` executable — no interpreter, no garbage collector, no stutters.
- 📡 **Native TDLib Client:** Real Telegram client protocol via `libtdjson` with a custom server-ID remap engine, so every status message is **edited in place** through the full search → download → progress → now-playing chain.
- 🎵 **Multi-Platform Provider:** Stream resolution for YouTube, Spotify metadata matching, Telegram audio files, live streams, and direct HTTP audio URLs.
- 🛑 **Real Download Cancellation:** Live progress editing plus a working `/cancel` that aborts the transfer mid-stream through the download backend.
- 🎛️ **Full Playback Controls:** Complete command suite — seek, volume, mute, loop, shuffle, autoplay, force-play variants, and queue management.
- 🌐 **Multi-Language Engine:** YAML localization packs (English, Hindi, Arabic, Turkish) hot-reloadable via `/reload`.
- 🛡️ **Admin & Security:** Granular sudo management, per-chat user authorization, anti-flood protection, and owner-only maintenance flows.

---

## 🚀 Quick Deploy

<details>
  <summary><b>🖥️ [Click to Expand] VPS / Linux Server Build Guide</b></summary>
  <br>

  ### 📋 Prerequisites
  ```bash
  sudo apt-get update && sudo apt-get upgrade -y
  sudo apt-get install -y git cmake ninja-build g++ libcurl4-openssl-dev libsqlite3-dev ffmpeg
  ```

  ### 🚀 Step-by-Step Setup
  ```bash
  # 1. Clone the repository
  git clone https://github.com/Iotatg/AdvaitMusic.git
  cd AdvaitMusic

  # 2. Place runtime libraries in ./lib/
  #    libtdjson.so   (TDLib 1.8)
  #    libntgcalls.a  (voice-chat engine)

  # 3. Configure environment
  nano .env  # API_ID, API_HASH, TOKEN, OWNER_ID, etc.

  # 4. Compile and run
  cmake -S . -B build -G Ninja
  cmake --build build -j$(nproc)
  ./build/advait_bot
  ```

  ### 🔄 Running 24/7 in Background
  **Method 1: Using Tmux (Recommended)**
  ```bash
  tmux new -s advaitmusic
  ./build/advait_bot
  # Press Ctrl+B then D to detach. Re-attach anytime with: tmux a -t advaitmusic
  ```

  **Method 2: Using Systemd Service**
  ```bash
  sudo nano /etc/systemd/system/advaitmusic.service
  ```
  Paste the following unit configuration:
  ```ini
  [Unit]
  Description=AdvaitMusic C++ Telegram Bot
  After=network.target

  [Service]
  Type=simple
  User=ubuntu
  WorkingDirectory=/home/ubuntu/AdvaitMusic
  ExecStart=/home/ubuntu/AdvaitMusic/build/advait_bot
  Restart=always
  RestartSec=5

  [Install]
  WantedBy=multi-user.target
  ```
  Enable and start the service:
  ```bash
  sudo systemctl daemon-reload
  sudo systemctl enable advaitmusic
  sudo systemctl start advaitmusic
  sudo systemctl status advaitmusic
  ```
</details>

---

## ⚙️ Configuration Variables

<details open>
  <summary><b>🔑 Environment Variables Reference Table</b></summary>
  <br>

  | Variable | Required | Default | Description |
  | :--- | :---: | :---: | :--- |
  | `API_ID` | **Yes** | — | Telegram API ID from [my.telegram.org](https://my.telegram.org) |
  | `API_HASH` | **Yes** | — | Telegram API Hash from [my.telegram.org](https://my.telegram.org) |
  | `TOKEN` | **Yes** | — | Telegram Bot Token from [@BotFather](https://t.me/BotFather) |
  | `OWNER_ID` | **Yes** | — | Telegram User ID of the bot owner |
  | `IOTA_API_URL` | **Yes** | — | Download backend API endpoint |
  | `IOTA_API_KEY` | No | — | Download backend API key |
  | `DURATION_LIMIT` | No | `4200` | Max track duration in seconds (70 minutes) |
  | `QUEUE_LIMIT` | No | `24` | Max tracks allowed in queue per chat |
  | `DEFAULT_LANG` | No | `en` | Default language code (`en`, `hi`, `ar`, `tr`) |
  | `LOGGER_ID` | No | `0` | Telegram Chat ID for logs and error reports |
  | `SUPPORT_CHAT` | No | `https://t.me/+Q493A_VBAUpkMmRl` | Link to your Telegram support group |
  | `SUPPORT_CHANNEL` | No | `https://t.me/im_iota` | Link to your Telegram announcement channel |
  | `DEV_URL` | No | — | Developer profile link shown on repo cards |
  | `COOKIES_LINK` | No | — | Remote URL to YouTube cookies file |
  | `START_IMG_URL` | No | — | Media shown on `/start` greeting |
</details>

---

## 🎮 Bot Commands Reference

<details>
  <summary><b>🎵 [Click to Expand] Playback & Stream Commands</b></summary>
  <br>

  | Command | Description |
  | :--- | :--- |
  | `/play <query/url>` | Streams requested song into group voice chat |
  | `/vplay <query/url>` | Streams track with video rendering enabled |
  | `/fplay <query/url>` | Force plays track as owner/sudo privileges |
  | `/skip` | Skips current track to the next item in queue |
  | `/pause` | Pauses active playback stream |
  | `/resume` | Resumes paused audio stream |
  | `/stop` | Stops playback and clears the entire playlist queue |
  | `/seek <seconds>` | Seeks playback to a specific timestamp |
  | `/volume <1-200>` | Adjusts voice-chat output volume |
  | `/mute` / `/unmute` | Mutes or unmutes the stream audio |
  | `/queue` | Displays the current playlist and upcoming tracks |
  | `/replay` | Replays the current playing song from beginning |
  | `/cancel` | Aborts the active track download mid-transfer |
</details>

<details>
  <summary><b>👥 [Click to Expand] Group Admin & Settings Commands</b></summary>
  <br>

  | Command | Description |
  | :--- | :--- |
  | `/auth <user>` | Authorizes non-admin user to use playback commands |
  | `/unauth <user>` | Revokes bot access authorization from a user |
  | `/autoplay` | Toggles continuous auto-play when queue empties |
  | `/lang` | Opens the interactive language selector panel |
  | `/settings` | Opens the interactive visual settings control panel |
  | `/reload` | Refreshes admin caches and locale packs live |
  | `/askip` | Personal skip that only advances your own request |
</details>

<details>
  <summary><b>🛡️ [Click to Expand] Bot Owner & Sudo Commands</b></summary>
  <br>

  | Command | Description |
  | :--- | :--- |
  | `/addsudo <user>` | Adds a new user to sudoers list |
  | `/delsudo <user>` | Removes a user from sudoers list |
  | `/sudolist` | Shows list of all current bot sudoers |
  | `/stats` | Shows server performance, memory, and uptime metrics |
  | `/broadcast <msg>` | Broadcasts announcements to all served chats |
  | `/purge <n>` | Deletes last n messages from the current chat |
  | `/ping` | Live latency and response-time benchmark |
</details>

---

## 📁 Project Architecture

```
advaitmusic/
├── include/advait/
│   ├── core/             # Config, buttons, dispatcher & engine state
│   ├── database/         # Persistence layer (playlists, auth, sudo, settings)
│   ├── modules/          # Command modules (play, skip, settings, admin, etc.)
│   ├── platforms/        # Media resolvers & download backends (YouTube, Spotify)
│   └── telegram/         # TDLib client, ID-remap engine & live message editor
├── src/                  # Implementation mirroring header layout
├── locales/              # YAML multi-language packs (en / hi / ar / tr)
├── docs/                 # Engineering notes (start with EDIT_PLAY_AUDIT.md)
├── lib/                  # Runtime libraries (libtdjson.so, libntgcalls.a)
└── CMakeLists.txt        # Modern CMake build definition
```

---

## 📜 License

Distributed under the **MIT License**. See the [`LICENSE`](./LICENSE) file for full details.

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=gradient&customColorList=6,11,19,26&height=100&section=footer" width="100%"/>
</p>

<p align="center">
  <b>Developed & Maintained with ❤️ by <a href="https://github.com/Iotatg">Iota coder</a></b>
</p>
