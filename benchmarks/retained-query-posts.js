"use strict";

/* Offline opt-in only. Live acquisition must independently authorize each
   exact anonymous query. Never infer permission to contact an endpoint. */
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

function queryDocumentOnly(query) {
  if (typeof query !== "string" || query.length > 16384) return false;
  const tokens = query.match(/#[^\r\n]*|"""[\s\S]*?"""|"(?:\\.|[^"\\])*"|[_A-Za-z][_0-9A-Za-z]*|\.\.\.|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|[!$():=@\[\]{|},]|\s+/g);
  if (!tokens || tokens.join("") !== query) return false;
  let braces = 0, parentheses = 0, brackets = 0, queries = 0, definition = false;
  for (const token of tokens) {
    if (/^(?:\s|#)/.test(token)) continue;
    if (braces === 0 && parentheses === 0 && brackets === 0 && !definition) {
      if (token === "query") { queries++; definition = true; }
      else if (token === "fragment") definition = true;
      else if (token === "{") { queries++; definition = true; }
      else return false;
    }
    if (token === "{") braces++;
    else if (token === "}") { if (--braces < 0) return false; if (!braces) definition = false; }
    else if (token === "(") parentheses++;
    else if (token === ")") { if (--parentheses < 0) return false; }
    else if (token === "[") brackets++;
    else if (token === "]") { if (--brackets < 0) return false; }
    if (braces > 64 || parentheses > 64 || brackets > 64 || queries > 1) return false;
  }
  return queries === 1 && !braces && !parentheses && !brackets && !definition;
}

function readQueryBody(body) {
  if (!Buffer.isBuffer(body) || !body.length || body.length > 16384) throw new Error("query body exceeds bound");
  const text = body.toString("utf8");
  if (!Buffer.from(text).equals(body)) throw new Error("query body is not UTF-8");
  const value = JSON.parse(text);
  if (JSON.stringify(value) !== text || !value || typeof value !== "object" || Array.isArray(value)
      || Object.keys(value).some(k => !["query", "variables", "operationName"].includes(k))
      || !queryDocumentOnly(value.query)
      || (value.variables !== undefined && (value.variables === null
        || typeof value.variables !== "object" || Array.isArray(value.variables)))
      || (value.operationName !== undefined && value.operationName !== null
        && !/^[_A-Za-z][_0-9A-Za-z]{0,127}$/.test(value.operationName))) {
    throw new Error("retained POST is not a bounded single GraphQL query");
  }
  return text;
}

function loadRetainedQueryPosts(trace, documentUrl, hashBody) {
  const origin = new URL(documentUrl).origin, result = [], identities = new Set();
  for (const record of trace.records) {
    if (record.method !== "POST") continue;
    if (result.length === 32) throw new Error("retained query count exceeds bound");
    const url = new URL(record.url);
    if (url.protocol !== "https:" || url.origin !== origin || url.username || url.password || url.hash)
      throw new Error("retained query must be same-origin HTTPS");
    const file = path.join(trace.root, `${record.id}.request`), stat = fs.lstatSync(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 16384) throw new Error("invalid query sidecar");
    const body = fs.readFileSync(file), text = readQueryBody(body);
    if (body.length !== record.requestBodyLength || hashBody(body) !== record.requestBodyHash
        || record.requestCookieBytes !== 0 || record.requestCredentials !== 0)
      throw new Error("query body or anonymous authority differs from retained metadata");
    const sha256 = crypto.createHash("sha256").update(body).digest("hex");
    const identity = record.url + "\0" + sha256;
    if (identities.has(identity)) throw new Error("duplicate retained query body is ambiguous");
    identities.add(identity);
    result.push({id:record.id,url:record.url,body:text,sha256});
  }
  if (!result.length) throw new Error("no retained query POSTs");
  return result;
}

module.exports = {queryDocumentOnly, readQueryBody, loadRetainedQueryPosts};
