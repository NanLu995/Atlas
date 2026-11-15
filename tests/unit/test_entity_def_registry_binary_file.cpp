// Container-file round trip for EntityDefRegistry::RegisterFromBinaryFile.
//
// The container is pure framing around the per-record blobs that
// RegisterStruct / RegisterComponent / RegisterType already accept, so
// the test crafts a buffer in memory, optionally writes it to a temp
// file, and verifies the registry has the entries afterwards. Wire
// format spec lives in entity_def_registry.h.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"
#if defined(ATLAS_EDR_DLL)
#include "entitydef/entitydef_api.h"
#endif

namespace atlas {

namespace {

// ---- Inner-blob helpers (mirror the per-record formats consumed by
// RegisterStruct / RegisterComponent / RegisterType) -------------------

auto MakeStructBlob(uint16_t struct_id, std::string_view name) -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(struct_id);
  w.WriteString(name);
  w.WritePackedInt(1);  // 1 field
  w.WriteString("hp");
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kInt32));
  return w.Detach();
}

auto MakeComponentBlob(uint16_t component_type_id, std::string_view name)
    -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(component_type_id);
  w.WriteString(name);
  w.WriteString("");  // no base
  w.Write<uint8_t>(static_cast<uint8_t>(ComponentLocality::kSynced));
  w.WritePackedInt(0);  // no properties
  return w.Detach();
}

auto MakeMinimalEntityBlob(std::string_view name, uint16_t type_id) -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString(name);
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);  // 0 properties
  w.WritePackedInt(0);  // 0 rpcs
  w.WritePackedInt(0);
  return w.Detach();
}

// Wraps an inner blob in the per-record [PackedInt blob_len][bytes]
// framing the container reader expects.
void AppendRecord(BinaryWriter& w, std::span<const std::byte> blob) {
  w.WritePackedInt(static_cast<uint32_t>(blob.size()));
  w.WriteBytes(blob);
}

// Builds a complete container buffer from the three section blobs.
auto BuildContainer(std::span<const std::vector<std::byte>> structs,
                    std::span<const std::vector<std::byte>> components,
                    std::span<const std::vector<std::byte>> types,
                    uint16_t version = EntityDefRegistry::kBinaryFileVersion,
                    uint32_t magic = EntityDefRegistry::kBinaryFileMagic)
    -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint32_t>(magic);
  w.Write<uint16_t>(version);
  w.Write<uint16_t>(0);  // flags
  // 32-byte digest placeholder; tests don't assert digest contents.
  std::array<std::byte, 32> kZeroDigest{};
  w.WriteBytes(kZeroDigest);

  w.WritePackedInt(static_cast<uint32_t>(structs.size()));
  for (const auto& s : structs) AppendRecord(w, s);

  w.WritePackedInt(static_cast<uint32_t>(components.size()));
  for (const auto& c : components) AppendRecord(w, c);

  w.WritePackedInt(static_cast<uint32_t>(types.size()));
  for (const auto& t : types) AppendRecord(w, t);
  auto checksum = Sha256(w.Data());
  EXPECT_TRUE(checksum.HasValue());
  if (!checksum) return {};
  w.WriteBytes(*checksum);

  return w.Detach();
}

class RegistryBinaryFileTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryBinaryFileTest, EmptyContainerRegistersNothing) {
  // Header-only container — three sections each declaring 0 records.
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(empty, empty, empty);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 0u);
  EXPECT_EQ(result->components, 0u);
  EXPECT_EQ(result->types, 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().ComponentCount(), 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().TypeCount(), 0u);
}

