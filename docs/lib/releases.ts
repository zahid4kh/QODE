// Which release each download comes from. Releases are not always complete: a version may be published for Linux
// first and get its Windows files later. So every file is looked up on its own: it links to the newest release
// that has it, and Windows and Linux can show different versions.

export const REPO = "https://github.com/zahid4kh/QODE";
export const RELEASES_PAGE = `${REPO}/releases`;
const API = "https://api.github.com/repos/zahid4kh/QODE/releases?per_page=30";

// How often the page asks GitHub again (seconds). Also `revalidate` in app/page.tsx.
export const REFRESH_SECONDS = 900;

export type Download = {
  href: string;
  file: string; // the file name, or what to look for on the releases page
  version: string | null; // null: GitHub could not be asked, href is the releases page
};

const kinds = {
  windowsFull: (v: string) => `QODE-${v}-setup.exe`,
  windowsNoweb: (v: string) => `QODE-${v}-noweb-setup.exe`,
  windowsZip: (v: string) => `QODE-${v}-portable.zip`,
  windowsNowebZip: (v: string) => `QODE-${v}-noweb-portable.zip`,
  linuxFull: (v: string) => `qode-bundled_${v}_amd64.deb`,
  linuxNoweb: (v: string) => `qode-bundled-noweb_${v}_amd64.deb`,
};

export type Kind = keyof typeof kinds;
export type Downloads = Record<Kind, Download>;

type Release = {
  tag_name: string;
  draft: boolean;
  prerelease: boolean;
  assets: { name: string; browser_download_url: string }[];
};

// "2.1.10" > "2.1.9"; a leading "v" is ignored.
function compareVersions(a: string, b: string) {
  const pa = a.replace(/^v/, "").split(".").map((n) => parseInt(n, 10) || 0);
  const pb = b.replace(/^v/, "").split(".").map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

function fallback(kind: Kind): Download {
  return { href: RELEASES_PAGE, file: kinds[kind]("<version>"), version: null };
}

export async function getDownloads(): Promise<Downloads> {
  let releases: Release[];
  try {
    const headers: Record<string, string> = { Accept: "application/vnd.github+json", "User-Agent": "qode-site" };
    if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
    const res = await fetch(API, { headers, next: { revalidate: REFRESH_SECONDS } });
    if (!res.ok) throw new Error(`GitHub answered ${res.status}`);
    releases = await res.json();
  } catch (e) {
    // While the site runs, a failed refresh keeps the page it already has (a render that throws is not cached).
    // Only the first build has nothing to keep: then every link goes to the releases page.
    if (process.env.NEXT_PHASE !== "phase-production-build") throw e;
    console.warn(`Releases could not be read (${e}); the download links point to the releases page.`);
    return Object.fromEntries(Object.keys(kinds).map((k) => [k, fallback(k as Kind)])) as Downloads;
  }

  const published = releases
    .filter((r) => !r.draft && !r.prerelease)
    .sort((a, b) => compareVersions(b.tag_name, a.tag_name));

  const result = {} as Downloads;
  for (const kind of Object.keys(kinds) as Kind[]) {
    result[kind] = fallback(kind);
    for (const r of published) {
      const version = r.tag_name.replace(/^v/, "");
      const asset = r.assets.find((a) => a.name === kinds[kind](version));
      if (asset) {
        result[kind] = { href: asset.browser_download_url, file: asset.name, version };
        break;
      }
    }
  }
  return result;
}
