#include "tsengine/server.h"

#include <httplib.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

#include "tsengine/api.h"

namespace tsengine {
namespace {

constexpr char kJson[] = "application/json";

// What InfluxDB answers to SHOW DATABASES. The TSBS loader asks this before it loads.
constexpr char kShowDatabases[] =
    "{\"results\":[{\"series\":[{\"name\":\"databases\",\"columns\":[\"name\"],"
    "\"values\":[[\"_internal\"]]}]}]}";

int64_t NowNs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

// Maps the `precision` parameter of a write to the size of one timestamp unit.
bool TimestampUnit(std::string_view precision, int64_t* unit_ns) {
  if (precision == "ns" || precision == "n") {
    *unit_ns = 1;
  } else if (precision == "u" || precision == "us") {
    *unit_ns = 1000;
  } else if (precision == "ms") {
    *unit_ns = 1000000;
  } else if (precision == "s") {
    *unit_ns = 1000000000;
  } else {
    return false;
  }
  return true;
}

// Reads an integer parameter into *value if it is present. Returns false if it is present
// but is not an integer.
bool Int64Param(const httplib::Request& req, const char* name, int64_t* value) {
  if (!req.has_param(name)) {
    return true;
  }
  const std::string text = req.get_param_value(name);
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, *value);
  return ec == std::errc() && ptr == end && begin != end;
}

void Fail(const Status& status, httplib::Response* res) {
  res->status = status.code() == Status::Code::kInvalidArgument ? 400 : 500;
  res->set_content(Api::ErrorJson(status), kJson);
}

}  // namespace

struct Server::Impl {
  explicit Impl(Engine* engine) : api(engine) {}

  using Method = void (Impl::*)(const httplib::Request&, httplib::Response*);

  // Wraps a member function as an httplib handler.
  httplib::Server::Handler Route(Method method) {
    return [this, method](const httplib::Request& req, httplib::Response& res) {
      (this->*method)(req, &res);
    };
  }

  void Health(const httplib::Request& req, httplib::Response* res);
  void Write(const httplib::Request& req, httplib::Response* res);
  void InfluxQuery(const httplib::Request& req, httplib::Response* res);
  void Query(const httplib::Request& req, httplib::Response* res);
  void Stats(const httplib::Request& req, httplib::Response* res);
  void Flush(const httplib::Request& req, httplib::Response* res);
  void Compact(const httplib::Request& req, httplib::Response* res);

  Api api;
  // Guards `api`: the engine handles one request at a time.
  std::mutex mutex;
  httplib::Server http;
};

void Server::Impl::Health(const httplib::Request&, httplib::Response* res) {
  res->set_content("ok\n", "text/plain");
}

void Server::Impl::Write(const httplib::Request& req, httplib::Response* res) {
  WriteRequest request;
  request.body = req.body;
  request.now_ns = NowNs();
  if (req.has_param("precision")) {
    const std::string precision = req.get_param_value("precision");
    if (!TimestampUnit(precision, &request.timestamp_unit_ns)) {
      Fail(Status::InvalidArgument("unknown precision: " + precision), res);
      return;
    }
  }
  // Parse on this request's own thread, in parallel with other requests. Only storing the
  // result needs the engine, and so the lock.
  ParsedBatch batch;
  Status s = Api::Parse(request, &batch);
  if (s.ok()) {
    WriteSummary summary;
    const std::lock_guard<std::mutex> lock(mutex);
    s = api.Store(batch, &summary);
  }
  if (!s.ok()) {
    Fail(s, res);
    return;
  }
  res->status = 204;
}

