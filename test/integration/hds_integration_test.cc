#include "envoy/api/v2/eds.pb.h"
#include "envoy/api/v2/endpoint/endpoint.pb.h"
#include "envoy/service/discovery/v2/hds.pb.h"
#include "envoy/upstream/upstream.h"

#include "common/config/metadata.h"
#include "common/config/resources.h"
#include "common/network/utility.h"
#include "common/protobuf/utility.h"
#include "common/upstream/health_checker_impl.h"
#include "common/upstream/health_discovery_service.h"

#include "test/common/upstream/utility.h"
#include "test/config/utility.h"
#include "test/integration/http_integration.h"
#include "test/test_common/network_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace {

class HdsIntegrationTest : public HttpIntegrationTest,
                           public testing::TestWithParam<Network::Address::IpVersion> {
public:
  HdsIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void createUpstreams() override {
    fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_));
    hds_upstream_ = fake_upstreams_.back().get();
    HttpIntegrationTest::createUpstreams();
  }

  void initialize() override {
    setUpstreamCount(upstream_endpoints_);
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      // Setup hds and corresponding gRPC cluster.
      auto* hds_config = bootstrap.mutable_hds_config();
      hds_config->set_api_type(envoy::api::v2::core::ApiConfigSource::GRPC);
      hds_config->add_grpc_services()->mutable_envoy_grpc()->set_cluster_name("hds_cluster");
      auto* hds_cluster = bootstrap.mutable_static_resources()->add_clusters();
      hds_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      hds_cluster->mutable_circuit_breakers()->Clear();
      hds_cluster->set_name("hds_cluster");
      hds_cluster->mutable_http2_protocol_options();
      auto* cluster_0 = bootstrap.mutable_static_resources()->mutable_clusters(0);
      cluster_0->mutable_hosts()->Clear();
    });

    HttpIntegrationTest::initialize();
    host_upstream_.reset(new FakeUpstream(0, FakeHttpConnection::Type::HTTP1, version_));
  }

  void waitForHdsStream() {
    hds_fake_connection_ = hds_upstream_->waitForHttpConnection(*dispatcher_);
    hds_stream_ = hds_fake_connection_->waitForNewStream(*dispatcher_);
  }

  void cleanupUpstreamConnection() {
    codec_client_->close();
    if (fake_upstream_connection_ != nullptr) {
      fake_upstream_connection_->close();
      fake_upstream_connection_->waitForDisconnect();
    }
  }

  void cleanupHdsConnection() {
    if (hds_fake_connection_ != nullptr) {
      hds_fake_connection_->close();
      hds_fake_connection_->waitForDisconnect();
    }
  }

  envoy::service::discovery::v2::HealthCheckSpecifier makeHealthCheckSpecifier() {
    envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier;
    server_health_check_specifier.mutable_interval()->set_nanos(500000000); // 500ms

    auto* health_check = server_health_check_specifier.add_health_check();

    health_check->add_endpoints()
        ->add_endpoints()
        ->mutable_address()
        ->mutable_socket_address()
        ->set_address(Network::Test::getLoopbackAddressString(version_));
    health_check->mutable_endpoints(0)
        ->mutable_endpoints(0)
        ->mutable_address()
        ->mutable_socket_address()
        ->set_port_value(host_upstream_->localAddress()->ip()->port());
    health_check->mutable_endpoints(0)->mutable_locality()->set_region("some_region");
    health_check->mutable_endpoints(0)->mutable_locality()->set_zone("some_zone");
    health_check->mutable_endpoints(0)->mutable_locality()->set_sub_zone("crete");

    health_check->add_health_checks()->mutable_timeout()->set_seconds(1);
    health_check->mutable_health_checks(0)->mutable_interval()->set_seconds(1);
    health_check->mutable_health_checks(0)->mutable_unhealthy_threshold()->set_value(2);
    health_check->mutable_health_checks(0)->mutable_healthy_threshold()->set_value(2);
    health_check->mutable_health_checks(0)->mutable_grpc_health_check();
    health_check->mutable_health_checks(0)->mutable_http_health_check()->set_use_http2(false);
    health_check->mutable_health_checks(0)->mutable_http_health_check()->set_path("/healthcheck");

    return server_health_check_specifier;
  }

  static constexpr uint32_t upstream_endpoints_ = 0;

  IntegrationStreamDecoderPtr response_;
  std::string sub_zone_{"winter"};
  FakeHttpConnectionPtr hds_fake_connection_;
  FakeStreamPtr hds_stream_;
  FakeUpstream* hds_upstream_{};
  FakeUpstreamPtr host_upstream_{};
  uint32_t hds_requests_{};
  FakeHttpConnectionPtr host_fake_connection_;
  FakeStreamPtr host_stream;
};

