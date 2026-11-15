#include "clrscript/clr_bootstrap.h"

#include <format>

#include "clrscript/clr_export.h"
#include "foundation/error.h"
#include "foundation/log.h"

namespace atlas {

namespace {

using BootstrapFn = int (*)(ClrBootstrapArgs*, ClrObjectVTableOut*);

auto ClrBootstrapImpl(ClrHost& host, const std::filesystem::path& runtime_dll,
                      ClrBootstrapArgs args) -> Result<void> {
  auto abi = VerifyClrAbi(host, runtime_dll, "Atlas.Core.Bootstrap, Atlas.ClrHost");
  if (!abi) return abi.Error();
  auto method = host.GetMethodAs<BootstrapFn>(runtime_dll, "Atlas.Core.Bootstrap, Atlas.ClrHost",
                                              "Initialize");
  if (!method)
    return Error{ErrorCode::kScriptError,
                 std::format("clr_bootstrap: failed to resolve Bootstrap.Initialize: {}",
                             method.Error().Message())};

  ClrObjectVTableOut vtable_out{};

  const int kRc = (*method)(&args, &vtable_out);
  if (kRc != 0)
    return Error{ErrorCode::kScriptError,
                 "clr_bootstrap: Bootstrap.Initialize() returned non-zero; check stderr"};

  if (!vtable_out.free_handle || !vtable_out.get_type_name || !vtable_out.is_none ||
      !vtable_out.to_int64 || !vtable_out.to_double || !vtable_out.to_string || !vtable_out.to_bool)
    return Error{ErrorCode::kScriptError,
                 "clr_bootstrap: Bootstrap.Initialize() returned incomplete vtable"};

  ClrObjectVTable vtable{};
  vtable.free_handle = vtable_out.free_handle;
  vtable.get_type_name = vtable_out.get_type_name;
  vtable.is_none = vtable_out.is_none;
  vtable.to_int64 = vtable_out.to_int64;
  vtable.to_double = vtable_out.to_double;
  vtable.to_string = vtable_out.to_string;
  vtable.to_bool = vtable_out.to_bool;

  SetClrObjectVtable(vtable);

  ATLAS_LOG_INFO("CLR bootstrap complete: ErrorBridge and ClrObjectVTable registered");
  return {};
}

}  // namespace

auto VerifyClrAbi(ClrHost& host, const std::filesystem::path& assembly, std::string_view type_name)
    -> Result<void> {
  auto version = host.GetMethodAs<uint32_t (*)()>(assembly, type_name, "GetAbiVersion");
  if (!version)
    return Error{ErrorCode::kScriptError,
                 std::format("Managed ABI version entry missing for {}: {}", type_name,
                             version.Error().Message())};
  const auto actual = (*version)();
  if (actual != kAtlasAbiVersion)
    return Error{ErrorCode::kScriptError,
                 std::format("Managed ABI mismatch for {}: expected {}, received {}", type_name,
                             kAtlasAbiVersion, actual)};
  return {};
}

auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll) -> Result<void> {
  return ClrBootstrapImpl(host, runtime_dll, ClrBootstrapArgs{});
}

// Custom args: caller supplies DLL-sourced error function pointers.
auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll, ClrBootstrapArgs args)
    -> Result<void> {
  return ClrBootstrapImpl(host, runtime_dll, args);
}

}  // namespace atlas
