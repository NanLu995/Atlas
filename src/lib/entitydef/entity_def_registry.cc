#include "entitydef/entity_def_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <span>
#include <string>

#include "foundation/log.h"
#include "serialization/binary_stream.h"

namespace atlas {

// ReadDataTypeRef reads [kind byte][body]; ReadDataTypeRefBody reads only
// the body because container records already carry the top-level kind.
namespace {
auto ReadDataTypeRef(BinaryReader& reader, std::size_t depth) -> std::optional<DataTypeRef>;
auto ReadDataTypeRefBody(BinaryReader& reader, PropertyDataType kind, std::size_t depth)
    -> std::optional<DataTypeRef>;

constexpr uint32_t kRpcReplyBit = 1u << 31;

[[nodiscard]] constexpr uint32_t CanonicalRpcId(uint32_t rpc_id) {
  return rpc_id & ~kRpcReplyBit;
}

// RegisterType and RegisterComponent emit identical property records; keeping
// one reader prevents the two paths from drifting.
bool ReadPropertyRecord(BinaryReader& reader, PropertyDescriptor& prop, std::string_view owner) {
  auto prop_name = reader.ReadString();
  auto data_type = reader.Read<uint8_t>();
  auto scope = reader.Read<uint8_t>();
  auto persistent = reader.Read<uint8_t>();
  auto detail_level = reader.Read<uint8_t>();
  auto index = reader.Read<uint16_t>();

  if (!prop_name || !data_type || !scope || !persistent || !detail_level || !index) {
    ATLAS_LOG_ERROR("read_property: header read failed for '{}'", owner);
    return false;
  }

  prop.name = std::move(*prop_name);
  prop.data_type = static_cast<PropertyDataType>(*data_type);
  if (prop.data_type == PropertyDataType::kInvalid) {
    ATLAS_LOG_ERROR("read_property: '{}.{}' has kInvalid data_type — generator bug", owner,
                    prop.name);
    return false;
  }
  prop.scope = static_cast<ReplicationScope>(*scope);
  prop.persistent = *persistent != 0;
  prop.detail_level = *detail_level;
  prop.index = *index;

  auto id_flag = reader.Read<uint8_t>();
  auto reliable_flag = reader.Read<uint8_t>();
  if (!id_flag || !reliable_flag) {
    ATLAS_LOG_ERROR("read_property: flags read failed for '{}.{}'", owner, prop.name);
    return false;
  }
  prop.identifier = *id_flag != 0;
  prop.reliable = *reliable_flag != 0;

  // Container records carry `[DataTypeRef body] [PackedInt max_size]`; the
  // body skips its leading kind byte because prop.data_type already has it.
  const bool is_container = prop.data_type == PropertyDataType::kList ||
                            prop.data_type == PropertyDataType::kDict ||
                            prop.data_type == PropertyDataType::kStruct;
  if (is_container) {
    auto type_ref = ReadDataTypeRefBody(reader, prop.data_type, /*depth=*/0);
    if (!type_ref) {
      ATLAS_LOG_ERROR("read_property: type_ref read failed for '{}.{}'", owner, prop.name);
      return false;
    }
    prop.type_ref = std::move(*type_ref);

    auto max_size = reader.ReadPackedInt();
    if (!max_size) {
      ATLAS_LOG_ERROR("read_property: max_size read failed for '{}.{}'", owner, prop.name);
      return false;
    }
    prop.max_size = *max_size;
  }

  if (!ValidatePropertyInvariant(prop)) {
    ATLAS_LOG_ERROR(
        "read_property: '{}.{}' fails data_type/type_ref invariant "
        "(data_type={}, type_ref={})",
        owner, prop.name, static_cast<int>(prop.data_type),
        prop.type_ref.has_value() ? "present" : "absent");
    return false;
  }
  return true;
}
}  // namespace

EntityDefRegistry& EntityDefRegistry::Instance() {
  static EntityDefRegistry registry;
  return registry;
}

bool EntityDefRegistry::RegisterType(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_type: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  EntityTypeDescriptor desc;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_type: failed to read type name");
    return false;
  }
  desc.name = std::move(*name_result);

