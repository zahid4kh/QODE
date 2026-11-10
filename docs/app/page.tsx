import Image from "next/image";
import {
  ArrowDown,
  ArrowUpRight,
  Braces,
  Bug,
  Columns2,
  Eye,
  FolderX,
  Trash2,
  Boxes,
  Cpu,
  FolderLock,
  Globe,
  History,
  MessageSquareCode,
  PackageOpen,
  Send,
  ShieldCheck,
  SquareTerminal,
  Terminal,
  Zap,
} from "lucide-react";

import { FilesMock } from "@/components/files-mock";
import { LinuxIcon, WindowsIcon } from "@/components/os-icons";

// bump-version.sh rewrites every "2.0.0" in this file.
const VERSION = "2.0.0";
const RELEASE = `https://github.com/zahid4kh/QODE/releases/download/${VERSION}`;
const RELEASES_PAGE = "https://github.com/zahid4kh/QODE/releases";
const REPO = "https://github.com/zahid4kh/QODE";

const downloads = {
  windows: {
    full: `${RELEASE}/QODE-${VERSION}-setup.exe`,
    noweb: `${RELEASE}/QODE-${VERSION}-noweb-setup.exe`,
    fullZip: `${RELEASE}/QODE-${VERSION}-portable.zip`,
    nowebZip: `${RELEASE}/QODE-${VERSION}-noweb-portable.zip`,
  },
  linux: {
    full: `${RELEASE}/qode-bundled_${VERSION}_amd64.deb`,
    noweb: `${RELEASE}/qode-bundled-noweb_${VERSION}_amd64.deb`,
  },
};

const features = [
  {
    icon: MessageSquareCode,
    title: "A coding chat that does the work",
    body: "The QODE chat runs on Gemini or Claude with your own key. It reads the project, edits files, runs commands, checks diagnostics after each change, and every edit can be undone from the chat.",
    span: "lg:col-span-2",
  },
  {
    icon: SquareTerminal,
    title: "Claude Code, with buttons",
    body: "A graphical front end for the claude CLI: sign-in, permission prompts as cards, history, /usage and /mcp.",
    span: "",
  },
  {
    icon: Terminal,
    title: "A real terminal, built in",
    body: "Split panes and renamable tabs that come back with your project. Bash and zsh on Linux; PowerShell, cmd, Git Bash or WSL on Windows.",
    span: "",
  },
  {
    icon: Braces,
    title: "Language servers",
    body: "Diagnostics, completion, Go to Definition, call hierarchy and quick fixes. clangd, Kotlin, TypeScript, Python, or bring your own.",
    span: "",
  },
  {
    icon: Bug,
    title: "JVM, properly",
    body: "Gradle tasks, a Maven panel, Spring Boot, Ktor, and a debugger with breakpoints, steps and variables.",
    span: "",
  },
  {
    icon: Globe,
    title: "Web, Expo and Python",
    body: "Dev server strip, a live preview with phone and tablet frames, .env editor, venvs and packages.",
    span: "",
  },
  {
    icon: Send,
    title: "HTTP client",
    body: "Send requests from .http and .rest files, with variables, environments and chained responses.",
    span: "",
  },
  {
    icon: History,
    title: "Git and local history",
    body: "Diffs, inline blame and a Version Control panel, plus a saved version of every file on save that you can diff and restore.",
    span: "",
  },
];

const specs = [
  ["C++17 + Qt 6", "No web runtime"],
  ["0 Electron", "Not a single byte"],
  ["2 platforms", "Windows and Linux"],
  ["MIT", "Read it, change it"],
];

const transparency = [
  {
    icon: FolderX,
    title: "Your project stays yours",
    body: "No .qode folder, no hidden config, nothing added to your repo. Open tabs, terminals and run configurations are kept in QODE's own settings folder.",
  },
  {
    icon: Eye,
    title: "One window lists it all",
    body: "Help > QODE's Files shows every file and folder QODE has created, with sizes. That includes what lives outside its folders, like the Kotlin link and downloaded JDKs, and what the installer put on your system.",
  },
  {
    icon: Boxes,
    title: "Language servers in their own box",
    body: "Kotlin, Java, Python and the JS / TS servers are installed into QODE's folder: no global npm install, no pip into your system Python, no sudo. Removing one shows exactly what gets deleted.",
  },
  {
    icon: Trash2,
    title: "Leaves cleanly",
    body: "On Windows, QODE writes nothing to the registry, and the uninstaller lists QODE's data with sizes so you choose what goes. On Linux, removing the package deletes what QODE created and prints each item; switching between QODE packages keeps it.",
  },
];

