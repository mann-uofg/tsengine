#include "tsengine/server.h"

#include <gtest/gtest.h>
#include <httplib.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"
#include "tsengine/engine.h"

namespace tsengine {
namespace {

// Starts a real server on a free port and talks to it over HTTP.
class ServerTest : public TempDirTest {
 protected:
  void SetUp() override {
    TempDirTest::SetUp();
    EngineOptions options;
    options.sync_writes = false;
    const Status s = Engine::Open(Path("db"), options, &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
    server_ = std::make_unique<Server>(engine_.get());
    port_ = server_->Bind("127.0.0.1", 0);
    ASSERT_GT(port_, 0);
    thread_ = std::thread([this] { server_->Serve(); });
    server_->WaitUntilReady();
    client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  }

  void TearDown() override {
    if (thread_.joinable()) {
      server_->Stop();
      thread_.join();
    }
    TempDirTest::TearDown();
  }

  // Sends a GET and returns the status, keeping the response body in body_.
  int Get(const std::string& path) {
    const httplib::Result result = client_->Get(path);
    if (!result) {
      return -1;
    }
    body_ = result->body;
    return result->status;
  }

  int Post(const std::string& path, const std::string& body) {
    const httplib::Result result = client_->Post(path, body, "text/plain");
    if (!result) {
      return -1;
    }
    body_ = result->body;
    return result->status;
  }

  // True if the last response body contains `text`.
  bool Has(const std::string& text) const { return body_.find(text) != std::string::npos; }

  std::unique_ptr<Engine> engine_;
  std::unique_ptr<Server> server_;
  std::unique_ptr<httplib::Client> client_;
  std::thread thread_;
  int port_ = 0;
  std::string body_;
};

// Run by each writer thread: fifty single-point requests on a connection of its own.
void WriteFiftyPoints(int port, int writer) {
  httplib::Client client("127.0.0.1", port);
  const std::string series = "load,writer=w" + std::to_string(writer) + " value=1 ";
  for (int i = 0; i < 50; ++i) {
    client.Post("/write", series + std::to_string(writer * 1000 + i), "text/plain");
  }
}

TEST_F(ServerTest, Health) {
  EXPECT_EQ(Get("/health"), 200);
  EXPECT_EQ(body_, "ok\n");
}

TEST_F(ServerTest, WriteThenQuery) {
  const std::string lines =
      "cpu,host=a user=1 0\n"
      "cpu,host=a user=5 10\n"
      "cpu,host=b user=3 0\n"
      "cpu,host=b user=9 10\n";
  EXPECT_EQ(Post("/write", lines), 204);

  EXPECT_EQ(Get("/api/v1/query?measurement=cpu&field=user&tag=host:a"), 200);
  const std::string a = "{\"name\":\"cpu,host=a user\",\"timestamps\":[0,10],\"values\":[1,5]}";
  EXPECT_TRUE(Has(a)) << body_;
  EXPECT_TRUE(Has("\"series\":1")) << body_;

  EXPECT_EQ(Get("/api/v1/query?measurement=cpu&field=user&agg=max&bucket=10"), 200);
  EXPECT_TRUE(Has("\"timestamps\":[0,10],\"values\":[3,9]")) << body_;

  EXPECT_EQ(Get("/api/v1/query?measurement=cpu&agg=sum&group_by=host&start=0&end=10"), 200);
  EXPECT_TRUE(Has("{\"name\":\"a\",\"timestamps\":[0],\"values\":[6]}")) << body_;
  EXPECT_TRUE(Has("{\"name\":\"b\",\"timestamps\":[0],\"values\":[12]}")) << body_;

  // The same tag key twice means either value.
  EXPECT_EQ(Get("/api/v1/query?measurement=cpu&agg=count&tag=host:a&tag=host:b"), 200);
  EXPECT_TRUE(Has("\"values\":[4]")) << body_;
}

TEST_F(ServerTest, WritePrecision) {
  EXPECT_EQ(Post("/write?precision=s", "cpu value=1 3\n"), 204);
  EXPECT_EQ(Get("/api/v1/query?measurement=cpu"), 200);
  EXPECT_TRUE(Has("\"timestamps\":[3000000000]")) << body_;
  EXPECT_EQ(Post("/write?precision=fortnights", "cpu value=1 3\n"), 400);
}

TEST_F(ServerTest, BadRequestsGetA400WithAReason) {
  EXPECT_EQ(Post("/write", "cpu value=1 1\nnot line protocol\n"), 400);
  EXPECT_TRUE(Has("line 2")) << body_;
  EXPECT_EQ(Get("/api/v1/query?start=yesterday"), 400);
  EXPECT_TRUE(Has("error")) << body_;
  EXPECT_EQ(Get("/api/v1/query?agg=median"), 400);
  EXPECT_EQ(Get("/api/v1/query?tag=novalue"), 400);
  EXPECT_EQ(Get("/no/such/path"), 404);
}

TEST_F(ServerTest, FlushCompactAndStats) {
  EXPECT_EQ(Post("/write", "cpu value=1 1\n"), 204);
  EXPECT_EQ(Get("/api/v1/stats"), 200);
  EXPECT_TRUE(Has("\"block_files\":0,\"memtable_points\":1")) << body_;
  EXPECT_EQ(Post("/api/v1/flush", ""), 200);
  EXPECT_EQ(Post("/api/v1/compact", ""), 200);
  EXPECT_EQ(Get("/api/v1/stats"), 200);
  EXPECT_TRUE(Has("\"block_files\":1,\"memtable_points\":0")) << body_;
  EXPECT_EQ(Get("/api/v1/query?measurement=cpu"), 200);
  EXPECT_TRUE(Has("\"values\":[1]")) << body_;
}

// A second server on the same engine that accepts at most 64 bytes per request.
TEST_F(ServerTest, OversizedBodyIsRefused) {
  Server small(engine_.get(), 64);
  const int port = small.Bind("127.0.0.1", 0);
  ASSERT_GT(port, 0);
  std::thread thread([&small] { small.Serve(); });
  small.WaitUntilReady();
  {
    httplib::Client client("127.0.0.1", port);
    const httplib::Result fits = client.Post("/write", "cpu value=1 1\n", "text/plain");
    ASSERT_TRUE(static_cast<bool>(fits));
    EXPECT_EQ(fits->status, 204);
    const std::string big(4096, 'x');
    const httplib::Result refused = client.Post("/write", big, "text/plain");
    ASSERT_TRUE(static_cast<bool>(refused));
    EXPECT_EQ(refused->status, 413);
  }
  small.Stop();
  thread.join();
  // Nothing of the refused body was stored.
  EXPECT_EQ(Get("/api/v1/query?measurement=cpu&agg=count"), 200);
  EXPECT_TRUE(Has("\"values\":[1]")) << body_;
}

TEST_F(ServerTest, HostileInputIsRejectedCleanly) {
  // A timestamp that overflows once scaled to nanoseconds.
  EXPECT_EQ(Post("/write?precision=s", "cpu value=1 99999999999999\n"), 400);
  // Bytes that are not text, a line with no end, and a body of only separators.
  EXPECT_EQ(Post("/write", std::string("\x00\xff\xfe,=, \x01", 8)), 400);
  EXPECT_EQ(Post("/write", std::string(100000, 'a')), 400);
  EXPECT_EQ(Post("/write", std::string(5000, ',')), 400);
  // Extreme query parameters.
  EXPECT_EQ(Get("/api/v1/query?start=-9223372036854775808&end=9223372036854775807&bucket=1"), 200);
  EXPECT_EQ(Get("/api/v1/query?agg=sum&bucket=9223372036854775807"), 200);
  EXPECT_EQ(Get("/api/v1/query?agg=sum&bucket=-5&start=5&end=1"), 200);
  EXPECT_EQ(Get("/api/v1/query?start=99999999999999999999"), 400);
  EXPECT_EQ(Get("/api/v1/query?measurement=%00%ff&tag=%22:%5c&group_by=%0a"), 200);
  // The server is still healthy and stored none of it.
  EXPECT_EQ(Get("/health"), 200);
  EXPECT_EQ(Get("/api/v1/stats"), 200);
  EXPECT_TRUE(Has("\"series\":0")) << body_;
}

// The requests tsbs_load_influx makes before it starts writing.
TEST_F(ServerTest, AnswersTheInfluxLoaderHandshake) {
  EXPECT_EQ(Get("/query?q=show%20databases"), 200);
  EXPECT_TRUE(Has("\"values\":[[\"_internal\"]]")) << body_;
  EXPECT_EQ(Post("/query?q=drop+database+benchmark", ""), 200);
  EXPECT_EQ(Get("/query?q=CREATE%20DATABASE%20benchmark"), 200);
  EXPECT_EQ(Post("/write?consistency=all&db=benchmark", "cpu value=1 1\n"), 204);
  EXPECT_EQ(Get("/query?q=select%20*%20from%20cpu"), 400);
}

// The server handles requests on several threads; the engine must still see them one at a
// time, and none may be lost.
TEST_F(ServerTest, ConcurrentWritersAllLand) {
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back(WriteFiftyPoints, port_, w);
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  EXPECT_EQ(Get("/api/v1/query?measurement=load&agg=count"), 200);
  EXPECT_TRUE(Has("\"values\":[200]")) << body_;
  EXPECT_TRUE(Has("\"series\":4")) << body_;
}

}  // namespace
}  // namespace tsengine
