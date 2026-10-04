import Image from "next/image";
import {
  ArrowDown,
  ArrowUpRight,
  Braces,
  Command,
  Cpu,
  GitBranch,
  Columns2,
  Search,
  Terminal,
  Zap,
} from "lucide-react";

const DEB_URL =
  "https://github.com/zahid4kh/QODE/releases/download/0.5.0/qode_0.5.0_amd64.deb";

const features = [
  {
    icon: Terminal,
    title: "A real terminal, built in",
    body: "Pty-backed, with split panes and renamable tabs that come back with your project. Toggle it with Ctrl+J.",
    span: "lg:col-span-2",
  },
  {
    icon: GitBranch,
    title: "Git without leaving",
    body: "Status colours in the explorer, gutter markers, side-by-side diffs, history and inline blame.",
    span: "",
  },
  {
    icon: Braces,
    title: "Language servers",
    body: "Diagnostics, hover, go to definition and completion via clangd and Kotlin. Bring your own server.",
    span: "",
  },
  {
    icon: Columns2,
    title: "Split anything",
    body: "Drag a tab to an edge and keep files side by side or stacked. The layout is remembered.",
    span: "",
  },
  {
    icon: Command,
    title: "Keyboard first",
    body: "Command palette, Quick Open, Go to Symbol and Go to Line. Fuzzy, instant, no mouse required.",
    span: "",
  },
  {
    icon: Search,
    title: "Find in Files, with replace",
    body: "A background index and a worker thread keep search quick, even in large projects. Unsaved buffers count too.",
    span: "lg:col-span-2",
  },
];

const specs = [
  ["C++17 + Qt 6", "No web runtime"],
  ["0 Electron", "Not a single byte"],
  ["1 .deb", "Install and go"],
  ["MIT", "Read it, change it"],
];

const shortcuts = [
  ["Ctrl+Shift+P", "Command palette"],
  ["Ctrl+J", "Toggle terminal"],
  ["Ctrl+B", "Toggle sidebar"],
  ["Ctrl+\\", "Split editor"],
  ["F12", "Go to definition"],
  ["F5", "Run file"],
];

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
          <a className="transition-colors hover:text-foreground" href="#inside">
            Inside
          </a>
        </nav>
        <div className="flex items-center gap-3">
          <a
            href="https://github.com/zahid4kh/QODE"
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
          <p className="eyebrow rise" style={{ "--i": 0 } as React.CSSProperties}>
            Native code editor · Linux
          </p>
          <h1
            className="font-display rise mt-8 max-w-[14ch] text-balance text-[clamp(4rem,11vw,10.5rem)] leading-[0.86]"
            style={{ "--i": 1 } as React.CSSProperties}
          >
            Code without <em className="text-gradient pr-2">the baggage</em>
            <span className="caret" aria-hidden />
          </h1>
          <div
            className="rise mt-12 flex max-w-xl flex-col gap-8"
            style={{ "--i": 2 } as React.CSSProperties}
          >
            <p className="text-lg leading-relaxed text-muted-foreground">
              QODE is a lightweight editor written in C++ and Qt. It opens
              instantly, edits real files and carries its own terminal. No
              Electron, no background swarm, no drama.
            </p>
            <div className="flex flex-wrap items-center gap-4">
              <a
                href={DEB_URL}
                className="group inline-flex items-center gap-3 rounded-full bg-foreground px-7 py-4 text-sm font-bold text-background shadow-[0_0_60px_rgba(123,140,255,0.35)] transition-transform hover:-translate-y-0.5"
              >
                Download .deb
                <ArrowDown className="size-4 transition-transform group-hover:translate-y-0.5" />
              </a>
              <span className="font-code text-xs text-muted-foreground">
                v0.5.0 · amd64 · Debian/Ubuntu
              </span>
            </div>
          </div>
        </div>

        {/* Product shot: bleeds off the right edge, tilted into the dark */}
        <div
          className="rise relative z-10 mx-auto mt-6 max-w-7xl px-6 lg:px-10"
          style={{ "--i": 4 } as React.CSSProperties}
        >
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
          <p className="eyebrow">The pitch</p>
          <h2 className="font-display mt-6 max-w-[12ch] text-5xl leading-[0.95] sm:text-7xl">
            An editor should edit <em className="text-gradient">code.</em>
          </h2>
        </div>
        <div className="flex flex-col justify-end gap-10">
          <p className="max-w-md text-lg leading-relaxed text-muted-foreground">
            Everything you need sits in one window and nothing you don&apos;t.
            No accounts, no telemetry, no extension marketplace to babysit.
            Settings live in one config folder and your project folders stay
            clean.
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
        <div className="mb-14 flex flex-col justify-between gap-6 sm:flex-row sm:items-end">
          <div>
            <p className="eyebrow">What made the cut</p>
            <h2 className="font-display mt-6 text-5xl leading-[0.95] sm:text-7xl">
              Small footprint.
              <br />
              <em className="text-muted-foreground">Serious tools.</em>
            </h2>
          </div>
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
            Open a project, pick up where you left off, or jump straight to a
            file. Your tabs, splits and terminals come back exactly as you
            left them.
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
      <section
        aria-label="Keyboard shortcuts"
        className="relative z-10 border-y border-border"
      >
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
      <section
        id="download"
        className="relative z-10 px-6 py-28 lg:px-10 lg:py-40"
      >
        <div className="relative mx-auto max-w-7xl overflow-hidden rounded-[2rem] border border-primary/30 px-7 py-20 sm:px-16 lg:py-28">
          <div className="aurora !inset-auto !-right-1/4 !top-0 !h-full !w-3/4 !opacity-50" aria-hidden />
          <div className="relative">
            <p className="eyebrow">Ready when you are</p>
            <h2 className="font-display mt-6 max-w-3xl text-6xl leading-[0.9] sm:text-8xl">
              Install the editor. <em className="text-gradient">Keep the RAM.</em>
            </h2>
            <div className="mt-12 flex flex-col gap-5 sm:flex-row sm:items-center">
              <a
                href={DEB_URL}
                className="group inline-flex w-fit items-center gap-3 rounded-full bg-foreground px-7 py-4 text-sm font-bold text-background transition-transform hover:-translate-y-0.5"
              >
                Download for Linux (.deb)
                <ArrowDown className="size-4 transition-transform group-hover:translate-y-0.5" />
              </a>
              <a
                href="https://github.com/zahid4kh/QODE#build"
                className="inline-flex items-center gap-2 text-sm text-muted-foreground transition-colors hover:text-foreground"
              >
                <Cpu className="size-4" /> Or build from source
              </a>
            </div>
            <p className="font-code mt-10 text-xs text-muted-foreground">
              <Zap className="mr-2 inline size-3.5 text-accent" />
              sudo apt install ./qode_0.5.0_amd64.deb
            </p>
          </div>
        </div>
      </section>

      <footer className="relative z-10 mx-auto flex max-w-7xl flex-col items-start justify-between gap-3 border-t border-border px-6 py-8 text-xs text-muted-foreground sm:flex-row sm:items-center lg:px-10">
        <span>© 2026 QODE · MIT licensed</span>
        <a
          href="https://github.com/zahid4kh/QODE"
          className="flex items-center gap-1.5 transition-colors hover:text-foreground"
        >
          Made for people who like their editors light
          <ArrowUpRight className="size-3" />
        </a>
      </footer>
    </main>
  );
}