  auto type_id_result = reader.Read<uint16_t>();
  if (!type_id_result) {
    ATLAS_LOG_ERROR("register_type: failed to read type_id for '{}'", desc.name);
    return false;
  }
  desc.type_id = *type_id_result;

  auto has_cell_result = reader.Read<uint8_t>();
  auto has_client_result = reader.Read<uint8_t>();
  if (!has_cell_result || !has_client_result) {
    ATLAS_LOG_ERROR("register_type: failed to read flags for '{}'", desc.name);
    return false;
  }
  desc.has_cell = *has_cell_result != 0;
  desc.has_client = *has_client_result != 0;

  auto prop_count_result = reader.ReadPackedInt();
  if (!prop_count_result) {
    ATLAS_LOG_ERROR("register_type: failed to read property count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *prop_count_result; ++i) {
    PropertyDescriptor prop;
    if (!ReadPropertyRecord(reader, prop, desc.name)) {
      ATLAS_LOG_ERROR("register_type: failed to read property {} for '{}'", i, desc.name);
      return false;
    }
    desc.properties.push_back(std::move(prop));
  }

  auto rpc_count_result = reader.ReadPackedInt();
  if (!rpc_count_result) {
    ATLAS_LOG_ERROR("register_type: failed to read RPC count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *rpc_count_result; ++i) {
    RpcDescriptor rpc;
    auto rpc_name = reader.ReadString();
    auto rpc_id = reader.ReadPackedInt();
    auto param_count = reader.ReadPackedInt();

    if (!rpc_name || !rpc_id || !param_count) {
      ATLAS_LOG_ERROR("register_type: failed to read RPC {} for '{}'", i, desc.name);
      return false;
    }

    rpc.name = std::move(*rpc_name);
    rpc.rpc_id = *rpc_id;

    for (uint32_t j = 0; j < *param_count; ++j) {
      auto pt = reader.Read<uint8_t>();
      if (!pt) {
        ATLAS_LOG_ERROR("register_type: failed to read RPC param type");
        return false;
      }
      rpc.param_types.push_back(static_cast<PropertyDataType>(*pt));
    }

    auto exposed_val = reader.Read<uint8_t>();
    if (exposed_val) {
      rpc.exposed = static_cast<ExposedScope>(*exposed_val);
    }

    desc.rpcs.push_back(std::move(rpc));
  }

  // Any remaining bytes are the optional component slot section.
  if (reader.Remaining() > 0) {
    auto slot_count = reader.ReadPackedInt();
    if (!slot_count) {
      ATLAS_LOG_ERROR("register_type: failed to read slot count for '{}'", desc.name);
      return false;
    }
    for (uint32_t i = 0; i < *slot_count; ++i) {
      EntitySlotDescriptor slot;
      auto slot_idx = reader.Read<uint16_t>();
      auto slot_name = reader.ReadString();
      auto component_type_id = reader.Read<uint16_t>();
      auto slot_scope = reader.Read<uint8_t>();
      auto lazy = reader.Read<uint8_t>();
      if (!slot_idx || !slot_name || !component_type_id || !slot_scope || !lazy) {
        ATLAS_LOG_ERROR("register_type: failed to read slot {} for '{}'", i, desc.name);
        return false;
      }
      slot.slot_idx = *slot_idx;
      slot.slot_name = std::move(*slot_name);
      slot.component_type_id = *component_type_id;
      slot.scope = static_cast<ReplicationScope>(*slot_scope);
      slot.lazy = *lazy != 0;
      if (slot.component_type_id == 0) {
        auto type_name = reader.ReadString();
        auto count = reader.ReadPackedInt();
        if (!type_name || type_name->empty() || !count) return false;
        slot.inline_component.name = std::move(*type_name);
        for (uint32_t field = 0; field < *count; ++field) {
          PropertyDescriptor property;
          if (!ReadPropertyRecord(reader, property, slot.inline_component.name)) return false;
          slot.inline_component.properties.push_back(std::move(property));
        }
      }
      desc.slots.push_back(std::move(slot));
    }
  }

  if (reader.Remaining() > 0) {
    ATLAS_LOG_ERROR("register_type: {} trailing bytes after '{}' descriptor", reader.Remaining(),
                    desc.name);
    return false;
  }

  // Hard guard: cross-assembly id/name collision can corrupt persistence
  // and routing — fail loud instead of silently overwriting the prior entry.
  if (auto it = name_index.find(desc.name); it != name_index.end()) {
    auto msg = std::format("entity '{}' already registered (prev id={}); cross-assembly conflict",
                           desc.name, types[it->second].type_id);
    DefaultAssertHandler("EntityDefRegistry::RegisterType: duplicate name", msg,
                         std::source_location::current());
  }
  if (auto it = id_index.find(desc.type_id); it != id_index.end()) {
    auto msg = std::format("type_id {} already registered (prev name='{}', new name='{}')",
                           desc.type_id, types[it->second].name, desc.name);
    DefaultAssertHandler("EntityDefRegistry::RegisterType: duplicate id", msg,
                         std::source_location::current());
  }

  // Store
  auto idx = types.size();
  auto& type_name = desc.name;
  auto type_id = desc.type_id;

  ATLAS_LOG_INFO("Registered entity type '{}' (id={}, {} props, {} rpcs)", type_name, type_id,
                 desc.properties.size(), desc.rpcs.size());

  for (const auto& rpc : desc.rpcs) {
    rpc_to_type[rpc.rpc_id] = idx;
  }

  name_index[type_name] = idx;
  id_index[type_id] = idx;
  types.push_back(std::move(desc));

  return true;
}

const EntityTypeDescriptor* EntityDefRegistry::FindByName(std::string_view name) const {
  auto it = name_index.find(std::string(name));
  if (it == name_index.end()) return nullptr;
  return &types[it->second];
}

const EntityTypeDescriptor* EntityDefRegistry::FindById(uint16_t type_id) const {
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return nullptr;
  return &types[it->second];
}

bool EntityDefRegistry::ValidateRpc(uint16_t type_id, uint32_t rpc_id) const {
  const auto canonical_id = CanonicalRpcId(rpc_id);
  auto it = rpc_to_type.find(canonical_id);
  if (it == rpc_to_type.end()) return false;
  return types[it->second].type_id == type_id;
}

const RpcDescriptor* EntityDefRegistry::FindRpc(uint32_t rpc_id) const {
  const auto canonical_id = CanonicalRpcId(rpc_id);
  auto it = rpc_to_type.find(canonical_id);
  if (it == rpc_to_type.end()) return nullptr;
  for (const auto& rpc : types[it->second].rpcs) {
    if (rpc.rpc_id == canonical_id) return &rpc;
  }
  return nullptr;
}

bool EntityDefRegistry::IsExposed(uint32_t rpc_id) const {
  auto* rpc = FindRpc(rpc_id);
  return rpc != nullptr && rpc->IsExposed();
}

ExposedScope EntityDefRegistry::GetExposedScope(uint32_t rpc_id) const {
  auto* rpc = FindRpc(rpc_id);
  return rpc ? rpc->exposed : ExposedScope::kNone;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::GetReplicatedProperties(
    uint16_t type_id, ReplicationScope min_scope) const {
  std::vector<const PropertyDescriptor*> result;
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return result;

  for (const auto& prop : types[it->second].properties) {
    if (static_cast<uint8_t>(prop.scope) >= static_cast<uint8_t>(min_scope)) {
      result.push_back(&prop);
    }
  }
  return result;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::GetPersistentProperties(
    uint16_t type_id) const {
  std::vector<const PropertyDescriptor*> result;
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return result;

  for (const auto& prop : types[it->second].properties) {
    if (prop.persistent) {
      result.push_back(&prop);
    }
  }
  return result;
}

void EntityDefRegistry::clear() {
  types.clear();
  name_index.clear();
  id_index.clear();
  rpc_to_type.clear();
  structs.clear();
  struct_id_index.clear();
  struct_name_index.clear();
  components.clear();
  component_id_index.clear();
  component_name_index.clear();
  entity_def_digest_.fill(0);
  ATLAS_LOG_INFO("EntityDefRegistry cleared");
}

void EntityDefRegistry::SetDigest(const std::byte* data, int32_t len) {
  if (data == nullptr || len != static_cast<int32_t>(kDigestSize)) {
    ATLAS_LOG_ERROR("SetDigest: expected {} bytes, got {}", kDigestSize, len);
    return;
  }
  std::memcpy(entity_def_digest_.data(), data, kDigestSize);
}

namespace {

auto ReadDataTypeRefBody(BinaryReader& reader, PropertyDataType kind, std::size_t depth)
    -> std::optional<DataTypeRef> {
  if (depth > kMaxDataTypeDepth) {
    ATLAS_LOG_ERROR("data_type_ref: nesting exceeds max depth {}", kMaxDataTypeDepth);
    return std::nullopt;
  }

  DataTypeRef out;
  out.kind = kind;

  switch (kind) {
    case PropertyDataType::kBool:
    case PropertyDataType::kInt8:
    case PropertyDataType::kUInt8:
    case PropertyDataType::kInt16:
    case PropertyDataType::kUInt16:
    case PropertyDataType::kInt32:
    case PropertyDataType::kUInt32:
    case PropertyDataType::kInt64:
    case PropertyDataType::kUInt64:
    case PropertyDataType::kFloat:
    case PropertyDataType::kDouble:
    case PropertyDataType::kString:
    case PropertyDataType::kBytes:
    case PropertyDataType::kVector3:
    case PropertyDataType::kQuaternion:
    case PropertyDataType::kCustom:
      // Scalar: no trailing data.
      return out;

    case PropertyDataType::kList: {
      auto elem = ReadDataTypeRef(reader, depth + 1);
      if (!elem) return std::nullopt;
      out.elem = std::make_shared<DataTypeRef>(std::move(*elem));
      return out;
    }

    case PropertyDataType::kDict: {
      auto key = ReadDataTypeRef(reader, depth + 1);
      if (!key) return std::nullopt;
      auto val = ReadDataTypeRef(reader, depth + 1);
      if (!val) return std::nullopt;
      out.key = std::make_shared<DataTypeRef>(std::move(*key));
      out.elem = std::make_shared<DataTypeRef>(std::move(*val));
      return out;
    }

    case PropertyDataType::kStruct: {
      auto sid = reader.Read<uint16_t>();
      if (!sid) {
        ATLAS_LOG_ERROR("data_type_ref: kStruct missing struct_id");
        return std::nullopt;
      }
      out.struct_id = *sid;
      return out;
    }

    case PropertyDataType::kInvalid:
      ATLAS_LOG_ERROR("data_type_ref: kInvalid kind");
      return std::nullopt;
  }

  ATLAS_LOG_ERROR("data_type_ref: unknown kind {}", static_cast<int>(kind));
  return std::nullopt;
}

auto ReadDataTypeRef(BinaryReader& reader, std::size_t depth) -> std::optional<DataTypeRef> {
  auto kind_byte = reader.Read<uint8_t>();
  if (!kind_byte) {
    ATLAS_LOG_ERROR("data_type_ref: failed to read kind byte");
    return std::nullopt;
  }
  return ReadDataTypeRefBody(reader, static_cast<PropertyDataType>(*kind_byte), depth);
}

}  // namespace

bool EntityDefRegistry::RegisterStruct(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_struct: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  auto sid_result = reader.Read<uint16_t>();
  if (!sid_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read struct_id");
    return false;
  }
  const uint16_t sid = *sid_result;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read name (struct_id={})", sid);
    return false;
  }

  StructDescriptor desc;
  desc.id = sid;
  desc.name = std::move(*name_result);

  auto field_count_result = reader.ReadPackedInt();
  if (!field_count_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read field count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *field_count_result; ++i) {
    FieldDescriptor field;
    auto fname = reader.ReadString();
    if (!fname) {
      ATLAS_LOG_ERROR("register_struct: failed to read field {} name for '{}'", i, desc.name);
      return false;
    }
    field.name = std::move(*fname);
    auto type = ReadDataTypeRef(reader, /*depth=*/0);
    if (!type) {
      ATLAS_LOG_ERROR("register_struct: failed to read field '{}' type for '{}'", field.name,
                      desc.name);
      return false;
    }
    field.type = std::move(*type);
    desc.fields.push_back(std::move(field));
  }

  if (reader.Remaining() > 0) {
    ATLAS_LOG_ERROR("register_struct: {} trailing bytes after '{}'", reader.Remaining(), desc.name);
    return false;
  }

  // Same (id, name) may be re-registered; mismatched pairs are generator bugs.
  if (auto it = struct_id_index.find(desc.id); it != struct_id_index.end()) {
    if (structs[it->second].name != desc.name) {
      ATLAS_LOG_ERROR("register_struct: id {} already bound to '{}', rejecting '{}'", desc.id,
                      structs[it->second].name, desc.name);
      return false;
    }
    ATLAS_LOG_WARNING("register_struct: replacing existing struct '{}' (id={})", desc.name,
                      desc.id);
    structs[it->second] = std::move(desc);
    return true;
  }
  if (auto it = struct_name_index.find(desc.name); it != struct_name_index.end()) {
    if (structs[it->second].id != desc.id) {
      ATLAS_LOG_ERROR("register_struct: name '{}' already bound to id {}, rejecting id {}",
                      desc.name, structs[it->second].id, desc.id);
      return false;
    }
    // A matching (id, name) pair returns in the id branch above.
    // Only id-new/name-duplicate reaches here, and that is rejected.
  }

  auto idx = structs.size();
  struct_id_index[desc.id] = idx;
  struct_name_index[desc.name] = idx;
  ATLAS_LOG_INFO("Registered struct '{}' (id={}, {} fields)", desc.name, desc.id,
                 desc.fields.size());
  structs.push_back(std::move(desc));
  return true;
}

const StructDescriptor* EntityDefRegistry::FindStructById(uint16_t struct_id) const {
  auto it = struct_id_index.find(struct_id);
  if (it == struct_id_index.end()) return nullptr;
  return &structs[it->second];
}

const StructDescriptor* EntityDefRegistry::FindStructByName(std::string_view name) const {
  auto it = struct_name_index.find(std::string(name));
  if (it == struct_name_index.end()) return nullptr;
  return &structs[it->second];
}

bool EntityDefRegistry::RegisterComponent(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_component: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  ComponentDescriptor desc;

  auto type_id_result = reader.Read<uint16_t>();
  if (!type_id_result) {
    ATLAS_LOG_ERROR("register_component: failed to read component_type_id");
    return false;
  }
  desc.component_type_id = *type_id_result;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_component: failed to read name (id={})", desc.component_type_id);
    return false;
  }
  desc.name = std::move(*name_result);

  // Empty base_name means no inheritance; concrete resolution happens later.
  // A startup batch may register the base after this descriptor.
  auto base_name_result = reader.ReadString();
  if (!base_name_result) {
    ATLAS_LOG_ERROR("register_component: failed to read base_name for '{}'", desc.name);
    return false;
  }
  desc.base_name = std::move(*base_name_result);

  auto locality_result = reader.Read<uint8_t>();
  if (!locality_result) {
    ATLAS_LOG_ERROR("register_component: failed to read locality for '{}'", desc.name);
    return false;
  }
  desc.locality = static_cast<ComponentLocality>(*locality_result);

  auto prop_count_result = reader.ReadPackedInt();
  if (!prop_count_result) {
    ATLAS_LOG_ERROR("register_component: failed to read property count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *prop_count_result; ++i) {
    PropertyDescriptor prop;
    if (!ReadPropertyRecord(reader, prop, desc.name)) {
      ATLAS_LOG_ERROR("register_component: failed to read property {} for '{}'", i, desc.name);
      return false;
    }
    desc.properties.push_back(std::move(prop));
  }

  if (reader.Remaining() > 0) {
    ATLAS_LOG_ERROR("register_component: {} trailing bytes after '{}'", reader.Remaining(),
                    desc.name);
    return false;
  }

  // Same (id, name) may be re-registered; mismatched pairs are generator bugs.
  if (auto it = component_id_index.find(desc.component_type_id); it != component_id_index.end()) {
    if (components[it->second].name != desc.name) {
      ATLAS_LOG_ERROR("register_component: id {} already bound to '{}', rejecting '{}'",
                      desc.component_type_id, components[it->second].name, desc.name);
      return false;
    }
    ATLAS_LOG_WARNING("register_component: replacing existing component '{}' (id={})", desc.name,
                      desc.component_type_id);
    components[it->second] = std::move(desc);
    return true;
  }
  if (auto it = component_name_index.find(desc.name); it != component_name_index.end()) {
    if (components[it->second].component_type_id != desc.component_type_id) {
      ATLAS_LOG_ERROR("register_component: name '{}' already bound to id {}, rejecting id {}",
                      desc.name, components[it->second].component_type_id, desc.component_type_id);
      return false;
    }
  }

  auto idx = components.size();
  component_id_index[desc.component_type_id] = idx;
  component_name_index[desc.name] = idx;
  ATLAS_LOG_INFO("Registered component '{}' (id={}, base='{}', {} props, locality={})", desc.name,
                 desc.component_type_id, desc.base_name, desc.properties.size(),
                 static_cast<int>(desc.locality));
  components.push_back(std::move(desc));
  return true;
}

const ComponentDescriptor* EntityDefRegistry::FindComponentById(uint16_t component_type_id) const {
  auto it = component_id_index.find(component_type_id);
  if (it == component_id_index.end()) return nullptr;
  return &components[it->second];
}

const ComponentDescriptor* EntityDefRegistry::FindComponentByName(std::string_view name) const {
  auto it = component_name_index.find(std::string(name));
  if (it == component_name_index.end()) return nullptr;
  return &components[it->second];
}

namespace {

auto ReadAndRegisterRecord(BinaryReader& reader, std::string_view section,
                           const std::function<bool(const std::byte*, int32_t)>& register_fn)
    -> Result<void> {
  auto blob_len_result = reader.ReadPackedInt();
  if (!blob_len_result) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: failed to read blob length in ") +
                     std::string(section) + " section"};
  }
  auto blob_len = *blob_len_result;
  auto bytes_result = reader.ReadBytes(blob_len);
  if (!bytes_result) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: failed to read blob bytes in ") +
                     std::string(section) + " section"};
  }
  if (!register_fn(bytes_result->data(), static_cast<int32_t>(blob_len))) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: register failed for ") +
                     std::string(section) + " record"};
  }
  return {};
}

}  // namespace