INSTANTIATE_TEST_CASE_P(IpVersions, HdsIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                        TestUtility::ipTestParamsToString);

TEST_P(HdsIntegrationTest, SingleEndpointHealthy) {
  initialize();

  // Messages
  envoy::service::discovery::v2::HealthCheckRequest envoy_msg;
  envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse response;
  envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier =
      makeHealthCheckSpecifier();

  // Server <--> Envoy
  hds_fake_connection_ = hds_upstream_->waitForHttpConnection(*dispatcher_);
  hds_stream_ = hds_fake_connection_->waitForNewStream(*dispatcher_);
  hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg);

  // Server asks for healthchecking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier);

  EXPECT_EQ(host_upstream_->httpType(), FakeHttpConnection::Type::HTTP1);
  host_fake_connection_ = host_upstream_->waitForHttpConnection(*dispatcher_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a healthcheck message to an endpoint
  host_stream = host_fake_connection_->waitForNewStream(*dispatcher_);
  host_stream->waitForEndStream(*dispatcher_);
  EXPECT_STREQ(host_stream->headers().Path()->value().c_str(), "/healthcheck");
  EXPECT_STREQ(host_stream->headers().Method()->value().c_str(), "GET");

  // Endpoint reponds to the healthcheck
  host_stream->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, false);
  host_stream->encodeData(1024, true);
  hds_stream_->waitForGrpcMessage(*dispatcher_, response);
  EXPECT_EQ(
      envoy::api::v2::core::HealthStatus::HEALTHY,
      response.mutable_endpoint_health_response()->mutable_endpoints_health(0)->health_status());
  test_server_->waitForCounterGe("cluster.anna.health_check.success", 1);

  // Clean up connections
  host_fake_connection_->close();
  host_fake_connection_->waitForDisconnect();
  cleanupHdsConnection();

  EXPECT_EQ(1, test_server_->counter("hds_delegate.requests")->value());
  EXPECT_EQ(2, test_server_->counter("hds_delegate.responses")->value());
  EXPECT_EQ(1, test_server_->counter("cluster.anna.health_check.success")->value());
  EXPECT_EQ(0, test_server_->counter("cluster.anna.health_check.failure")->value());
}

TEST_P(HdsIntegrationTest, SingleEndpointUnhealthy) {
  initialize();

  // Messages
  envoy::service::discovery::v2::HealthCheckRequest envoy_msg;
  envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse response;
  envoy::service::discovery::v2::HealthCheckSpecifier server_health_check_specifier =
      makeHealthCheckSpecifier();

  // Server <--> Envoy
  hds_fake_connection_ = hds_upstream_->waitForHttpConnection(*dispatcher_);
  hds_stream_ = hds_fake_connection_->waitForNewStream(*dispatcher_);
  hds_stream_->waitForGrpcMessage(*dispatcher_, envoy_msg);

  // Server asks for healthchecking
  hds_stream_->startGrpcStream();
  hds_stream_->sendGrpcMessage(server_health_check_specifier);

  EXPECT_EQ(host_upstream_->httpType(), FakeHttpConnection::Type::HTTP1);
  host_fake_connection_ = host_upstream_->waitForHttpConnection(*dispatcher_);
  test_server_->waitForCounterGe("hds_delegate.requests", ++hds_requests_);

  // Envoy sends a healthcheck message to an endpoint
  host_stream = host_fake_connection_->waitForNewStream(*dispatcher_);
  host_stream->waitForEndStream(*dispatcher_);
  EXPECT_STREQ(host_stream->headers().Path()->value().c_str(), "/healthcheck");
  EXPECT_STREQ(host_stream->headers().Method()->value().c_str(), "GET");

  // Endpoint reponds to the healthcheck
  host_stream->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, false);
  host_stream->encodeData(1024, true);
  hds_stream_->waitForGrpcMessage(*dispatcher_, response);
  EXPECT_EQ(
      envoy::api::v2::core::HealthStatus::UNHEALTHY,
      response.mutable_endpoint_health_response()->mutable_endpoints_health(0)->health_status());
  test_server_->waitForCounterGe("cluster.anna.health_check.failure", 1);

  // Clean up connections
  host_fake_connection_->close();
  host_fake_connection_->waitForDisconnect();
  cleanupHdsConnection();

  EXPECT_EQ(1, test_server_->counter("hds_delegate.requests")->value());
  EXPECT_EQ(2, test_server_->counter("hds_delegate.responses")->value());
  EXPECT_EQ(0, test_server_->counter("cluster.anna.health_check.success")->value());
  EXPECT_EQ(1, test_server_->counter("cluster.anna.health_check.failure")->value());
}

} // namespace
} // namespace Envoy
