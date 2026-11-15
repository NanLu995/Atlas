using System.Collections.Generic;
using Atlas.Generators.Def;
using Atlas.Generators.Def.Emitters;
using Xunit;

namespace Atlas.Generators.Tests;

public class DigestEmitterTests
{
    [Fact]
    public void InlineComponentFieldsAreExportedAndChangeTheDefinitionDigest()
    {
        var entity = MakeAvatar();
        var component = new ComponentDefModel
        {
            TypeName = "Inventory", SlotName = "inventory", SlotIdx = 1,
        };
        var field = new PropertyDefModel { Name = "inlineValue", Type = "int32" };
        component.Properties.Add(field);
        entity.Components.Add(component);
        var types = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        string Digest() => ExtractHex(DigestEmitter.Emit(new[] { entity }, new List<StructDefModel>(),
                                                        new List<ComponentDefModel>(), types));
        var before = Digest();
        field.Type = "int64";
        Assert.NotEqual(before, Digest());
        var code = TypeRegistryEmitter.Emit(
            new List<(EntityDefModel, string, string)> { (entity, "Avatar", "Game") },
            types, ProcessContext.Base);
        Assert.Contains("writer.WriteString(\"Inventory\")", code);
        Assert.Contains("writer.WriteString(\"inlineValue\")", code);
        Assert.Contains("writer.WriteUInt16(0)", code);
    }

    [Fact]
    public void ContainerSizeLimitChangesTheDigest()
    {
        var entity = MakeAvatar();
        var property = new PropertyDefModel
        {
            Name = "values",
            Type = "list<int32>",
            TypeRef = new DataTypeRefModel
            {
                Kind = PropertyDataKind.List,
                Elem = new DataTypeRefModel { Kind = PropertyDataKind.Int32 },
            },
        };
        entity.Properties.Add(property);
        var types = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        string Emit() => ExtractHex(DigestEmitter.Emit(new[] { entity }, new List<StructDefModel>(),
                                                      new List<ComponentDefModel>(), types));
        var before = Emit();
        property.MaxSize = 12;
        Assert.NotEqual(before, Emit());
    }

    [Theory]
    [InlineData("name")]
    [InlineData("lazy")]
    [InlineData("id")]
    [InlineData("parent")]
    public void ComponentStorageMetadataChangesTheDigest(string change)
    {
        var entity = MakeAvatar();
        var component = new ComponentDefModel
        {
            TypeName = "Equipment", SlotName = "equipment", SlotIdx = 1, ComponentTypeId = 3,
        };
        entity.Components.Add(component);
        var types = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        var components = new List<ComponentDefModel> { component };
        string Emit() => ExtractHex(DigestEmitter.Emit(new[] { entity }, new List<StructDefModel>(),
                                                      components, types));
        var before = Emit();
        switch (change)
        {
            case "name": component.SlotName = "other"; break;
            case "lazy": component.Lazy = true; break;
            case "id": component.ComponentTypeId = 4; break;
            case "parent": component.BaseTypeName = "Stats"; break;
        }
        Assert.NotEqual(before, Emit());
    }

    private static EntityDefModel MakeAvatar()
    {
        var def = new EntityDefModel { Name = "Avatar" };
        def.Properties.Add(new PropertyDefModel
        {
            Name = "hp",
            Type = "int32",
            Scope = PropertyScope.AllClients,
        });
        def.ClientMethods.Add(new MethodDefModel { Name = "ShowEffect" });
        return def;
    }

    [Fact]
    public void Emit_SameInputs_ProducesIdenticalDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };

        var first = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                       new List<ComponentDefModel>(), typeIds);
        var second = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                        new List<ComponentDefModel>(), typeIds);

        Assert.Equal(ExtractHex(first), ExtractHex(second));
    }

    [Fact]
    public void Emit_RenameMethod_ChangesDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        var baseline = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                     new List<ComponentDefModel>(), typeIds));

        defs[0].ClientMethods[0].Name = "ShowEffectV2";
        var mutated = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                    new List<ComponentDefModel>(), typeIds));

        Assert.NotEqual(baseline, mutated);
    }

    [Fact]
    public void Emit_DifferentTypeId_ChangesDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var first = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                  new List<ComponentDefModel>(),
                                                  new Dictionary<string, ushort> { ["Avatar"] = 1 }));
        var second = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                   new List<ComponentDefModel>(),
                                                   new Dictionary<string, ushort> { ["Avatar"] = 2 }));
        Assert.NotEqual(first, second);
    }

    [Fact]
    public void Emit_AppendsBytesArrayLiteral()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        var code = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                      new List<ComponentDefModel>(), typeIds);
        Assert.Contains("Sha256Hex = \"", code);
        Assert.Contains("private static readonly byte[] s_bytes", code);
        Assert.Contains("public static System.ReadOnlySpan<byte> Bytes => s_bytes;", code);
    }

    private static string ExtractHex(string emitted)
    {
        const string Marker = "Sha256Hex = \"";
        int start = emitted.IndexOf(Marker, System.StringComparison.Ordinal);
        Assert.True(start >= 0);
        start += Marker.Length;
        int end = emitted.IndexOf('"', start);
        return emitted.Substring(start, end - start);
    }
}