void Server::Impl::InfluxQuery(const httplib::Request& req, httplib::Response* res) {
  std::string q = req.get_param_value("q");
  for (char& c : q) {
    // A '+' in a query string stands for a space.
    c = c == '+' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (q.starts_with("show databases")) {
    res->set_content(kShowDatabases, kJson);
  } else if (q.starts_with("create database") || q.starts_with("drop database")) {
    // There is only one database, so there is nothing to create or drop.
    res->set_content("{\"results\":[{}]}", kJson);
  } else {
    Fail(Status::InvalidArgument("InfluxQL is not supported; use /api/v1/query"), res);
  }
}

void Server::Impl::Query(const httplib::Request& req, httplib::Response* res) {
  QueryRequest request;
  request.selector.measurement = req.get_param_value("measurement");
  request.selector.field = req.get_param_value("field");
  request.group_by = req.get_param_value("group_by");
  bool numbers_ok = Int64Param(req, "start", &request.range.min);
  numbers_ok = numbers_ok && Int64Param(req, "end", &request.range.max);
  numbers_ok = numbers_ok && Int64Param(req, "bucket", &request.bucket_width_ns);
  if (!numbers_ok) {
    Fail(Status::InvalidArgument("start, end and bucket must be integers (nanoseconds)"), res);
    return;
  }
  if (!ParseAggregateKind(req.get_param_value("agg"), &request.aggregate)) {
    Fail(Status::InvalidArgument("agg must be count, sum, min, max or mean"), res);
    return;
  }
  // tag=key:value, possibly several times. Values given for the same key are alternatives.
  const size_t tag_count = req.get_param_value_count("tag");
  for (size_t i = 0; i < tag_count; ++i) {
    const std::string tag = req.get_param_value("tag", i);
    const size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0) {
      Fail(Status::InvalidArgument("tag must look like key:value"), res);
      return;
    }
    const std::string key = tag.substr(0, colon);
    TagFilter* filter = nullptr;
    for (TagFilter& existing : request.selector.tags) {
      if (existing.key == key) {
        filter = &existing;
      }
    }
    if (filter == nullptr) {
      request.selector.tags.emplace_back();
      filter = &request.selector.tags.back();
      filter->key = key;
    }
    filter->values.push_back(tag.substr(colon + 1));
  }

  std::string json;
  Status s;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    s = api.Query(request, &json);
  }
  if (!s.ok()) {
    Fail(s, res);
    return;
  }
  res->set_content(json, kJson);
}

void Server::Impl::Stats(const httplib::Request&, httplib::Response* res) {
  const std::lock_guard<std::mutex> lock(mutex);
  res->set_content(api.StatsJson(), kJson);
}

void Server::Impl::Flush(const httplib::Request&, httplib::Response* res) {
  const std::lock_guard<std::mutex> lock(mutex);
  const Status s = api.Flush();
  if (!s.ok()) {
    Fail(s, res);
    return;
  }
  res->set_content("{}", kJson);
}

void Server::Impl::Compact(const httplib::Request&, httplib::Response* res) {
  const std::lock_guard<std::mutex> lock(mutex);
  const Status s = api.Compact();
  if (!s.ok()) {
    Fail(s, res);
    return;
  }
  res->set_content("{}", kJson);
}

Server::Server(Engine* engine) : impl_(std::make_unique<Impl>(engine)) {
  Impl* impl = impl_.get();
  httplib::Server& http = impl->http;
  // Send each response as soon as it is written. Without this, Nagle's algorithm holds the
  // last small packet of a response until the client acknowledges the previous one, and the
  // client delays that acknowledgement, which adds about 40 ms to every small request.
  http.set_tcp_nodelay(true);
  http.Get("/health", impl->Route(&Impl::Health));
  http.Post("/write", impl->Route(&Impl::Write));
  http.Get("/query", impl->Route(&Impl::InfluxQuery));
  http.Post("/query", impl->Route(&Impl::InfluxQuery));
  http.Get("/api/v1/query", impl->Route(&Impl::Query));
  http.Get("/api/v1/stats", impl->Route(&Impl::Stats));
  http.Post("/api/v1/flush", impl->Route(&Impl::Flush));
  http.Post("/api/v1/compact", impl->Route(&Impl::Compact));
}

Server::~Server() = default;

int Server::Bind(const std::string& host, int port) {
  if (port == 0) {
    return impl_->http.bind_to_any_port(host);
  }
  return impl_->http.bind_to_port(host, port) ? port : -1;
}

bool Server::Serve() { return impl_->http.listen_after_bind(); }

void Server::WaitUntilReady() { impl_->http.wait_until_ready(); }

void Server::Stop() { impl_->http.stop(); }

}  // namespace tsengine
