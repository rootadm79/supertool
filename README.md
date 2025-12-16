# Atari MiNT Web File Manager & Remote Terminal

This project is a **replacement for uIPtool**, which does **not work under FreeMiNT** on Atari systems.

It provides a lightweight **HTTP server running directly on Atari**, allowing you to **manage files and access a remote terminal from a PC web browser**.  
In addition, it can be used as a **REST API backend** for custom clients and automation.

Designed for **Atari Falcon / TT / ST** running **FreeMiNT**.

---

## 🎥 Demo Video

▶️ **YouTube demonstration:**  
https://youtu.be/srewT4SZ5l0

---

## ✨ Features

- 📁 **Web-based file management**
  - browse directories
  - upload files to Atari
  - download files from Atari
  - delete files

- 💻 **Remote terminal**
  - command execution via web interface
  - useful for headless or remote system management

- 🌐 **REST API**
  - all functionality accessible programmatically
  - suitable for custom clients, scripts, or automation
  - no dependency on a web browser

- 🚀 **Always on port 80**
  - no configuration required
  - simply open the Atari IP address in a browser

---

## 🎯 Motivation

`uIPtool` is a historically useful tool, but:
- it does not run under **MiNT**
- it is difficult to integrate into modern workflows

This project aims to:
- work reliably under **FreeMiNT**
- provide a **simple and modern HTTP-based interface**
- enable both **interactive use** and **API-driven control**
- keep the implementation **lightweight and Atari-friendly**

---

## 🖥️ Requirements

### Atari
- Atari Falcon / TT / ST
- **FreeMiNT**
- Working TCP/IP stack (e.g. MiNTnet)
- Network connectivity (Ethernet, NetUSBee, etc.)

### Client
- Any modern web browser  
  **or**
- Custom REST client

---

## ⚙️ Building

```sh
make