auto EntityDefRegistry::RegisterFromBinaryFile(const std::filesystem::path& path)
    -> Result<LoadedCounts> {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: cannot open ") + path.string()};
  }
  std::streamsize size = f.tellg();
  if (size < 0) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: tellg failed for ") + path.string()};
  }
  std::vector<std::byte> buf(static_cast<size_t>(size));
  f.seekg(0, std::ios::beg);
  if (size > 0) {
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (f.gcount() != size) {
      return Error{ErrorCode::kInvalidArgument,
                   std::string("RegisterFromBinaryFile: short read for ") + path.string()};
    }
  }
  return RegisterFromBinaryBuffer(std::span<const std::byte>(buf.data(), buf.size()));
}

auto EntityDefRegistry::RegisterFromBinaryBuffer(std::span<const std::byte> buf)
    -> Result<LoadedCounts> {
  constexpr std::size_t kChecksumSize = Sha256Digest{}.size();
  if (buf.size() < kChecksumSize)
    return Error{ErrorCode::kInvalidArgument, "RegisterFromBinaryBuffer: truncated file checksum"};
  const auto payload = buf.first(buf.size() - kChecksumSize);
  BinaryReader reader(payload);

  auto magic_result = reader.Read<uint32_t>();
  if (!magic_result || *magic_result != kBinaryFileMagic) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: bad magic (expected 'ATDF')"};
  }
  auto version_result = reader.Read<uint16_t>();
  if (!version_result || *version_result != kBinaryFileVersion) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryBuffer: unsupported version ") +
                     std::to_string(version_result.HasValue() ? *version_result : 0)};
  }
  auto flags_result = reader.Read<uint16_t>();
  if (!flags_result || *flags_result != 0) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: missing or unsupported flags word"};
  }
  auto checksum = Sha256(payload);
  if (!checksum) return checksum.Error();
  if (!std::equal(checksum->begin(), checksum->end(), buf.end() - kChecksumSize))
    return Error{ErrorCode::kInvalidArgument, "RegisterFromBinaryBuffer: file checksum mismatch"};

  auto digest_result = reader.ReadBytes(kDigestSize);
  if (!digest_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read 32-byte digest"};
  }
  SetDigest(digest_result->data(), static_cast<int32_t>(kDigestSize));

  LoadedCounts out;

  // Section order is structs -> components -> types.
  // Later sections may reference ids registered by earlier sections.
  auto struct_count_result = reader.ReadPackedInt();
  if (!struct_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read struct count"};
  }
  for (uint32_t i = 0; i < *struct_count_result; ++i) {
    auto r = ReadAndRegisterRecord(
        reader, "struct", [this](const std::byte* d, int32_t n) { return RegisterStruct(d, n); });
    if (!r) return r.Error();
  }
  out.structs = *struct_count_result;

  auto component_count_result = reader.ReadPackedInt();
  if (!component_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read component count"};
  }
  for (uint32_t i = 0; i < *component_count_result; ++i) {
    auto r = ReadAndRegisterRecord(reader, "component", [this](const std::byte* d, int32_t n) {
      return RegisterComponent(d, n);
    });
    if (!r) return r.Error();
  }
  out.components = *component_count_result;

  auto type_count_result = reader.ReadPackedInt();
  if (!type_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read type count"};
  }
  for (uint32_t i = 0; i < *type_count_result; ++i) {
    auto r = ReadAndRegisterRecord(reader, "type", [this](const std::byte* data, int32_t size) {
      if (size <= 0) return false;
      BinaryReader header(std::span<const std::byte>(data, static_cast<std::size_t>(size)));
      auto name = header.ReadString();
      auto id = header.Read<uint16_t>();
      if (!name || !id || FindByName(*name) || FindById(*id)) {
        ATLAS_LOG_ERROR("RegisterFromBinaryBuffer: invalid or duplicate entity type");
        return false;
      }
      return RegisterType(data, size);
    });
    if (!r) return r.Error();
  }
  out.types = *type_count_result;

  if (reader.Remaining() > 0) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("RegisterFromBinaryBuffer: {} trailing bytes after type section",
                             reader.Remaining())};
  }
  auto schema = StorageSchemaDigest();
  if (!schema) return schema.Error();
  return out;
}

