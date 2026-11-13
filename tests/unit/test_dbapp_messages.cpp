#include <gtest/gtest.h>

#include "dbapp_messages.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::dbapp;

namespace {

// Helper: serialise a message into a buffer and deserialise it back.
template <typename Msg>
auto round_trip(const Msg& msg) -> std::optional<Msg> {
  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();

  BinaryReader r(buf);
  auto result = Msg::Deserialize(r);
  if (!result) return std::nullopt;
  return std::move(*result);
}

std::vector<std::byte> make_blob(std::string_view text) {
  std::vector<std::byte> v;
  v.reserve(text.size());
  for (char c : text) v.push_back(static_cast<std::byte>(c));
  return v;
}

}  // namespace

// ============================================================================
// WriteEntity
// ============================================================================

TEST(DbappMessages, WriteEntityRoundTrip) {
  WriteEntity msg;
  msg.operation_id[0] = std::byte{1};
  msg.flags = WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn;
  msg.type_id = 7;
  msg.dbid = kInvalidDBID;
  msg.entity_id = 42;
  msg.request_id = 999;
  msg.owner = CheckoutInfo{Address(0x7F000001, 7123), 4, 42, 9876543210};
  msg.shard_table_version = 17;
  msg.identifier = "hero_test";
  msg.blob = make_blob("blob-payload");

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(static_cast<uint8_t>(out->flags),
            static_cast<uint8_t>(WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn));
  EXPECT_EQ(out->type_id, 7u);
  EXPECT_EQ(out->dbid, kInvalidDBID);
  EXPECT_EQ(out->entity_id, 42u);
  EXPECT_EQ(out->request_id, 999u);
  EXPECT_EQ(out->owner.base_addr, msg.owner.base_addr);
  EXPECT_EQ(out->owner.app_id, msg.owner.app_id);
  EXPECT_EQ(out->owner.entity_id, msg.owner.entity_id);
  EXPECT_EQ(out->owner.ownership_version, msg.owner.ownership_version);
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_EQ(out->shard_table_version, 17u);
  EXPECT_EQ(out->identifier, "hero_test");
  EXPECT_EQ(out->blob, msg.blob);
}

TEST(DbappMessages, WriteEntityEmptyBlobAndIdentifier) {
  WriteEntity msg;
  msg.operation_id[0] = std::byte{1};
  msg.type_id = 1;
  msg.dbid = 100;
  msg.request_id = 1;
  // blob and identifier empty

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->blob.empty());
  EXPECT_TRUE(out->identifier.empty());
  EXPECT_EQ(out->dbid, 100);
}

TEST(DbappMessages, WriteEntityAckSuccess) {
  WriteEntityAck msg;
  msg.operation_id[15] = std::byte{0x42};
  msg.request_id = 77;
  msg.success = true;
  msg.dbid = 42;
  msg.owner = CheckoutInfo{Address(0x7F000001, 7100), 7, 13, 123};
  msg.current_shard_table_version = 5;
  msg.error = "";

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->request_id, 77u);
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_TRUE(out->success);
  EXPECT_EQ(out->dbid, 42);
  EXPECT_EQ(out->owner.ownership_version, 123u);
  EXPECT_EQ(out->owner.entity_id, 13u);
  EXPECT_EQ(out->current_shard_table_version, 5u);
  EXPECT_TRUE(out->error.empty());
}

TEST(DbappMessages, WriteEntityAckFailure) {
  WriteEntityAck msg;
  msg.operation_id[15] = std::byte{0x42};
  msg.request_id = 78;
  msg.success = false;
  msg.error_kind = DatabaseErrorKind::kCommitUnknown;
  msg.dbid = kInvalidDBID;
  msg.error = "disk full";

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(out->success);
  EXPECT_EQ(out->error_kind, DatabaseErrorKind::kCommitUnknown);
  EXPECT_EQ(out->error, "disk full");
}

TEST(DbappMessages, WriteEntityAckRejectsInvalidResult) {
  WriteEntityAck msg;
  msg.error_kind = static_cast<DatabaseErrorKind>(255);
  EXPECT_FALSE(round_trip(msg).has_value());
  msg.error_kind = DatabaseErrorKind::kCommitUnknown;
  msg.success = true;
  EXPECT_FALSE(round_trip(msg).has_value());
}

