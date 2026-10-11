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
