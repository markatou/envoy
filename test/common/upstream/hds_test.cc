#include "envoy/service/discovery/v2/hds.pb.h"

#include "common/ssl/context_manager_impl.h"
#include "common/stats/stats_impl.h"
#include "common/upstream/health_discovery_service.h"

#include "test/common/upstream/hds_helper.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::_;

namespace Envoy {
namespace Upstream {

class HdsTest : public testing::Test {
public:
  HdsTest()
      : retry_timer_(new Event::MockTimer()), server_response_timer_(new Event::MockTimer()),
        async_client_(new Grpc::MockAsyncClient()) {
    node_.set_id("foo");
  }

  void createHdsDelegate() {
    InSequence s;
    EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([this](Event::TimerCb timer_cb) {
      retry_timer_cb_ = timer_cb;
      return retry_timer_;
    }));
    EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([this](Event::TimerCb timer_cb) {
      server_response_timer_cb_ = timer_cb;
      return server_response_timer_;
    }));
    hds_delegate_.reset(new HdsDelegate(node_, stats_store_, Grpc::AsyncClientPtr(async_client_),
                                        dispatcher_, runtime_, stats_store_, ssl_context_manager_,
                                        secret_manager_, random_, test_factory_));
  }

  envoy::api::v2::core::Node node_;
  Event::MockDispatcher dispatcher_;

  Stats::IsolatedStoreImpl stats_store_;

  HdsDelegatePtr hds_delegate_;
  TestHdsInfoFactory test_factory_;

  Event::MockTimer* retry_timer_;
  Event::TimerCb retry_timer_cb_;
  Event::MockTimer* server_response_timer_;
  Event::TimerCb server_response_timer_cb_;

  std::unique_ptr<envoy::service::discovery::v2::HealthCheckSpecifier> message;
  Grpc::MockAsyncStream async_stream_;
  Grpc::MockAsyncClient* async_client_;
  Runtime::MockLoader runtime_;
  Ssl::ContextManagerImpl ssl_context_manager_{runtime_};
  Secret::MockSecretManager secret_manager_;
  NiceMock<Runtime::MockRandomGenerator> random_;
};

TEST_F(HdsTest, TestProcessMessageEndpoints) {
  EXPECT_CALL(*async_client_, start(_, _)).WillOnce(Return(&async_stream_));
  EXPECT_CALL(async_stream_, sendMessage(_, _));
  createHdsDelegate();

  // Create Message
  message.reset(new envoy::service::discovery::v2::HealthCheckSpecifier);
  message->mutable_interval()->set_seconds(1);

  auto* health_check = message->add_health_check();
  health_check->set_cluster_name("anna");
  health_check->add_endpoints()
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("127.0.0.0");
  health_check->mutable_endpoints(0)
      ->mutable_endpoints(0)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(1234);

  health_check->mutable_endpoints(0)
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("127.0.0.1");
  health_check->mutable_endpoints(0)
      ->mutable_endpoints(1)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(2345);

  health_check->add_endpoints()
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("127.0.1.0");
  health_check->mutable_endpoints(1)
      ->mutable_endpoints(0)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(8765);

  auto* health_check2 = message->add_health_check();
  health_check2->set_cluster_name("voronoi");
  health_check2->add_endpoints()
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("128.0.0.0");
  health_check2->mutable_endpoints(0)
      ->mutable_endpoints(0)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(1234);

  health_check2->mutable_endpoints(0)
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("128.0.0.1");
  health_check2->mutable_endpoints(0)
      ->mutable_endpoints(1)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(2345);

  health_check2->add_endpoints()
      ->add_endpoints()
      ->mutable_address()
      ->mutable_socket_address()
      ->set_address("128.0.1.0");
  health_check2->mutable_endpoints(1)
      ->mutable_endpoints(0)
      ->mutable_address()
      ->mutable_socket_address()
      ->set_port_value(8765);

  // Process message
  hds_delegate_->processMessage(std::move(message));

  // Check Correctness
  auto& host = hds_delegate_->hdsClusters()[0]->prioritySet().hostSetsPerPriority()[0]->hosts()[0];
  EXPECT_EQ(host->address()->ip()->addressAsString(), "127.0.0.0");
  EXPECT_EQ(host->address()->ip()->port(), 1234);

  auto& host2 = hds_delegate_->hdsClusters()[0]->prioritySet().hostSetsPerPriority()[0]->hosts()[1];
  EXPECT_EQ(host2->address()->ip()->addressAsString(), "127.0.0.1");
  EXPECT_EQ(host2->address()->ip()->port(), 2345);

  auto& host3 = hds_delegate_->hdsClusters()[0]->prioritySet().hostSetsPerPriority()[0]->hosts()[2];
  EXPECT_EQ(host3->address()->ip()->addressAsString(), "127.0.1.0");
  EXPECT_EQ(host3->address()->ip()->port(), 8765);

  auto& host4 = hds_delegate_->hdsClusters()[1]->prioritySet().hostSetsPerPriority()[0]->hosts()[0];
  EXPECT_EQ(host4->address()->ip()->addressAsString(), "128.0.0.0");
  EXPECT_EQ(host4->address()->ip()->port(), 1234);

  auto& host5 = hds_delegate_->hdsClusters()[1]->prioritySet().hostSetsPerPriority()[0]->hosts()[1];
  EXPECT_EQ(host5->address()->ip()->addressAsString(), "128.0.0.1");
  EXPECT_EQ(host5->address()->ip()->port(), 2345);

  auto& host6 = hds_delegate_->hdsClusters()[1]->prioritySet().hostSetsPerPriority()[0]->hosts()[2];
  EXPECT_EQ(host6->address()->ip()->addressAsString(), "128.0.1.0");
  EXPECT_EQ(host6->address()->ip()->port(), 8765);
}

} // namespace Upstream
} // namespace Envoy
