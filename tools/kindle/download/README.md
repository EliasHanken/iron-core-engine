# Kindle's private download

A small Worker that gives each tester their own link, serving `Kindle.zip` from a private R2 bucket. Only hashes of the invite codes are stored, so nothing here needs to stay secret.

R2 must be enabled on the Cloudflare account first, in the dashboard.

```
wrangler r2 bucket create kindle-builds
wrangler r2 object put kindle-builds/Kindle.zip --file ../../../tmp/dist/Kindle.zip --remote
wrangler deploy
```

For each tester, make a random code, then put the SHA-256 hashes of all codes in one comma-separated secret. Their link is `https://<worker address>/d/<code>`.

```
wrangler secret put INVITE_HASHES
```

To cut someone off, set the secret again without their hash. To ship a new build, repeat the `object put`.

To open it to everyone, set `PUBLIC_DOWNLOAD` to `"true"` in `wrangler.toml` and deploy again. The front page then offers the download, and personal links keep working.
