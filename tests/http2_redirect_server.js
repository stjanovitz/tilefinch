// Loopback HTTP/2 server for test_http2_redirect_reuse.py. GET /first
// answers 200; /post (POST, after reading the body) answers 302 to
// /target with a small body; /post-trailer does the same and ends the
// redirect with a set-cookie trailer; /post-large sends a redirect body
// over the client's 64 KiB drain bound; /target answers 200. Every response repeats the
// same literal fields, so a client whose HPACK table falls out of step with
// this encoder decodes wrong fields on the next stream. Each stream prints one
// "stream SESSION ID METHOD PATH" line, so the driver can check that the
// redirect hop really reused the connection.
"use strict";
const fs = require("fs");
const http2 = require("http2");

const [certificate, privateKey, portFile] = process.argv.slice(2);
const fields = () => ({
  "content-type": "text/html; charset=utf-8",
  "x-frame-options": "DENY",
  "cache-control": "no-cache, no-store, max-age=0, must-revalidate",
  "x-content-type-options": "nosniff",
});
let sessions = 0;
const server = http2.createSecureServer({
  cert: fs.readFileSync(certificate),
  key: fs.readFileSync(privateKey),
});
server.on("session", (session) => {
  session.tilefinchId = ++sessions;
});
server.on("stream", (stream, headers) => {
  const method = headers[":method"];
  const path = headers[":path"];
  console.log(`stream ${stream.session.tilefinchId} ${stream.id} ${method} ${path}`);
  stream.on("error", () => {});
  if (path === "/post" || path === "/post-trailer" || path === "/post-large") {
    stream.on("data", () => {});
    stream.on("end", () => {
      const body = path === "/post-large"
        ? "<html><body>" + "m".repeat(96 * 1024) + "</body></html>"
        : "<html><body>Moved</body></html>";
      const trailer = path === "/post-trailer";
      stream.respond({
        ":status": 302,
        location: "/target",
        ...fields(),
        "content-length": String(Buffer.byteLength(body)),
      }, { waitForTrailers: trailer });
      if (trailer)
        stream.on("wantTrailers", () => {
          stream.sendTrailers({ "set-cookie": "trailer=1; Path=/" });
        });
      stream.end(body);
    });
    return;
  }
  const body = `<!doctype html><p>${path}</p>`;
  stream.respond({
    ":status": 200,
    ...fields(),
    "content-length": String(Buffer.byteLength(body)),
  });
  stream.end(body);
});
server.listen(0, "127.0.0.1", () => {
  fs.writeFileSync(portFile, String(server.address().port));
});