const shortcuts = [
  ["Ctrl+Alt+G", "QODE chat"],
  ["Ctrl+K", "Inline edit"],
  ["Ctrl+Shift+P", "Command palette"],
  ["Ctrl+J", "Toggle terminal"],
  ["F12", "Go to definition"],
  ["F5", "Run file"],
];

const rise = (i: number) => ({ "--i": i }) as React.CSSProperties;

export default function Page() {
  return (
    <main
      id="top"
      className="relative min-h-screen overflow-x-clip bg-background text-foreground"
    >
      <div className="grain" aria-hidden />
      <a
        href="#why"
        className="sr-only focus:not-sr-only focus:fixed focus:left-4 focus:top-4 focus:z-[60] focus:rounded-full focus:bg-primary focus:px-4 focus:py-2 focus:text-primary-foreground"
      >
        Skip to content
      </a>

      <header className="relative z-20 mx-auto flex max-w-7xl items-center justify-between px-6 py-6 lg:px-10">
        <a
          href="#top"
          className="flex items-center gap-3 font-semibold tracking-tight"
          aria-label="QODE home"
        >
          <span className="size-9 overflow-hidden rounded-[10px] shadow-[0_0_30px_rgba(123,140,255,0.45)]">
            <img src="/icon-192.png" alt="" className="size-full object-cover" />
          </span>
          <span className="font-code text-sm tracking-[0.3em]">QODE</span>
        </a>
        <nav
          className="hidden items-center gap-9 text-sm text-muted-foreground md:flex"
          aria-label="Main navigation"
        >
          <a className="transition-colors hover:text-foreground" href="#why">
            Why
          </a>
          <a className="transition-colors hover:text-foreground" href="#features">
            Features
          </a>
          <a className="transition-colors hover:text-foreground" href="#platforms">
            Platforms
          </a>
          <a className="transition-colors hover:text-foreground" href="#transparency">
            Transparency
          </a>
          <a className="transition-colors hover:text-foreground" href="#download">
            Download
          </a>
        </nav>
        <div className="flex items-center gap-3">
          <a
            href={REPO}
            className="hidden items-center gap-1 text-sm text-muted-foreground transition-colors hover:text-foreground sm:inline-flex"
          >
            GitHub <ArrowUpRight className="size-3.5" />
          </a>
          <a
            href="#download"
            className="rounded-full border border-border bg-white/[0.04] px-4 py-2 text-xs font-semibold backdrop-blur transition-colors hover:border-primary/60"
          >
            Get QODE
          </a>
        </div>
      </header>

      {/* HERO */}
      <section className="relative">
        <div className="aurora" aria-hidden />
        <div className="relative z-10 mx-auto max-w-7xl px-6 pb-10 pt-16 lg:px-10 lg:pt-24">
          <div className="rise flex flex-wrap items-center gap-3" style={rise(0)}>
            <p className="eyebrow">Native code editor</p>
            <span className="inline-flex items-center gap-2 rounded-full border border-border bg-white/[0.04] px-3 py-1 text-xs text-foreground backdrop-blur">
              <WindowsIcon className="size-3.5 text-primary" /> Windows
              <span className="text-muted-foreground">·</span>
              <LinuxIcon className="size-3.5 text-primary" /> Linux
            </span>
          </div>
          <h1
            className="font-display rise mt-8 max-w-[14ch] text-balance text-[clamp(4rem,11vw,10.5rem)] leading-[0.86]"
            style={rise(1)}
          >
            Code without <em className="text-gradient pr-2">the baggage</em>
            <span className="caret" aria-hidden />
          </h1>
          <div className="rise mt-12 flex max-w-2xl flex-col gap-8" style={rise(2)}>
            <p className="text-lg leading-relaxed text-muted-foreground">
              QODE is a fast editor written in C++ and Qt, with a coding chat
              built in. It opens instantly, edits real files and carries its own
              terminal, git, language servers and debugger. The same app on
              Windows and Linux. No Electron, no account, no drama.
            </p>
            <div className="flex flex-wrap items-center gap-3">
              <OsButton
                href={downloads.windows.full}
                icon={<WindowsIcon className="size-4" />}
                label="Download for Windows"
                primary
              />
              <OsButton
                href={downloads.linux.full}
                icon={<LinuxIcon className="size-4" />}
                label="Download for Linux"
                primary
              />
            </div>
            <p className="font-code -mt-3 text-xs text-muted-foreground">
              v{VERSION} · Windows 10+ x64 installer · Debian / Ubuntu amd64 .deb ·{" "}
              <a href="#download" className="underline decoration-border underline-offset-4 hover:text-foreground">
                more options
              </a>
            </p>
          </div>
        </div>

        {/* Product shot: bleeds off the right edge, tilted into the dark */}
        <div className="rise relative z-10 mx-auto mt-6 max-w-7xl px-6 lg:px-10" style={rise(4)}>
          <div className="image-frame lg:ml-[18%] lg:w-[112%] lg:[transform:perspective(1800px)_rotateY(-7deg)_rotateX(2deg)]">
            <Image
              src="/screenshots/editor-dark.png"
              alt="QODE editing a Kotlin file with project explorer, breadcrumbs, minimap and inline git blame"
              width={1920}
              height={1048}
              priority
              unoptimized
              className="h-auto w-full rounded-xl"
            />
          </div>
          <div className="hairline mt-16" />
        </div>
      </section>

      {/* WHY */}
      <section
        id="why"
        className="relative z-10 mx-auto grid max-w-7xl gap-16 px-6 py-28 lg:grid-cols-[1fr_1fr] lg:px-10 lg:py-40"
      >
        <div>
          <h2 className="font-display max-w-[12ch] text-5xl leading-[0.95] sm:text-7xl">
            An editor should edit <em className="text-gradient">code.</em>
          </h2>
        </div>
        <div className="flex flex-col justify-end gap-10">
          <p className="max-w-md text-lg leading-relaxed text-muted-foreground">
            Everything you need sits in one window and nothing you don&apos;t.
            No accounts, no extension marketplace to babysit. The chat uses your
            own API key, kept in a file only you can read, and secret files are
            never sent. Your project folders stay clean: QODE writes nothing
            into them, and Help &gt; QODE&apos;s Files lists everything it has
            created.
          </p>
          <dl className="grid grid-cols-2 gap-px overflow-hidden rounded-2xl border border-border bg-border">
            {specs.map(([stat, label]) => (
              <div key={stat} className="bg-background p-6">
                <dt className="font-display text-3xl sm:text-4xl">{stat}</dt>
                <dd className="mt-2 text-sm text-muted-foreground">{label}</dd>
              </div>
            ))}
          </dl>
        </div>
      </section>

      {/* FEATURES */}
      <section
        id="features"
        className="relative z-10 mx-auto max-w-7xl px-6 pb-28 lg:px-10 lg:pb-40"
      >
        <div className="mb-14">
          <h2 className="font-display text-5xl leading-[0.95] sm:text-7xl">
            Small footprint.
            <br />
            <em className="text-muted-foreground">Serious tools.</em>
          </h2>
        </div>
        <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-3">
          {features.map(({ icon: Icon, title, body, span }) => (
            <article key={title} className={`panel p-8 ${span}`}>
              <span className="mb-10 inline-grid size-11 place-items-center rounded-xl border border-border bg-white/[0.03] text-primary">
                <Icon className="size-5" aria-hidden />
              </span>
              <h3 className="text-xl font-semibold tracking-tight">{title}</h3>
              <p className="mt-3 max-w-md text-sm leading-relaxed text-muted-foreground">
                {body}
              </p>
            </article>
          ))}
        </div>
      </section>

      {/* PLATFORMS */}
      <section
        id="platforms"
        className="relative z-10 mx-auto max-w-7xl px-6 pb-28 lg:px-10 lg:pb-40"
      >
        <div className="mb-14 max-w-3xl">
          <h2 className="font-display text-5xl leading-[0.95] sm:text-7xl">
            Native on Windows. <em className="text-gradient">Native on Linux.</em>
          </h2>
          <p className="mt-6 max-w-xl text-lg leading-relaxed text-muted-foreground">
            Not a port with rough edges: each OS gets its own terminal, process
            handling and installer, built from the same code.
          </p>
        </div>
        <div className="grid gap-4 lg:grid-cols-2">
          <PlatformCard
            icon={<WindowsIcon className="size-6" />}
            name="Windows"
            points={[
              "Per-user installer: no administrator rights, nothing in the registry",
              "Portable zip that keeps its settings next to qode.exe",
              "Terminal with PowerShell 7, Windows PowerShell, cmd, Git Bash or WSL",
              "Run commands rewritten for the shell you use",
            ]}
          />
          <PlatformCard
            icon={<LinuxIcon className="size-6" />}
            name="Linux"
            points={[
              "A .deb with its own Qt 6.11, so your system Qt does not matter",
              "Updates through apt from the QODE package repository",
              "Real pty terminal running your own shell, with each command's exit code marked",
              "Language servers found on PATH, or downloaded for you on request",
            ]}
          />
        </div>
      </section>

      {/* TRANSPARENCY */}
      <section
        id="transparency"
        className="relative z-10 mx-auto max-w-7xl px-6 pb-28 lg:px-10 lg:pb-40"
      >
        <div className="hairline mb-28 lg:mb-40" />
        <div className="grid items-start gap-14 lg:grid-cols-[0.85fr_1.15fr]">
          <div className="lg:sticky lg:top-10">
            <p className="eyebrow">Nothing hidden</p>
            <h2 className="font-display mt-6 text-5xl leading-[0.95] sm:text-7xl">
              Every file it writes, <em className="text-gradient">on the record.</em>
            </h2>
            <p className="mt-6 max-w-md text-lg leading-relaxed text-muted-foreground">
              Most editors scatter caches, servers and dot-folders across your
              disk and leave you to find them. QODE keeps a list, and shows it
              to you.
            </p>
            <ul className="mt-10 flex flex-col gap-7">
              {transparency.map(({ icon: Icon, title, body }) => (
                <li key={title} className="flex gap-4">
                  <span className="inline-grid size-10 shrink-0 place-items-center rounded-xl border border-border bg-white/[0.03] text-primary">
                    <Icon className="size-4.5" aria-hidden />
                  </span>
                  <div>
                    <h3 className="font-semibold tracking-tight">{title}</h3>
                    <p className="mt-1.5 text-sm leading-relaxed text-muted-foreground">{body}</p>
                  </div>
                </li>
              ))}
            </ul>
          </div>
          <div>
            <FilesMock />
            <p className="mt-4 text-center text-xs text-muted-foreground">
              Help &gt; QODE&apos;s Files: real paths, example sizes. Switch between Linux and Windows.
            </p>
          </div>
        </div>
      </section>

      {/* INSIDE */}
      <section
        id="inside"
        className="relative z-10 mx-auto grid max-w-7xl items-center gap-12 px-6 pb-28 lg:grid-cols-[0.7fr_1.3fr] lg:px-10 lg:pb-40"
      >
        <div>
          <p className="eyebrow">Start clean</p>
          <h2 className="font-display mt-6 text-4xl leading-[1] sm:text-6xl">
            A welcome screen that doesn&apos;t ask for your life story.
          </h2>
          <p className="mt-6 max-w-sm text-muted-foreground">
            Open a project, pick up where you left off, or start a new one from
            a template. Your tabs, splits and terminals come back exactly as
            you left them.
          </p>
        </div>
        <div className="image-frame">
          <Image
            src="/screenshots/home-dark.png"
            alt="QODE welcome screen with quick actions and recent projects"
            width={1920}
            height={1080}
            unoptimized
            className="h-auto w-full rounded-xl"
          />
        </div>
      </section>

      {/* SHORTCUTS */}
      <section aria-label="Keyboard shortcuts" className="relative z-10 border-y border-border">
        <ul className="mx-auto grid max-w-7xl grid-cols-2 gap-px px-6 py-px sm:grid-cols-3 lg:grid-cols-6 lg:px-10">
          {shortcuts.map(([keys, label]) => (
            <li key={keys} className="py-8 pr-4">
              <kbd className="font-code rounded-md border border-border bg-white/[0.04] px-2 py-1 text-xs text-accent">
                {keys}
              </kbd>
              <p className="mt-3 text-sm text-muted-foreground">{label}</p>
            </li>
          ))}
        </ul>
      </section>

      {/* DOWNLOAD */}
      <section id="download" className="relative z-10 px-6 py-28 lg:px-10 lg:py-40">
        <div className="relative mx-auto max-w-7xl overflow-hidden rounded-[2rem] border border-primary/30 px-7 py-20 sm:px-16 lg:py-24">
          <div className="aurora !inset-auto !-right-1/4 !top-0 !h-full !w-3/4 !opacity-50" aria-hidden />
          <div className="relative">
            <p className="eyebrow">Ready when you are · v{VERSION}</p>
            <h2 className="font-display mt-6 max-w-3xl text-6xl leading-[0.9] sm:text-8xl">
              Install the editor. <em className="text-gradient">Keep the RAM.</em>
            </h2>
            <p className="mt-8 max-w-2xl text-muted-foreground">
              Every download ships with its own Qt. <b className="font-semibold text-foreground">Full</b>{" "}
              includes the live web and Expo preview (Qt WebEngine).{" "}
              <b className="font-semibold text-foreground">Lite</b> leaves it out and is a much
              smaller download; pages open in your browser instead.
            </p>

            <div className="mt-12 grid gap-4 lg:grid-cols-2">
              {/* Windows */}
              <div className="rounded-3xl border border-border bg-black/50 p-7 backdrop-blur sm:p-9">
                <div className="flex items-center gap-3">
                  <WindowsIcon className="size-6 text-primary" />
                  <h3 className="text-2xl font-semibold tracking-tight">Windows</h3>
                  <span className="font-code ml-auto text-xs text-muted-foreground">10 (1809)+ · x64</span>
                </div>
                <div className="mt-8 flex flex-col gap-3">
                  <DownloadRow
                    href={downloads.windows.full}
                    title="Full"
                    detail="Installer · with web preview"
                    file={`QODE-${VERSION}-setup.exe`}
                    primary
                  />
                  <DownloadRow
                    href={downloads.windows.noweb}
                    title="Lite"
                    detail="Installer · no web preview"
                    file={`QODE-${VERSION}-noweb-setup.exe`}
                  />
                </div>
                <p className="mt-5 text-xs text-muted-foreground">
                  <PackageOpen className="mr-1.5 inline size-3.5 text-accent" />
                  Portable zip:{" "}
                  <a className="underline decoration-border underline-offset-4 hover:text-foreground" href={downloads.windows.fullZip}>
                    Full
                  </a>{" "}
                  ·{" "}
                  <a className="underline decoration-border underline-offset-4 hover:text-foreground" href={downloads.windows.nowebZip}>
                    Lite
                  </a>
                </p>
                <p className="mt-3 text-xs leading-relaxed text-muted-foreground">
                  <ShieldCheck className="mr-1.5 inline size-3.5 text-accent" />
                  The installer is not code-signed yet. If SmartScreen warns
                  about an unknown publisher, choose More info &gt; Run anyway.
                </p>
              </div>

              {/* Linux */}
              <div className="rounded-3xl border border-border bg-black/50 p-7 backdrop-blur sm:p-9">
                <div className="flex items-center gap-3">
                  <LinuxIcon className="size-6 text-primary" />
                  <h3 className="text-2xl font-semibold tracking-tight">Linux</h3>
                  <span className="font-code ml-auto text-xs text-muted-foreground">Debian / Ubuntu · amd64</span>
                </div>
                <div className="mt-8 flex flex-col gap-3">
                  <DownloadRow
                    href={downloads.linux.full}
                    title="Full"
                    detail=".deb · with web preview"
                    file={`qode-bundled_${VERSION}_amd64.deb`}
                    primary
                  />
                  <DownloadRow
                    href={downloads.linux.noweb}
                    title="Lite"
                    detail=".deb · no web preview"
                    file={`qode-bundled-noweb_${VERSION}_amd64.deb`}
                  />
                </div>
                <p className="font-code mt-5 overflow-x-auto whitespace-nowrap text-xs text-muted-foreground">
                  <Zap className="mr-1.5 inline size-3.5 text-accent" />
                  sudo apt install ./qode-bundled_{VERSION}_amd64.deb
                </p>
                <p className="mt-3 text-xs leading-relaxed text-muted-foreground">
                  <FolderLock className="mr-1.5 inline size-3.5 text-accent" />
                  Prefer updates through apt? Add the{" "}
                  <a className="underline decoration-border underline-offset-4 hover:text-foreground" href={`${REPO}#linux-debian--ubuntu-apt`}>
                    QODE package repository
                  </a>
                  . Switching packages keeps your settings.
                </p>
              </div>
            </div>

            <div className="mt-10 flex flex-wrap items-center gap-x-8 gap-y-3 text-sm text-muted-foreground">
              <a href={RELEASES_PAGE} className="inline-flex items-center gap-2 transition-colors hover:text-foreground">
                <ArrowUpRight className="size-4" /> All releases and checksums
              </a>
              <a href={`${REPO}#build`} className="inline-flex items-center gap-2 transition-colors hover:text-foreground">
                <Cpu className="size-4" /> Build from source
              </a>
              <a href={REPO} className="inline-flex items-center gap-2 transition-colors hover:text-foreground">
                <Columns2 className="size-4" /> Open source on GitHub
              </a>
            </div>
          </div>
        </div>
      </section>

      <footer className="relative z-10 mx-auto flex max-w-7xl flex-col items-start justify-between gap-3 border-t border-border px-6 py-8 text-xs text-muted-foreground sm:flex-row sm:items-center lg:px-10">
        <span>© 2026 QODE · MIT licensed · Windows and Linux</span>
        <a href={REPO} className="flex items-center gap-1.5 transition-colors hover:text-foreground">
          Made for people who like their editors light
          <ArrowUpRight className="size-3" />
        </a>
      </footer>
    </main>
  );
}

