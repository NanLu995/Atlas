#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {
namespace {

auto Type(PropertyDataType kind) -> DataTypeRef {
  DataTypeRef type;
  type.kind = kind;
  return type;
}

auto MakeSnapshotSchema() -> EntityDefRegistry {
  EntityDefRegistry registry;
  StructDescriptor payload;
  payload.id = 7;
  payload.name = "Payload";
  payload.fields = {{"amount", Type(PropertyDataType::kInt32)},
                    {"label", Type(PropertyDataType::kString)}};
  registry.struct_id_index.emplace(payload.id, 0);
  registry.struct_name_index.emplace(payload.name, 0);
  registry.structs.push_back(std::move(payload));

  PropertyDescriptor values;
  values.name = "values";
  values.data_type = PropertyDataType::kList;
  values.scope = ReplicationScope::kBase;
  values.persistent = true;
  auto dictionary = Type(PropertyDataType::kDict);
  dictionary.key = std::make_shared<DataTypeRef>(Type(PropertyDataType::kString));
  dictionary.elem = std::make_shared<DataTypeRef>(Type(PropertyDataType::kStruct));
  dictionary.elem->struct_id = 7;
  values.type_ref = Type(PropertyDataType::kList);
  values.type_ref->elem = std::make_shared<DataTypeRef>(std::move(dictionary));

  PropertyDescriptor scratch;
  scratch.name = "scratch";
  scratch.data_type = PropertyDataType::kInt32;
  scratch.scope = ReplicationScope::kBase;
  scratch.index = 1;
  ComponentDescriptor parent;
  parent.name = "Stats";
  parent.component_type_id = 3;
  parent.properties = {scratch};
  ComponentDescriptor component;
  component.name = "Inventory";
  component.component_type_id = 4;
  component.base_name = parent.name;
  component.properties = {values};
  registry.component_id_index = {{3, 0}, {4, 1}};
  registry.component_name_index = {{parent.name, 0}, {component.name, 1}};
  registry.components = {parent, component};

  EntityTypeDescriptor entity;
  entity.name = "Account";
  entity.type_id = 1;
  entity.properties = {values, scratch};
  entity.slots = {{1, "inventory", 4, ReplicationScope::kBase, false}};
  registry.types.push_back(std::move(entity));
  registry.name_index = {{"Account", 0}};
  registry.id_index = {{1, 0}};
  return registry;
}

auto DigestOf(const EntityDefRegistry& registry) -> Sha256Digest {
  auto digest = registry.StorageSchemaDigest();
  EXPECT_TRUE(digest.HasValue());
  return digest ? *digest : Sha256Digest{};
}

TEST(PersistenceSchema, NestedDictionaryKeyAndValueAreIncluded) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  registry.types[0].properties[0].type_ref->elem->key->kind = PropertyDataType::kUInt64;
  EXPECT_NE(original, DigestOf(registry));
  registry = MakeSnapshotSchema();
  registry.types[0].properties[0].type_ref->elem->elem =
      std::make_shared<DataTypeRef>(Type(PropertyDataType::kUInt64));
  EXPECT_NE(original, DigestOf(registry));
}

TEST(PersistenceSchema, StructFieldsAndTheirOrderAreIncluded) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  registry.structs[0].fields[0].type.kind = PropertyDataType::kInt64;
  EXPECT_NE(original, DigestOf(registry));
  registry = MakeSnapshotSchema();
  std::swap(registry.structs[0].fields[0], registry.structs[0].fields[1]);
  EXPECT_NE(original, DigestOf(registry));
}

TEST(PersistenceSchema, SnapshotFieldsIncludeNonPersistentPropertiesAndScopes) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  registry.types[0].properties[1].data_type = PropertyDataType::kInt64;
  EXPECT_NE(original, DigestOf(registry));
  registry = MakeSnapshotSchema();
  registry.types[0].properties[0].scope = ReplicationScope::kCellPrivate;
  EXPECT_NE(original, DigestOf(registry));
}

TEST(PersistenceSchema, ComponentInheritanceAndSlotBindingsAreIncluded) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  registry.components[0].properties[0].data_type = PropertyDataType::kInt64;
  EXPECT_NE(original, DigestOf(registry));
  registry = MakeSnapshotSchema();
  registry.types[0].slots[0].component_type_id = 3;
  EXPECT_NE(original, DigestOf(registry));
}

