"use client";

import { useState } from "react";
import { ChevronDown, File, Folder, Link2, Package } from "lucide-react";

import { LinuxIcon, WindowsIcon } from "@/components/os-icons";

type Row = {
  name: string;
  note: string;
  size: string;
  kind?: "folder" | "file" | "link";
  tag?: string;
};

type Group = {
  title: string;
  path: string;
  size: string;
  rows: Row[];
};

// A mock of Help > QODE's Files. The paths and file names are QODE's real ones; the sizes are examples.
const data: Record<"linux" | "windows", { groups: Group[]; outside: Row[]; installed: Row[]; total: string }> = {
  linux: {
    groups: [
      {
        title: "Settings",
        path: "~/.config/QODE-AGENTIC",
        size: "212 KB",
        rows: [
          { name: "QODE.conf", note: "Settings, window layout, recent projects", size: "38 KB", kind: "file" },
          { name: "claude.key · gemini.key", note: "Your API keys, mode 0600", size: "1 KB", kind: "file" },
          { name: "projects/", note: "Per-project state: open tabs, terminals, run configs", size: "164 KB" },
          { name: "mcp.json", note: "MCP servers for the chat", size: "2 KB", kind: "file" },
        ],
      },
      {
        title: "Downloads",
        path: "~/.local/share/QODE-AGENTIC",
        size: "641 MB",
        rows: [
          { name: "lsp/kotlin/", note: "Kotlin language server", size: "371 MB", tag: "Kotlin" },
          { name: "lsp/jdtls/", note: "Java language server (Eclipse JDT LS)", size: "58 MB", tag: "Java" },
          { name: "lsp/python/", note: "basedpyright and ruff, in their own venv", size: "94 MB", tag: "Python" },
          { name: "lsp/web/", note: "TypeScript, JavaScript, HTML, CSS, JSON", size: "106 MB", tag: "JS · TS" },
          { name: "history/", note: "Local history: versions saved on every save", size: "12 MB" },
        ],
      },
      {
        title: "Caches",
        path: "~/.cache/QODE-AGENTIC",
        size: "47 MB",
        rows: [
          { name: "lsp/java/", note: "Java server workspace and index", size: "31 MB" },
          { name: "lsp/library-sources/", note: "Library sources opened with Go to Definition", size: "9 MB" },
          { name: "code-index/", note: "Codebase index for the chat's search", size: "7 MB" },
        ],
      },
    ],
    outside: [
      { name: "~/.local/bin/kotlin-lsp", note: "Link to the Kotlin server QODE downloaded", size: "—", kind: "link" },
      { name: "~/.jdks/temurin-21", note: "JDK you asked QODE to download", size: "318 MB" },
    ],
    installed: [
      { name: "/opt/qode", note: "Program with its own Qt", size: "—", kind: "folder" },
      { name: "/usr/bin/qode", note: "Command link", size: "—", kind: "link" },
      { name: "/usr/share/applications/qode.desktop", note: "Menu entry, plus icons and man page", size: "—", kind: "file" },
    ],
    total: "1.0 GB",
  },
  windows: {
    groups: [
      {
        title: "Settings",
        path: "%APPDATA%\\QODE-AGENTIC",
        size: "212 KB",
        rows: [
          { name: "QODE.ini", note: "Settings, window layout, recent projects", size: "38 KB", kind: "file" },
          { name: "claude.key · gemini.key", note: "Your API keys, in your own profile", size: "1 KB", kind: "file" },
          { name: "projects\\", note: "Per-project state: open tabs, terminals, run configs", size: "164 KB" },
          { name: "mcp.json", note: "MCP servers for the chat", size: "2 KB", kind: "file" },
        ],
      },
      {
        title: "Downloads",
        path: "%LOCALAPPDATA%\\QODE-AGENTIC",
        size: "641 MB",
        rows: [
          { name: "lsp\\kotlin\\", note: "Kotlin language server", size: "371 MB", tag: "Kotlin" },
          { name: "lsp\\jdtls\\", note: "Java language server (Eclipse JDT LS)", size: "58 MB", tag: "Java" },
          { name: "lsp\\python\\", note: "basedpyright and ruff, in their own venv", size: "94 MB", tag: "Python" },
          { name: "lsp\\web\\", note: "TypeScript, JavaScript, HTML, CSS, JSON", size: "106 MB", tag: "JS · TS" },
          { name: "history\\", note: "Local history: versions saved on every save", size: "12 MB" },
        ],
      },
      {
        title: "Caches",
        path: "%LOCALAPPDATA%\\QODE-AGENTIC\\cache",
        size: "47 MB",
        rows: [
          { name: "lsp\\java\\", note: "Java server workspace and index", size: "31 MB" },
          { name: "lsp\\library-sources\\", note: "Library sources opened with Go to Definition", size: "9 MB" },
          { name: "code-index\\", note: "Codebase index for the chat's search", size: "7 MB" },
        ],
      },
    ],
    outside: [
      { name: "%USERPROFILE%\\.local\\bin\\kotlin-lsp.cmd", note: "Link to the Kotlin server QODE downloaded", size: "—", kind: "link" },
      { name: "%USERPROFILE%\\.jdks\\temurin-21", note: "JDK you asked QODE to download", size: "318 MB" },
    ],
    installed: [
      { name: "%LOCALAPPDATA%\\Programs\\QODE", note: "Program with its own Qt, no admin rights", size: "—", kind: "folder" },
      { name: "Start menu shortcut", note: "Plus the PATH entry and Explorer menu, if you ticked them", size: "—", kind: "link" },
      { name: "Uninstall entry", note: "The only registry key, written by the installer", size: "—", kind: "file" },
    ],
    total: "1.0 GB",
  },
};