auto EntityDefRegistry::StorageSchemaDigest() const -> Result<Sha256Digest> {
  BinaryWriter writer;
  writer.Write(uint16_t{1});
  std::function<bool(const DataTypeRef&, std::size_t)> write_type;
  write_type = [&](const DataTypeRef& type, std::size_t depth) {
    if (depth > kMaxDataTypeDepth) return false;
    writer.Write(static_cast<uint8_t>(type.kind));
    switch (type.kind) {
      case PropertyDataType::kList:
        return type.elem && !type.key && write_type(*type.elem, depth + 1);
      case PropertyDataType::kDict:
        return type.key && type.elem && type.key->kind <= PropertyDataType::kCustom &&
               write_type(*type.key, depth + 1) && write_type(*type.elem, depth + 1);
      case PropertyDataType::kStruct:
        if (type.elem || type.key || !FindStructById(type.struct_id)) return false;
        writer.Write(type.struct_id);
        return true;
      default:
        return type.kind <= PropertyDataType::kCustom && !type.elem && !type.key;
    }
  };
  auto write_properties = [&](std::string_view owner,
                              const std::vector<PropertyDescriptor>& properties) -> Result<void> {
    writer.WritePackedInt(static_cast<uint32_t>(properties.size()));
    for (const auto& prop : properties) {
      writer.WriteString(prop.name);
      writer.Write(prop.index);
      writer.Write(static_cast<uint8_t>(prop.scope));
      writer.Write(static_cast<uint8_t>(prop.persistent));
      writer.Write(static_cast<uint8_t>(prop.identifier));
      writer.Write(prop.max_size);
      DataTypeRef scalar;
      scalar.kind = prop.data_type;
      if (!ValidatePropertyInvariant(prop) || !write_type(prop.type_ref.value_or(scalar), 0))
        return Error{ErrorCode::kInvalidArgument,
                     std::format("Invalid storage schema type for {}.{}", owner, prop.name)};
    }
    return {};
  };

  auto sorted_types = types;
  std::ranges::sort(sorted_types, {}, &EntityTypeDescriptor::type_id);
  writer.WritePackedInt(static_cast<uint32_t>(sorted_types.size()));
  for (const auto& type : sorted_types) {
    writer.Write(type.type_id);
    writer.WriteString(type.name);
    writer.Write(static_cast<uint8_t>(type.has_cell));
    auto properties = write_properties(type.name, type.properties);
    if (!properties) return properties.Error();
    writer.WritePackedInt(static_cast<uint32_t>(type.slots.size()));
    for (const auto& slot : type.slots) {
      if (slot.component_type_id != 0 && !FindComponentById(slot.component_type_id))
        return Error{ErrorCode::kInvalidArgument,
                     std::format("Unknown storage component for {}.{}", type.name, slot.slot_name)};
      writer.Write(slot.slot_idx);
      writer.WriteString(slot.slot_name);
      writer.Write(slot.component_type_id);
      writer.Write(static_cast<uint8_t>(slot.scope));
      writer.Write(static_cast<uint8_t>(slot.lazy));
      if (slot.component_type_id == 0) {
        if (slot.inline_component.name.empty())
          return Error{ErrorCode::kInvalidArgument, "Inline storage component type name missing"};
        writer.WriteString(slot.inline_component.name);
        auto fields =
            write_properties(slot.inline_component.name, slot.inline_component.properties);
        if (!fields) return fields.Error();
      }
    }
  }

  auto sorted_components = components;
  std::ranges::sort(sorted_components, {}, &ComponentDescriptor::component_type_id);
  writer.WritePackedInt(static_cast<uint32_t>(sorted_components.size()));
  for (const auto& component : sorted_components) {
    if (!component.base_name.empty() && !FindComponentByName(component.base_name))
      return Error{ErrorCode::kInvalidArgument,
                   std::format("Unknown storage component parent for {}", component.name)};
    writer.Write(component.component_type_id);
    writer.WriteString(component.name);
    writer.WriteString(component.base_name);
    writer.Write(static_cast<uint8_t>(component.locality));
    auto properties = write_properties(component.name, component.properties);
    if (!properties) return properties.Error();
  }

  auto sorted_structs = structs;
  std::ranges::sort(sorted_structs, {}, &StructDescriptor::id);
  writer.WritePackedInt(static_cast<uint32_t>(sorted_structs.size()));
  for (const auto& structure : sorted_structs) {
    writer.Write(structure.id);
    writer.WriteString(structure.name);
    writer.WritePackedInt(static_cast<uint32_t>(structure.fields.size()));
    for (const auto& field : structure.fields) {
      writer.WriteString(field.name);
      if (!write_type(field.type, 0))
        return Error{ErrorCode::kInvalidArgument,
                     std::format("Invalid storage struct field {}.{}", structure.name, field.name)};
    }
  }
  return Sha256(writer.Data());
}

}  // namespace atlas