// ============================================================================
// CheckoutEntity
// ============================================================================

TEST(DbappMessages, CheckoutEntityByName) {
  CheckoutEntity msg;
  msg.operation_id[15] = std::byte{0x42};
  msg.mode = LoadMode::kByName;
  msg.type_id = 3;
  msg.dbid = kInvalidDBID;
  msg.identifier = "alice";
  msg.entity_id = 55;
  msg.request_id = 100;
  msg.shard_table_version = 9;
  msg.owner_app_id = 17;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_EQ(out->mode, LoadMode::kByName);
  EXPECT_EQ(out->type_id, 3u);
  EXPECT_EQ(out->identifier, "alice");
  EXPECT_EQ(out->entity_id, 55u);
  EXPECT_EQ(out->request_id, 100u);
  EXPECT_EQ(out->shard_table_version, 9u);
  EXPECT_EQ(out->owner_app_id, 17u);
}

TEST(DbappMessages, CheckoutEntityByDBID) {
  CheckoutEntity msg;
  msg.operation_id[15] = std::byte{0x42};
  msg.mode = LoadMode::kByDbid;
  msg.type_id = 2;
  msg.dbid = 999;
  msg.entity_id = 10;
  msg.request_id = 200;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_EQ(out->mode, LoadMode::kByDbid);
  EXPECT_EQ(out->dbid, 999);
}

// ============================================================================
// CheckoutEntityAck
// ============================================================================

TEST(DbappMessages, CheckoutEntityAckSuccess) {
  CheckoutEntityAck msg;
  msg.request_id = 101;
  msg.status = CheckoutStatus::kSuccess;
  msg.checkout_version = 9999999999;
  msg.dbid = 42;
  msg.blob = make_blob("entity-data");
  msg.holder_addr = Address(0, 0);
  msg.holder_app_id = 0;
  msg.holder_entity_id = 0;
  msg.current_shard_table_version = 4;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->request_id, 101u);
  EXPECT_EQ(out->status, CheckoutStatus::kSuccess);
  EXPECT_EQ(out->checkout_version, 9999999999u);
  EXPECT_EQ(out->dbid, 42);
  EXPECT_EQ(out->current_shard_table_version, 4u);
  EXPECT_EQ(out->blob, msg.blob);
}

TEST(DbappMessages, CheckoutEntityAckAlreadyCheckedOut) {
  CheckoutEntityAck msg;
  msg.request_id = 102;
  msg.status = CheckoutStatus::kAlreadyCheckedOut;
  msg.dbid = 42;
  msg.holder_addr = Address(0x7F000001, 7100);
  msg.holder_app_id = 3;
  msg.holder_entity_id = 101;
  msg.error = "already checked out";

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->status, CheckoutStatus::kAlreadyCheckedOut);
  EXPECT_EQ(out->holder_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(out->holder_addr.Port(), 7100u);
  EXPECT_EQ(out->holder_app_id, 3u);
  EXPECT_EQ(out->error, "already checked out");
}

TEST(DbappMessages, CheckoutUncertainReplyPreservesKindAndOperation) {
  CheckoutEntityAck msg;
  msg.operation_id[3] = std::byte{91};
  msg.request_id = 101;
  msg.status = CheckoutStatus::kDbError;
  msg.error_kind = DatabaseErrorKind::kCommitUnknown;
  msg.error = "commit outcome unknown";
  auto result = round_trip(msg);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->operation_id, msg.operation_id);
  EXPECT_EQ(result->error_kind, DatabaseErrorKind::kCommitUnknown);
}

TEST(DbappMessages, CheckoutReplyRejectsInvalidStatusAndKind) {
  CheckoutEntityAck msg;
  msg.operation_id[0] = std::byte{1};
  msg.status = CheckoutStatus::kDbError;
  msg.error_kind = static_cast<DatabaseErrorKind>(255);
  EXPECT_FALSE(round_trip(msg));
  msg.error_kind = DatabaseErrorKind::kNone;
  msg.status = static_cast<CheckoutStatus>(255);
  EXPECT_FALSE(round_trip(msg));
  msg.status = CheckoutStatus::kSuccess;
  msg.dbid = 42;
  msg.checkout_version = 1;
  msg.error_kind = DatabaseErrorKind::kCommitUnknown;
  EXPECT_FALSE(round_trip(msg));
}