export function FilesMock() {
  const [os, setOs] = useState<"linux" | "windows">("linux");
  const d = data[os];

  return (
    <div className="image-frame">
      <div className="overflow-hidden rounded-xl bg-[#0b0b11]">
        {/* Title bar */}
        <div className="flex items-center gap-3 border-b border-border px-4 py-3">
          <div className="flex gap-1.5" aria-hidden>
            <span className="size-2.5 rounded-full bg-white/15" />
            <span className="size-2.5 rounded-full bg-white/15" />
            <span className="size-2.5 rounded-full bg-white/15" />
          </div>
          <span className="text-xs font-semibold text-muted-foreground">QODE&apos;s Files</span>
          <div role="tablist" aria-label="Operating system" className="ml-auto flex rounded-full border border-border p-0.5 text-xs">
            {(["linux", "windows"] as const).map((k) => (
              <button
                key={k}
                role="tab"
                aria-selected={os === k}
                onClick={() => setOs(k)}
                className={`inline-flex items-center gap-1.5 rounded-full px-3 py-1 transition-colors ${
                  os === k ? "bg-foreground text-background" : "text-muted-foreground hover:text-foreground"
                }`}
              >
                {k === "linux" ? <LinuxIcon className="size-3" /> : <WindowsIcon className="size-3" />}
                {k === "linux" ? "Linux" : "Windows"}
              </button>
            ))}
          </div>
        </div>

        <div className="font-code p-3 text-[0.72rem] sm:p-4">
          <p className="px-2 pb-3 font-sans text-xs text-muted-foreground">
            Everything QODE has created on this computer. Your project folders are not on this list: QODE never writes into them.
          </p>

          {d.groups.map((g) => (
            <section key={g.title} className="mb-2">
              <div className="flex items-center gap-2 rounded-lg bg-white/[0.04] px-2 py-2">
                <ChevronDown className="size-3.5 text-muted-foreground" aria-hidden />
                <Folder className="size-3.5 text-primary" aria-hidden />
                <span className="font-sans font-semibold text-foreground">{g.title}</span>
                <span className="truncate text-muted-foreground">{g.path}</span>
                <span className="ml-auto shrink-0 text-foreground">{g.size}</span>
              </div>
              <ul>
                {g.rows.map((r) => (
                  <FileRow key={r.name} row={r} />
                ))}
              </ul>
            </section>
          ))}

          <SubHead title="Outside QODE's folders" note="listed so nothing is a surprise" />
          <ul className="mb-2">
            {d.outside.map((r) => (
              <FileRow key={r.name} row={r} />
            ))}
          </ul>

          <SubHead title="Installed by the package" note="removed by the uninstaller" icon />
          <ul>
            {d.installed.map((r) => (
              <FileRow key={r.name} row={r} />
            ))}
          </ul>
        </div>

        <div className="flex items-center gap-3 border-t border-border px-4 py-3 text-xs">
          <span className="text-muted-foreground">Example sizes</span>
          <span className="font-code ml-auto text-foreground">Total {d.total}</span>
        </div>
      </div>
    </div>
  );
}

function SubHead({ title, note, icon = false }: { title: string; note: string; icon?: boolean }) {
  return (
    <div className="mt-4 flex items-center gap-2 px-2 pb-1 font-sans text-[0.68rem] uppercase tracking-[0.18em] text-muted-foreground">
      {icon && <Package className="size-3.5" aria-hidden />}
      <span className="text-foreground/80">{title}</span>
      <span className="normal-case tracking-normal">· {note}</span>
    </div>
  );
}

function FileRow({ row }: { row: Row }) {
  const Icon = row.kind === "file" ? File : row.kind === "link" ? Link2 : Folder;
  return (
    <li className="group flex items-center gap-2 rounded-lg py-1.5 pl-8 pr-2 transition-colors hover:bg-white/[0.03]">
      <Icon className="size-3.5 shrink-0 text-muted-foreground" aria-hidden />
      <span className="shrink-0 text-foreground">{row.name}</span>
      {row.tag && (
        <span className="shrink-0 rounded border border-accent/30 bg-accent/10 px-1.5 text-[0.62rem] text-accent">{row.tag}</span>
      )}
      <span className="hidden min-w-0 truncate font-sans text-muted-foreground md:inline">{row.note}</span>
      <span className="ml-auto shrink-0 pl-3 text-muted-foreground">{row.size}</span>
    </li>
  );
}
