// A private download for Kindle. Each tester gets their own link, /d/<code>, and only
// a code's hash is stored, so a leaked config reveals nothing. To cut someone off,
// remove their hash and redeploy.
//
// Codes live in the INVITE_HASHES secret, as comma-separated SHA-256 hex.

const FILE = "Kindle.zip";

const page = (title, body, status = 200) =>
  new Response(
    `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex"><title>Kindle</title>
<style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:#000;color:#f3e6d6;
font:18px/1.5 system-ui,sans-serif;text-align:center;padding:24px;box-sizing:border-box}
h1{font-weight:400;letter-spacing:.4em;margin:0 0 .4em}p{max-width:30em;margin:.5em auto}
a{display:inline-block;margin-top:1em;padding:.7em 1.6em;border:1px solid #ff9a3d;color:#ff9a3d;text-decoration:none}
small{color:#b9a78f}</style><main><h1>${title}</h1>${body}</main>`,
    { status, headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store",
                         "x-robots-tag": "noindex" } },
  );

async function sha256(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function allowed(code, env) {
  const hash = await sha256(code);
  return (env.INVITE_HASHES || "").split(",").map((h) => h.trim()).includes(hash);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const match = url.pathname.match(/^\/d\/([A-Za-z0-9_-]{8,64})(\/Kindle\.zip)?$/);
    if (!match) return page("KINDLE", "<p>This download is private. Use the link you were sent.</p>", 404);
    if (!(await allowed(match[1], env))) {
      return page("KINDLE", "<p>That link isn't valid. Ask Elias for a new one.</p>", 403);
    }

    if (!match[2]) {
      const info = await env.BUCKET.head(FILE);
      const size = info ? `${Math.round(info.size / 1e6)} MB` : "";
      return page("KINDLE", `<p>You are a swarm of light in a dead machine. Relight it, and wake the core.</p>
<a href="${url.pathname}/Kindle.zip">Download for Windows</a>
<p><small>${size}. Needs Windows 10 or 11 and a Vulkan 1.3 graphics card.<br>
This build is for you: please don't pass it on.</small></p>`);
    }

    const object = await env.BUCKET.get(FILE);
    if (!object) return page("KINDLE", "<p>The build isn't up yet. Try again soon.</p>", 503);
    return new Response(object.body, {
      headers: {
        "content-type": "application/zip",
        "content-length": String(object.size),
        "content-disposition": 'attachment; filename="Kindle.zip"',
        "cache-control": "private, no-store",
        "x-robots-tag": "noindex",
      },
    });
  },
};