// ============================================================================
// CheckinEntity
// ============================================================================

TEST(DbappMessages, CheckinEntityRoundTrip) {
  CheckinEntity msg;
  msg.request_id = 123;
  msg.type_id = 5;
  msg.dbid = 77;
  msg.owner = CheckoutInfo{Address(0x7F000001, 7100), 5, 7, 11};

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->type_id, 5u);
  EXPECT_EQ(out->dbid, 77);
  EXPECT_EQ(out->owner.base_addr, msg.owner.base_addr);
  EXPECT_EQ(out->owner.ownership_version, 11u);
  EXPECT_EQ(out->request_id, 123u);
  BinaryWriter wire;
  msg.Serialize(wire);
  EXPECT_EQ(wire.Data().size(), CheckinEntity::Descriptor().fixed_length);
  CheckinEntityAck ack{msg, true, {}};
  ASSERT_TRUE(round_trip(ack));
  EXPECT_EQ(round_trip(ack)->request.request_id, 123u);
  BinaryWriter reply;
  ack.Serialize(reply);
  BinaryReader truncated(reply.Data().first(reply.Data().size() - 1));
  EXPECT_FALSE(CheckinEntityAck::Deserialize(truncated));
}

// ============================================================================
// DeleteEntity / DeleteEntityAck
// ============================================================================

TEST(DbappMessages, DeleteEntityRoundTrip) {
  DeleteEntity msg;
  msg.operation_id[0] = std::byte{1};
  msg.owner = CheckoutInfo{Address(0x7F000001, 7100), 7, 13, 19};
  msg.type_id = 1;
  msg.dbid = 50;
  msg.request_id = 300;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_EQ(out->owner.ownership_version, 19u);
  EXPECT_EQ(out->type_id, 1u);
  EXPECT_EQ(out->dbid, 50);
  EXPECT_EQ(out->request_id, 300u);
}

TEST(DbappMessages, DeleteEntityAckRoundTrip) {
  DeleteEntityAck msg;
  msg.request_id = 300;
  msg.success = true;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->success);
  EXPECT_TRUE(out->error.empty());
}

// ============================================================================
// LookupEntity / LookupEntityAck
// ============================================================================

TEST(DbappMessages, LookupEntityRoundTrip) {
  LookupEntity msg;
  msg.type_id = 2;
  msg.identifier = "bob";
  msg.request_id = 400;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->type_id, 2u);
  EXPECT_EQ(out->identifier, "bob");
  EXPECT_EQ(out->request_id, 400u);
}

TEST(DbappMessages, LookupEntityAckFound) {
  LookupEntityAck msg;
  msg.request_id = 400;
  msg.found = true;
  msg.dbid = 88;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->found);
  EXPECT_EQ(out->dbid, 88);
}

TEST(DbappMessages, LookupEntityAckNotFound) {
  LookupEntityAck msg;
  msg.request_id = 401;
  msg.found = false;
  msg.dbid = kInvalidDBID;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(out->found);
  EXPECT_EQ(out->dbid, kInvalidDBID);
}

TEST(DbappMessages, AbortCheckoutRoundTrip) {
  AbortCheckout msg;
  msg.operation_id[15] = std::byte{0x42};
  msg.request_id = 501;
  msg.type_id = 9;
  msg.dbid = 12345;
  msg.mode = LoadMode::kByName;
  msg.identifier = "original";
  msg.entity_id = 77;
  msg.owner_app_id = 9;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->operation_id, msg.operation_id);
  EXPECT_EQ(out->request_id, 501u);
  EXPECT_EQ(out->type_id, 9u);
  EXPECT_EQ(out->dbid, 12345);
  EXPECT_EQ(out->mode, msg.mode);
  EXPECT_EQ(out->identifier, msg.identifier);
  EXPECT_EQ(out->entity_id, msg.entity_id);
  EXPECT_EQ(out->owner_app_id, msg.owner_app_id);
}

TEST(DbappMessages, AbortCheckoutAckRoundTrip) {
  AbortCheckoutAck msg;
  msg.request_id = 501;
  msg.success = true;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->request_id, 501u);
  EXPECT_TRUE(out->success);
}