#if defined(ATLAS_EDR_DLL)
TEST_F(RegistryBinaryFileTest, PublicApiExposesNamedAndInlineComponentSlots) {
  auto entity = MakeMinimalEntityBlob("Account", 1);
  entity.pop_back();
  BinaryWriter slots;
  slots.WritePackedInt(2);
  for (uint16_t slot : {uint16_t{1}, uint16_t{2}}) {
    slots.Write(slot);
    slots.WriteString(slot == 1 ? "named" : "inline");
    slots.Write(uint16_t{slot == 1 ? uint16_t{7} : uint16_t{0}});
    slots.Write(static_cast<uint8_t>(ReplicationScope::kAllClients));
    slots.Write(uint8_t{0});
    if (slot == 2) {
      slots.WriteString("InlineStats");
      slots.WritePackedInt(1);
      slots.WriteString("hp");
      slots.Write(static_cast<uint8_t>(PropertyDataType::kInt32));
      slots.Write(static_cast<uint8_t>(ReplicationScope::kAllClients));
      slots.Write(uint8_t{1});
      slots.Write(uint8_t{5});
      slots.Write(uint16_t{0});
      slots.Write(uint8_t{0});
      slots.Write(uint8_t{0});
    }
  }
  entity.insert(entity.end(), slots.Data().begin(), slots.Data().end());
  const std::vector<std::vector<std::byte>> components{MakeComponentBlob(7, "NamedStats")};
  const std::vector<std::vector<std::byte>> entities{entity};
  const auto bytes = BuildContainer({}, components, entities);
  std::unique_ptr<AtlasEdrContext, decltype(&AtlasEdrDestroy)> context(
      AtlasEdrCreate(ATLAS_EDR_ABI_VERSION), &AtlasEdrDestroy);
  ASSERT_NE(context, nullptr);
  ASSERT_EQ(AtlasEdrLoadFromBuffer(context.get(), reinterpret_cast<const uint8_t*>(bytes.data()),
                                   static_cast<int32_t>(bytes.size())),
            ATLAS_EDR_OK);
  const auto* account = AtlasEdrFindEntityByName(context.get(), "Account");
  ASSERT_NE(account, nullptr);
  const auto* named = AtlasEdrEntityComponentAtSlot(context.get(), account, 1);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(AtlasEdrComponentTypeId(named), 7);
  const auto* inlined = AtlasEdrEntityComponentAtSlot(context.get(), account, 2);
  ASSERT_NE(inlined, nullptr);
  EXPECT_STREQ(AtlasEdrComponentName(inlined), "InlineStats");
  EXPECT_EQ(AtlasEdrComponentTypeId(inlined), 0);
  ASSERT_EQ(AtlasEdrComponentPropertyCount(inlined), 1);
  EXPECT_EQ(AtlasEdrPropertyDataType(AtlasEdrComponentPropertyAt(inlined, 0)),
            ATLAS_EDR_TYPE_INT32);
  EXPECT_EQ(AtlasEdrComponentPropertyAt(inlined, -1), nullptr);
  EXPECT_EQ(AtlasEdrComponentPropertyAt(inlined, 1), nullptr);
  EXPECT_EQ(AtlasEdrEntityComponentAtSlot(context.get(), account, 3), nullptr);
  EXPECT_EQ(AtlasEdrEntityComponentAtSlot(nullptr, account, 1), nullptr);
  EXPECT_EQ(AtlasEdrEntityComponentAtSlot(context.get(), nullptr, 1), nullptr);
}

TEST_F(RegistryBinaryFileTest, PublicApiRejectsReloadAndPreservesReturnedViews) {
  const std::vector<std::vector<std::byte>> entities{MakeMinimalEntityBlob("Account", 1)};
  const auto bytes = BuildContainer({}, {}, entities);
  std::unique_ptr<AtlasEdrContext, decltype(&AtlasEdrDestroy)> context(
      AtlasEdrCreate(ATLAS_EDR_ABI_VERSION), &AtlasEdrDestroy);
  ASSERT_NE(context, nullptr);
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  ASSERT_EQ(AtlasEdrLoadFromBuffer(context.get(), data, static_cast<int32_t>(bytes.size())),
            ATLAS_EDR_OK);
  const auto* entity = AtlasEdrFindEntityById(context.get(), 1);
  ASSERT_NE(entity, nullptr);
  ASSERT_EQ(AtlasEdrLoadFromBuffer(context.get(), data, static_cast<int32_t>(bytes.size())),
            ATLAS_EDR_ERR_INVAL);
  EXPECT_EQ(AtlasEdrLoadFromFile(context.get(), "unused.atdf"), ATLAS_EDR_ERR_INVAL);
  EXPECT_STREQ(AtlasEdrEntityName(entity), "Account");
  EXPECT_EQ(AtlasEdrFindEntityById(context.get(), 1), entity);
}

