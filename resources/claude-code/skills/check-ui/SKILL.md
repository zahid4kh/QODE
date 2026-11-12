---
name: check-ui
description: Look at a web page or component in QODE's live web preview after changing it, on phone, tablet and desktop sizes, and check the browser console for errors. Use after any visual or layout change in a web app (HTML, CSS, Tailwind, React, Vue, Svelte, Angular...), when the user asks how something looks, whether it is responsive, or why a page is blank or broken.
---

# Check a UI change in QODE's preview

QODE shows the dev server's page in its web preview panel (a Chromium inside QODE) with device frames. Its tools let you look at that page, the same one the user sees.

## Steps

1. **State.** `mcp__qode__preview_status` says whether the preview is shown, which server it follows and that server's state, and the current url, device and console error count. If the server is not running, start it first (see the dev-server skill). If the preview is hidden, `mcp__qode__show_panel` with `web_preview` opens it (a screenshot also opens it).
2. **Go to the page you changed.** `mcp__qode__preview_navigate` with `path` (e.g. `/pricing`). Hot-reloading servers update the page after a save; a plain static server does not: pass `reload: true` to the screenshot.
3. **Look.** `mcp__qode__preview_screenshot` (`full_page: true` for the whole scroll height). Compare it with what was asked: spacing, alignment, overflow, text that is cut off, contrast, images that did not load.
4. **Other sizes**, when the change affects layout or the user asked about mobile: `mcp__qode__preview_set_viewport` with `device: "phone"` (`phone_os: "ios"` iPhone 15 393×852 or `"android"` Pixel 9 Pro 412×915), `"tablet"` (820×1180), `"desktop"` (1440 wide), screenshot each. Put the device back to what `preview_status` showed before when you are done: the user sees the switch.
5. **Console.** Every answer counts the console's errors. When there are any, or the page is blank or does not react, read `mcp__qode__preview_console` (it returns what earlier calls did not, unless `include_read`). Errors name the source file and line.
6. **Try it** when the change is interactive: `mcp__qode__preview_click` (CSS selector), `mcp__qode__preview_type` (fills a field, `submit: true` presses Enter), `mcp__qode__preview_eval` (JavaScript in the page: computed styles, element sizes, app state). The user approves each of these, so use them for a real check, not for every element.
7. **Fix and look again** until it matches. Then answer, saying which sizes you checked.

## Notes

- The preview's Chromium is the one in QODE's Qt. With an older QODE package (Qt 6.4) it is Chromium 102, so CSS newer than that (`:has()`, container queries, CSS nesting, `dvh`) does not apply there although a current browser shows it. If a style works in code but not in the preview, check this before rewriting it.
- The preview keeps its own cookies and local storage: a page behind a login needs the user to sign in inside the preview once.
- Do not open the page in another browser or install browser automation for this: the preview is already there.