function OsButton({
  href,
  icon,
  label,
  primary = false,
}: {
  href: string;
  icon: React.ReactNode;
  label: string;
  primary?: boolean;
}) {
  return (
    <a
      href={href}
      className={
        primary
          ? "group inline-flex items-center gap-3 rounded-full bg-foreground px-6 py-4 text-sm font-bold text-background shadow-[0_0_60px_rgba(123,140,255,0.35)] transition-transform hover:-translate-y-0.5"
          : "group inline-flex items-center gap-3 rounded-full border border-border bg-white/[0.04] px-6 py-4 text-sm font-semibold backdrop-blur transition-colors hover:border-primary/60"
      }
    >
      {icon}
      {label}
      <ArrowDown className="size-4 transition-transform group-hover:translate-y-0.5" />
    </a>
  );
}

function DownloadRow({
  href,
  title,
  detail,
  file,
  primary = false,
}: {
  href: string;
  title: string;
  detail: string;
  file: string;
  primary?: boolean;
}) {
  return (
    <a
      href={href}
      className={`group flex items-center gap-4 rounded-2xl border px-5 py-4 transition-all hover:-translate-y-0.5 ${
        primary
          ? "border-transparent bg-foreground text-background"
          : "border-border bg-white/[0.03] hover:border-primary/60"
      }`}
    >
      <div className="min-w-0 flex-1">
        <div className="flex items-baseline gap-3">
          <span className="font-bold">{title}</span>
          <span className={`text-xs ${primary ? "text-background/70" : "text-muted-foreground"}`}>{detail}</span>
        </div>
        <div className={`font-code mt-1 truncate text-[0.7rem] ${primary ? "text-background/60" : "text-muted-foreground"}`}>
          {file}
        </div>
      </div>
      <ArrowDown className="size-4 shrink-0 transition-transform group-hover:translate-y-0.5" />
    </a>
  );
}

function PlatformCard({
  icon,
  name,
  points,
}: {
  icon: React.ReactNode;
  name: string;
  points: string[];
}) {
  return (
    <article className="panel p-8 sm:p-10">
      <div className="flex items-center gap-4">
        <span className="inline-grid size-12 place-items-center rounded-xl border border-border bg-white/[0.03] text-primary">
          {icon}
        </span>
        <h3 className="font-display text-4xl">{name}</h3>
      </div>
      <ul className="mt-8 flex flex-col gap-4">
        {points.map((p) => (
          <li key={p} className="flex gap-3 text-sm leading-relaxed text-muted-foreground">
            <span className="mt-2 size-1.5 shrink-0 rounded-full bg-accent" aria-hidden />
            {p}
          </li>
        ))}
      </ul>
    </article>
  );
}