TEST_F(RegistryBinaryFileTest, PublicApiClearsPartialLoadsAndRequiresANewContext) {
  const std::vector<std::vector<std::byte>> entities{MakeMinimalEntityBlob("Account", 1),
                                                     {std::byte{0xff}}};
  const auto bytes = BuildContainer({}, {}, entities);
  std::unique_ptr<AtlasEdrContext, decltype(&AtlasEdrDestroy)> context(
      AtlasEdrCreate(ATLAS_EDR_ABI_VERSION), &AtlasEdrDestroy);
  ASSERT_NE(context, nullptr);
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  ASSERT_EQ(AtlasEdrLoadFromBuffer(context.get(), data, static_cast<int32_t>(bytes.size())),
            ATLAS_EDR_ERR_PARSE);
  EXPECT_EQ(AtlasEdrFindEntityById(context.get(), 1), nullptr);
  EXPECT_EQ(AtlasEdrGetDigest(context.get()), nullptr);
  EXPECT_EQ(AtlasEdrGetDigestSize(context.get()), 0);
  EXPECT_EQ(AtlasEdrLoadFromBuffer(context.get(), data, static_cast<int32_t>(bytes.size())),
            ATLAS_EDR_ERR_INVAL);
}

TEST_F(RegistryBinaryFileTest, PublicApiInvalidArgumentsConsumeTheSingleLoadAttempt) {
  std::unique_ptr<AtlasEdrContext, decltype(&AtlasEdrDestroy)> context(
      AtlasEdrCreate(ATLAS_EDR_ABI_VERSION), &AtlasEdrDestroy);
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(AtlasEdrLoadFromBuffer(context.get(), nullptr, 0), ATLAS_EDR_ERR_INVAL);
  EXPECT_STRNE(AtlasEdrLastError(context.get()), "");
  EXPECT_EQ(AtlasEdrLoadFromFile(context.get(), "unused.atdf"), ATLAS_EDR_ERR_INVAL);
  EXPECT_EQ(AtlasEdrLoadFromFile(nullptr, "unused.atdf"), ATLAS_EDR_ERR_INVAL);
}

TEST_F(RegistryBinaryFileTest, PublicApiMissingFileRequiresANewContext) {
  std::unique_ptr<AtlasEdrContext, decltype(&AtlasEdrDestroy)> context(
      AtlasEdrCreate(ATLAS_EDR_ABI_VERSION), &AtlasEdrDestroy);
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(AtlasEdrLoadFromFile(context.get(), "missing-edr-test.atdf"), ATLAS_EDR_ERR_PARSE);
  EXPECT_EQ(AtlasEdrFindEntityById(context.get(), 1), nullptr);
  EXPECT_EQ(AtlasEdrGetDigest(context.get()), nullptr);
  EXPECT_EQ(AtlasEdrLoadFromFile(context.get(), "missing-edr-test.atdf"), ATLAS_EDR_ERR_INVAL);
}
#endif

TEST_F(RegistryBinaryFileTest, RoundTripAllThreeSections) {
  std::vector<std::vector<std::byte>> structs = {MakeStructBlob(7, "Weapon")};
  std::vector<std::vector<std::byte>> components = {MakeComponentBlob(11, "AbilityComponent")};
  std::vector<std::vector<std::byte>> types = {MakeMinimalEntityBlob("Avatar", 5)};
  auto buf = BuildContainer(structs, components, types);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 1u);
  EXPECT_EQ(result->components, 1u);
  EXPECT_EQ(result->types, 1u);

  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(7), nullptr);
  EXPECT_NE(EntityDefRegistry::Instance().FindComponentById(11), nullptr);
  EXPECT_NE(EntityDefRegistry::Instance().FindByName("Avatar"), nullptr);
}

TEST_F(RegistryBinaryFileTest, MultipleRecordsPerSection) {
  std::vector<std::vector<std::byte>> structs = {
      MakeStructBlob(1, "A"),
      MakeStructBlob(2, "B"),
      MakeStructBlob(3, "C"),
  };
  std::vector<std::vector<std::byte>> components = {
      MakeComponentBlob(10, "X"),
      MakeComponentBlob(20, "Y"),
  };
  std::vector<std::vector<std::byte>> types = {
      MakeMinimalEntityBlob("E1", 1),
      MakeMinimalEntityBlob("E2", 2),
  };
  auto buf = BuildContainer(structs, components, types);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 3u);
  EXPECT_EQ(result->components, 2u);
  EXPECT_EQ(result->types, 2u);
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 3u);
  EXPECT_EQ(EntityDefRegistry::Instance().ComponentCount(), 2u);
  EXPECT_EQ(EntityDefRegistry::Instance().TypeCount(), 2u);
}

