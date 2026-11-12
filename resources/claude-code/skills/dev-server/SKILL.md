---
name: dev-server
description: Work with the servers QODE runs for the project (web dev servers such as Vite or Next.js, an Expo app's web version, Spring Boot, Ktor, Python servers): find out whether one is running and at which url, start or restart it, and read its log when it fails or crashes. Use before running any dev server or `npm run dev` / `expo start` / `bootRun`, and when a server, an API call or a page load fails.
---

# Servers QODE runs

QODE starts and stops this project's servers from its toolbar, watches their output and shows the web one in its preview. Work with that server, never next to it.

## Before anything that needs a server

Call `mcp__qode__server_status`. Each server has an id, a state (`stopped`, `starting`, `running`, `failed`), and a port and url when it runs.

- **running**: use that url. Do not start another one: a second dev server takes another port, QODE's preview keeps showing the first, and a Bash command that never ends blocks you.
- **stopped**: `mcp__qode__server_start` (the user approves it). It uses the project's configured command, port and environment, saves unsaved edits, and waits until the server runs or fails.
- **starting**: wait with `mcp__qode__server_status` again; do not start it twice.
- **failed**: read why (below) before starting it again.

With more than one server, pass `server` with the id from `server_status`.

## When it fails or misbehaves

1. `mcp__qode__server_log` (`lines` for more): build errors, stack traces, the port it tried. A failed start already answers with the end of the log.
2. Fix the cause in the code or configuration. Typical causes: a compile error (`mcp__qode__get_problems` lists it too), a missing package (install it with Bash, the project's package manager), a port taken by a process outside QODE (tell the user which; do not kill processes you did not start), a missing environment variable (`.env` files hold secrets: ask the user rather than reading them).
3. `mcp__qode__server_restart` and check the state it returns.

Hot-reloading servers (Vite, Next.js, Expo, Spring DevTools, Ktor auto-reload) pick up saved edits by themselves: restart only for changes they do not reload (dependencies, configuration files, environment variables, a crashed process).

## Calling the server

Use the url from `server_status` with Bash (`curl -s <url>/api/...`) to check an endpoint. For a page, prefer QODE's preview (`mcp__qode__preview_screenshot`, see the check-ui skill): it shows what the user sees.

Stop a server (`mcp__qode__server_stop`) only when the user asks or it must stop for a change (some migrations, a port change).
