const http = require("http");
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "public-mobile");
const port = 8765;

http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  let pathname = decodeURIComponent(url.pathname);
  if (pathname === "/" || pathname === "") pathname = "/QTA-Mobile.html";

  const file = path.resolve(root, "." + pathname);
  if (!file.startsWith(path.resolve(root))) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }

  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }

    res.writeHead(200, {
      "Content-Type": file.endsWith(".html") ? "text/html; charset=utf-8" : "application/octet-stream",
      "Cache-Control": "no-store"
    });
    res.end(data);
  });
}).listen(port, "0.0.0.0", () => {
  console.log(`QTA Mobile em http://0.0.0.0:${port}/QTA-Mobile.html`);
});