TEST(PersistenceSchema, LengthPrefixedNamesCannotShareAnEncoding) {
  auto registry = MakeSnapshotSchema();
  registry.types[0].name = "AB";
  registry.types[0].properties[0].name = "C";
  const auto original = DigestOf(registry);
  registry.types[0].name = "A";
  registry.types[0].properties[0].name = "BC";
  EXPECT_NE(original, DigestOf(registry));
}

TEST(PersistenceSchema, RpcChangesDoNotChangeTheStoredLayout) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  RpcDescriptor rpc;
  rpc.name = "Ping";
  rpc.rpc_id = 1;
  registry.types[0].rpcs.push_back(std::move(rpc));
  EXPECT_EQ(original, DigestOf(registry));
}

TEST(PersistenceSchema, RegistrationOrderDoesNotChangeTheDigest) {
  auto registry = MakeSnapshotSchema();
  const auto original = DigestOf(registry);
  std::swap(registry.components[0], registry.components[1]);
  registry.component_id_index = {{3, 1}, {4, 0}};
  registry.component_name_index = {{"Stats", 1}, {"Inventory", 0}};
  EXPECT_EQ(original, DigestOf(registry));
}

TEST(PersistenceSchema, InvalidAndUnresolvedTypesReturnAnError) {
  auto registry = MakeSnapshotSchema();
  registry.types[0].properties[0].type_ref->elem->elem->struct_id = 99;
  EXPECT_FALSE(registry.StorageSchemaDigest());
  registry = MakeSnapshotSchema();
  registry.types[0].properties[0].type_ref->elem.reset();
  EXPECT_FALSE(registry.StorageSchemaDigest());
  registry = MakeSnapshotSchema();
  registry.types[0].slots[0].component_type_id = 99;
  EXPECT_FALSE(registry.StorageSchemaDigest());
  registry = MakeSnapshotSchema();
  registry.components[1].base_name = "Missing";
  EXPECT_FALSE(registry.StorageSchemaDigest());
}

TEST(PersistenceSchema, InlineComponentFieldsAreDecodedAndChangeTheStorageDigest) {
  BinaryWriter writer;
  writer.WriteString("Account");
  writer.Write(uint16_t{1});
  writer.Write(uint8_t{1});
  writer.Write(uint8_t{1});
  writer.WritePackedInt(0);
  writer.WritePackedInt(0);
  writer.WritePackedInt(1);
  writer.Write(uint16_t{1});
  writer.WriteString("inventory");
  writer.Write(uint16_t{0});
  writer.Write(static_cast<uint8_t>(ReplicationScope::kBase));
  writer.Write(uint8_t{0});
  writer.WriteString("Inventory");
  writer.WritePackedInt(1);
  writer.WriteString("values");
  writer.Write(static_cast<uint8_t>(PropertyDataType::kList));
  writer.Write(static_cast<uint8_t>(ReplicationScope::kBase));
  writer.Write(uint8_t{1});
  writer.Write(uint8_t{5});
  writer.Write(uint16_t{0});
  writer.Write(uint8_t{0});
  writer.Write(uint8_t{0});
  writer.Write(static_cast<uint8_t>(PropertyDataType::kInt32));
  writer.WritePackedInt(64);
  auto bytes = writer.Detach();
  EntityDefRegistry registry;
  ASSERT_TRUE(registry.RegisterType(bytes.data(), static_cast<int32_t>(bytes.size())));
  const auto before = DigestOf(registry);
  auto& slot = registry.types[0].slots[0];
  ASSERT_EQ(slot.inline_component.name, "Inventory");
  ASSERT_EQ(slot.inline_component.properties.size(), 1u);
  EXPECT_EQ(slot.inline_component.properties[0].max_size, 64u);
  slot.inline_component.properties[0].type_ref->elem->kind = PropertyDataType::kInt64;
  EXPECT_NE(before, DigestOf(registry));
  bytes.pop_back();
  EntityDefRegistry truncated;
  EXPECT_FALSE(truncated.RegisterType(bytes.data(), static_cast<int32_t>(bytes.size())));
  EXPECT_EQ(truncated.TypeCount(), 0u);
}

}  // namespace
}  // namespace atlas
