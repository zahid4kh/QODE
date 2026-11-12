---
name: expo-web
description: Work on an Expo / React Native app with QODE, which runs the app's web version (react-native-web) in a phone frame in its preview. Use when the project depends on expo, when checking an Expo screen visually, when the Expo preview is blank or shows an error, or before running `npx expo start`.
---

# Expo apps in QODE

QODE finds Expo apps (a package.json that depends on `expo`, also in monorepo folders such as `apps/mobile`) and can show each one in its preview: QODE runs `npx expo start` itself, headless on a port from 19006 (a Metro the user started in the terminal keeps 8081), and loads the app's **web version** in an iPhone 15 or Pixel 9 Pro frame with the phone's safe-area insets.

## Rules

- **Do not run `npx expo start` with Bash.** The app's server is one of QODE's servers: `mcp__qode__server_status` lists it (its id starts with `expo:`), `mcp__qode__server_start` / `server_restart` run it, `mcp__qode__server_log` has Metro's output. Bundling errors appear there and in the preview's console, not in the page (a failed bundle is a blank page).
- **The preview is react-native-web, not the native app.** It is right for layout, styles, navigation and app logic. Native-only modules (camera, sensors, in-app purchases, some storage and notification libraries) do not exist there and may throw at start-up; that is not a bug in the app. Do not rewrite native code to make the web version work unless the user wants web support. Guard with `Platform.OS === 'web'` only where the user agrees.
- **Phone only.** The Expo target shows a phone: `mcp__qode__preview_set_viewport` with `device: "phone"` and `phone_os: "ios"` or `"android"` switches the frame (safe areas follow). Tablet and desktop sizes are for web apps.
- An app without `react-native-web` and `react-dom` has no web version: the preview offers Add Web Support, which changes package.json. Ask the user before adding it yourself (`npx expo install react-native-web react-dom @expo/metro-runtime`).

## Checking a screen

1. `mcp__qode__preview_status`: which target it shows (`expo:<folder>` or the web server) and whether its server runs.
2. Start the app's server if needed (above), open the preview with `mcp__qode__show_panel` `web_preview`.
3. With expo-router, `mcp__qode__preview_navigate` takes the route path (`/settings`, `/(tabs)` groups do not appear in the url). A screen behind a login redirects until the user signs in inside the preview.
4. `mcp__qode__preview_screenshot`, then `mcp__qode__preview_console` when the screen is blank or shows Expo's red error overlay.
5. Metro reloads saved edits by itself; restart the server only after installing packages or changing app.json / babel / metro configuration.

For native builds (`npx expo run:android`, prebuild), QODE's Expo menu runs them in its terminal; suggest that to the user rather than running long native builds with Bash.
