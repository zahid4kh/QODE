// Loaded by QODE (NODE_OPTIONS --require) into the `expo start` it runs for the web preview, never into the user's own
// commands; Metro's transform workers inherit it. Nothing in the project is touched:
// - An app with web.output "static" has Metro render every route in Node on each page load, where code that needs
//   `window` or a native module crashes it; the preview only needs the app in the browser, so "static" is served as a
//   single-page app ("single") instead. "server" (API routes) is left alone.
// - Packages whose browser build reads `import.meta` (zustand 5's middleware, …) stop the web bundle with "Cannot use
//   'import.meta' outside a module"; on a device Metro picks their React Native build, which does not. babel-preset-expo
//   can rewrite it (unstable_transformImportMeta), so that is the default here unless babel.config.js says otherwise.
//   The preview gets its own Metro cache key, so files transformed without it (by an earlier run) are not reused.
// - expo-sqlite's web build loads a .wasm file and needs a cross-origin isolated page (SharedArrayBuffer); Expo's docs
//   have the app add both to metro.config.js. When the app uses expo-sqlite, the preview's Metro serves .wasm as an
//   asset and sends the two headers itself.
'use strict';
const Module = require('module');
const load = Module._load;
const wrapped = new WeakMap();

function single(config) {
  const web = config && config.exp && config.exp.web;
  if (web && web.output === 'static') config.exp.web = { ...web, output: 'single' };
  return config;
}

// The CommonJS builds export getters that cannot be reassigned: hand out a proxy instead.
function override(m, key, fn) {
  if (!wrapped.has(m)) {
    const replacement = (...args) => fn(m[key](...args));
    wrapped.set(m, new Proxy(m, { get: (target, k) => (k === key ? replacement : Reflect.get(target, k)) }));
  }
  return wrapped.get(m);
}

function uses(pkg) {
  try {
    require.resolve(`${pkg}/package.json`, { paths: [process.cwd()] });
    return true;
  } catch {
    return false;
  }
}
const sqlite = uses('expo-sqlite');

function metroConfig(config) {
  config = { ...config, cacheVersion: `${config.cacheVersion || ''}qode-web-preview` };
  if (sqlite && config.resolver && !config.resolver.assetExts.includes('wasm'))
    config.resolver = { ...config.resolver, assetExts: [...config.resolver.assetExts, 'wasm'] };
  return config;
}

if (sqlite) {
  const http = require('http');
  const writeHead = http.ServerResponse.prototype.writeHead;
  http.ServerResponse.prototype.writeHead = function (...args) {
    if (!this.headersSent && !this.hasHeader('Cross-Origin-Opener-Policy')) {
      this.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
      this.setHeader('Cross-Origin-Embedder-Policy', 'credentialless');
    }
    return writeHead.apply(this, args);
  };
}

function preset(original) {
  if (!wrapped.has(original)) {
    const withImportMeta = function (api, options) {
      return original.call(this, api, { unstable_transformImportMeta: true, ...(options || {}) });
    };
    Object.assign(withImportMeta, original, { default: withImportMeta });
    wrapped.set(original, withImportMeta);
  }
  return wrapped.get(original);
}

Module._load = function (request) {
  const m = load.apply(this, arguments);
  if (!m)
    return m;
  if (request === '@expo/config' && typeof m.getConfig === 'function')
    return override(m, 'getConfig', single);
  if (request === '@expo/metro-config' && typeof m.getDefaultConfig === 'function')
    return override(m, 'getDefaultConfig', metroConfig);
  if (/(^|[\\/])babel-preset-expo([\\/]build[\\/]index\.js)?$/.test(request) && typeof m === 'function')
    return preset(m);
  return m;
};