TEST_F(RegistryBinaryFileTest, BadMagicRejected) {
  std::vector<std::vector<std::byte>> empty;
  // Wrong magic ('XYZW' little-endian) should fail without touching state.
  auto buf = BuildContainer(empty, empty, empty,
                            /*version=*/EntityDefRegistry::kBinaryFileVersion,
                            /*magic=*/0x57'5A'59'58u);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
}

TEST_F(RegistryBinaryFileTest, UnsupportedVersionRejected) {
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(empty, empty, empty, /*version=*/999);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
}

TEST_F(RegistryBinaryFileTest, UnknownFlagsAreRejected) {
  std::vector<std::vector<std::byte>> empty;
  auto buffer = BuildContainer(empty, empty, empty);
  buffer[6] = std::byte{1};
  EXPECT_FALSE(EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buffer));
}

TEST_F(RegistryBinaryFileTest, TrailingDataIsRejected) {
  std::vector<std::vector<std::byte>> empty;
  const std::vector<std::vector<std::byte>> types{MakeMinimalEntityBlob("Account", 1)};
  auto buffer = BuildContainer(empty, empty, types);
  buffer.push_back(std::byte{0xff});
  auto& registry = EntityDefRegistry::Instance();
  EXPECT_FALSE(registry.RegisterFromBinaryBuffer(buffer));
}

TEST_F(RegistryBinaryFileTest, TrailingRecordDataIsRejected) {
  std::vector<std::vector<std::byte>> empty;
  std::vector<std::vector<std::byte>> structs{MakeStructBlob(1, "Payload")};
  structs[0].push_back(std::byte{0xff});
  auto& registry = EntityDefRegistry::Instance();
  EXPECT_FALSE(registry.RegisterFromBinaryBuffer(BuildContainer(structs, empty, empty)));
  EXPECT_EQ(registry.StructCount(), 0u);
}

TEST_F(RegistryBinaryFileTest, DuplicateEntityTypesReturnAnError) {
  std::vector<std::vector<std::byte>> empty;
  const auto type = MakeMinimalEntityBlob("Account", 1);
  const std::vector<std::vector<std::byte>> types{type, type};
  auto result =
      EntityDefRegistry::Instance().RegisterFromBinaryBuffer(BuildContainer(empty, empty, types));
  ASSERT_FALSE(result.HasValue());
  EXPECT_NE(result.Error().Message().find("type"), std::string_view::npos);
}

TEST_F(RegistryBinaryFileTest, CorruptionAndEveryTruncatedPrefixAreRejectedBeforeRegistration) {
  std::vector<std::vector<std::byte>> empty;
  const std::vector<std::vector<std::byte>> types{MakeMinimalEntityBlob("Account", 1)};
  const auto file = BuildContainer(empty, empty, types);
  auto& registry = EntityDefRegistry::Instance();
  for (std::size_t index = 0; index < file.size(); ++index) {
    SCOPED_TRACE(index);
    auto damaged = file;
    damaged[index] ^= std::byte{0x80};
    EXPECT_FALSE(registry.RegisterFromBinaryBuffer(damaged));
    EXPECT_EQ(registry.TypeCount(), 0u);
    EXPECT_FALSE(registry.RegisterFromBinaryBuffer(std::span(file).first(index)));
    EXPECT_EQ(registry.TypeCount(), 0u);
  }
}

TEST_F(RegistryBinaryFileTest, MalformedRecordStopsAtFirstFailure) {
  // A struct blob short by one byte — RegisterStruct returns false, the
  // container reader bubbles that up. Subsequent sections aren't reached
  // so registry sees only the records loaded before the failure.
  auto good_struct = MakeStructBlob(1, "Good");
  auto truncated = good_struct;
  truncated.pop_back();  // drop the last field-type byte

  std::vector<std::vector<std::byte>> structs = {good_struct, truncated};
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(structs, empty, empty);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
  // Good record landed before the bad one.
  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(1), nullptr);
}

TEST_F(RegistryBinaryFileTest, FilePathRoundTrip) {
  // Same payload via the file-path overload — covers the I/O wrapper
  // separately from the buffer parser.
  std::vector<std::vector<std::byte>> structs = {MakeStructBlob(42, "FileWeapon")};
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(structs, empty, empty);

  auto path = std::filesystem::temp_directory_path() / "atlas_defdump_round_trip.bin";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f) << "cannot open temp file";
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  }

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryFile(path);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 1u);
  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(42), nullptr);

  std::filesystem::remove(path);
}

TEST_F(RegistryBinaryFileTest, NonexistentFileFails) {
  auto path = std::filesystem::temp_directory_path() / "atlas_defdump_does_not_exist.bin";
  std::filesystem::remove(path);  // ensure absent
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryFile(path);
  EXPECT_FALSE(result);
}

}  // namespace atlas