TEST(DbappMessages, CheckoutRequestsRejectTruncatedOperationAndPayload) {
  CheckoutEntity checkout;
  checkout.operation_id[0] = std::byte{1};
  checkout.dbid = 42;
  checkout.type_id = 1;
  BinaryWriter writer;
  checkout.Serialize(writer);
  for (std::size_t size = 0; size < writer.Data().size(); ++size) {
    BinaryReader reader(writer.Data().first(size));
    EXPECT_FALSE(CheckoutEntity::Deserialize(reader));
  }
  AbortCheckout abort;
  abort.operation_id = checkout.operation_id;
  abort.dbid = 42;
  abort.type_id = 1;
  BinaryWriter abort_writer;
  abort.Serialize(abort_writer);
  for (std::size_t size = 0; size < abort_writer.Data().size(); ++size) {
    BinaryReader reader(abort_writer.Data().first(size));
    EXPECT_FALSE(AbortCheckout::Deserialize(reader));
  }
}

// ============================================================================
// Descriptor IDs
// ============================================================================

TEST(DbappMessages, MessageDescriptorIds) {
  EXPECT_EQ(WriteEntity::Descriptor().id, 4000);
  EXPECT_EQ(WriteEntityAck::Descriptor().id, 4001);
  EXPECT_EQ(CheckoutEntity::Descriptor().id, 4002);
  EXPECT_EQ(CheckoutEntityAck::Descriptor().id, 4003);
  EXPECT_EQ(CheckinEntity::Descriptor().id, 4004);
  EXPECT_EQ(DeleteEntity::Descriptor().id, 4005);
  EXPECT_EQ(DeleteEntityAck::Descriptor().id, 4006);
  EXPECT_EQ(LookupEntity::Descriptor().id, 4007);
  EXPECT_EQ(LookupEntityAck::Descriptor().id, 4008);
  EXPECT_EQ(AbortCheckout::Descriptor().id, 4009);
  EXPECT_EQ(AbortCheckoutAck::Descriptor().id, 4010);
}

TEST(DbappMessages, WriteOperationIdCannotBeTruncated) {
  WriteEntity write;
  write.operation_id[0] = std::byte{1};
  BinaryWriter buffer;
  write.Serialize(buffer);
  BinaryReader incomplete(buffer.Data().first(15));
  EXPECT_FALSE(WriteEntity::Deserialize(incomplete).HasValue());
  WriteEntityAck ack;
  BinaryWriter reply;
  ack.Serialize(reply);
  BinaryReader incomplete_reply(reply.Data().first(15));
  EXPECT_FALSE(WriteEntityAck::Deserialize(incomplete_reply).HasValue());
}

TEST(DbappMessages, ProcessInstanceSurvivesEveryOwnershipMessage) {
  WriteEntity write;
  write.owner.process_instance = 111;
  ASSERT_TRUE(round_trip(write));
  EXPECT_EQ(round_trip(write)->owner.process_instance, 111u);
  WriteEntityAck written;
  written.owner.process_instance = 111;
  ASSERT_TRUE(round_trip(written));
  EXPECT_EQ(round_trip(written)->owner.process_instance, 111u);
  CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  ASSERT_TRUE(round_trip(checkout));
  EXPECT_EQ(round_trip(checkout)->owner_process_instance, 111u);
  CheckoutEntityAck acquired;
  acquired.dbid = 42;
  acquired.checkout_version = 1;
  acquired.holder_process_instance = 111;
  ASSERT_TRUE(round_trip(acquired));
  EXPECT_EQ(round_trip(acquired)->holder_process_instance, 111u);
  CheckinEntity checkin;
  checkin.owner.process_instance = 111;
  ASSERT_TRUE(round_trip(checkin));
  EXPECT_EQ(round_trip(checkin)->owner.process_instance, 111u);
  DeleteEntity removal;
  removal.owner.process_instance = 111;
  ASSERT_TRUE(round_trip(removal));
  EXPECT_EQ(round_trip(removal)->owner.process_instance, 111u);
  AbortCheckout abort;
  abort.owner_process_instance = 111;
  ASSERT_TRUE(round_trip(abort));
  EXPECT_EQ(round_trip(abort)->owner_process_instance, 111u);
  BinaryWriter fixed;
  removal.Serialize(fixed);
  EXPECT_EQ(fixed.Data().size(), DeleteEntity::Descriptor().fixed_length);
}
