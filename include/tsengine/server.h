#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "tsengine/engine.h"

namespace tsengine {

// HTTP front end for an engine.
//
//   POST /write[?precision=ns|u|ms|s]   body: Influx line protocol          -> 204
//   GET  /api/v1/query?measurement=..&field=..&tag=key:value&start=..&end=..
//                      &agg=count|sum|min|max|mean&bucket=<ns>&group_by=<tag>   -> JSON
//   GET  /api/v1/stats                                                      -> JSON
//   POST /api/v1/flush, POST /api/v1/compact
//   GET  /health
//   GET|POST /query?q=...   answers only SHOW, CREATE and DROP DATABASE, which is what the
//                           TSBS InfluxDB loader sends before it starts writing
//
// `tag` may be given several times. Repeating a key matches any of its values; different
// keys must all match. Requests are handled on a thread pool, but the engine is
// single-threaded, so one mutex lets one request touch it at a time.
//
// There is no authentication and no TLS. Anyone who can reach the port can read, write and
// fill the disk, so the server is meant for a loopback address or a trusted network only.
inline constexpr size_t kDefaultMaxRequestBytes = size_t{256} << 20;

class Server {
 public:
  // Request bodies larger than `max_request_bytes` are refused with 413, so that one request
  // cannot exhaust the server's memory.
  explicit Server(Engine* engine, size_t max_request_bytes = kDefaultMaxRequestBytes);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds to host:port. Port 0 picks any free port. Returns the port bound, or -1.
  int Bind(const std::string& host, int port);

  // Handles requests until Stop() is called from another thread. Call after Bind().
  bool Serve();

  // Blocks until Serve() is accepting connections.
  void WaitUntilReady();

  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tsengine
