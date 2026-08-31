#pragma once
/// @file Reflection.hpp
/// @brief Minimal runtime type information for engine types.
///
/// Goals (docs/architecture/reflection.md):
///  * Enough metadata to serialize a struct generically, to build an editor
///    inspector automatically and to instantiate types by name.
///  * Zero cost when unused: registration happens once at start-up, and hot
///    code never touches the registry.
///  * No code generation and no macros beyond a small registration helper, so
///    types stay plain C++ structs that gameplay code can use directly.
///
/// What this is *not*: a full C++ reflection system.  Methods, base classes and
/// templates are out of scope; components are described by their data.

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Hash.hpp"
#include "local3d/math/Math.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace l3d::reflect {

using TypeId = u64;
inline constexpr TypeId kInvalidTypeId = 0;

/// How the editor and the serializer should treat a property.  Types that map
/// to a well known kind get a specialised widget and binary layout.
enum class PropertyKind : u8 {
    Unknown = 0,
    Bool,
    Int32,
    Uint32,
    Int64,
    Float,
    Double,
    Vec2,
    Vec3,
    Vec4,
    Quaternion,
    Color,
    String,
    Enum,
    AssetReference,
    Struct,
};

struct PropertyInfo;
struct TypeInfo;

/// Description of one enum, used for drop-downs and text round trips.
struct EnumInfo {
    std::string name;
    std::vector<std::pair<i64, std::string>> entries;

    [[nodiscard]] std::string_view NameOf(i64 value) const noexcept {
        for (const auto& entry : entries) {
            if (entry.first == value) {
                return entry.second;
            }
        }
        return "";
    }

    [[nodiscard]] bool ValueOf(std::string_view entryName, i64& out) const noexcept {
        for (const auto& entry : entries) {
            if (entry.second == entryName) {
                out = entry.first;
                return true;
            }
        }
        return false;
    }
};

/// One reflected member.  Access is through void* so the serializer does not
/// need a template per property.
struct PropertyInfo {
    std::string name;
    PropertyKind kind = PropertyKind::Unknown;
    usize offset = 0;
    usize size = 0;
    const TypeInfo* structType = nullptr; ///< Set when kind == Struct.
    const EnumInfo* enumInfo = nullptr;   ///< Set when kind == Enum.
    std::string documentation;            ///< Shown in the inspector tooltip.

    /// Raw address of this property inside `object`.
    [[nodiscard]] void* Address(void* object) const noexcept {
        return static_cast<unsigned char*>(object) + offset;
    }
    [[nodiscard]] const void* Address(const void* object) const noexcept {
        return static_cast<const unsigned char*>(object) + offset;
    }
};

/// Reflected type: identity, layout and the operations the engine needs.
struct TypeInfo {
    TypeId id = kInvalidTypeId;
    std::string name;
    usize size = 0;
    usize alignment = 0;

    /// Lifecycle hooks.  They let generic code own instances without templates.
    void (*construct)(void* memory) = nullptr;
    void (*destruct)(void* memory) = nullptr;
    void (*copy)(void* dst, const void* src) = nullptr;

    /// Optional factory used by scene/prefab loading to create by name.
    std::unique_ptr<void, void (*)(void*)> (*create)() = nullptr;

    std::vector<PropertyInfo> properties;

    [[nodiscard]] const PropertyInfo* FindProperty(std::string_view propertyName) const noexcept {
        for (const PropertyInfo& property : properties) {
            if (property.name == propertyName) {
                return &property;
            }
        }
        return nullptr;
    }
};

/// Process wide registry.  Types are registered during static initialisation or
/// explicit start-up; lookups are read-only afterwards, so no locking is needed
/// on the read path (documented in docs/architecture/reflection.md).
class TypeRegistry {
public:
    [[nodiscard]] static TypeRegistry& Instance() noexcept;

    /// Register (or fetch, if already present) the type info for T.
    template <typename T>
    TypeInfo& Register(std::string_view name) {
        const TypeId id = HashString(name);
        auto found = types_.find(id);
        if (found != types_.end()) {
            return *found->second;
        }
        auto info = std::make_unique<TypeInfo>();
        info->id = id;
        info->name = std::string(name);
        info->size = sizeof(T);
        info->alignment = alignof(T);
        info->construct = [](void* memory) { new (memory) T(); };
        info->destruct = [](void* memory) { static_cast<T*>(memory)->~T(); };
        info->copy = [](void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); };
        TypeInfo* raw = info.get();
        types_.emplace(id, std::move(info));
        byName_.emplace(raw->name, raw);
        return *raw;
    }

    [[nodiscard]] const TypeInfo* Find(TypeId id) const noexcept {
        const auto found = types_.find(id);
        return found != types_.end() ? found->second.get() : nullptr;
    }

    [[nodiscard]] const TypeInfo* FindByName(std::string_view name) const noexcept {
        const auto found = byName_.find(std::string(name));
        return found != byName_.end() ? found->second : nullptr;
    }

    /// Register an enum so properties of that type can be edited by name.
    const EnumInfo& RegisterEnum(std::string_view name,
                                 std::vector<std::pair<i64, std::string>> entries);
    [[nodiscard]] const EnumInfo* FindEnum(std::string_view name) const noexcept;

    /// Every registered type, for editor tooling and debugging.
    [[nodiscard]] std::vector<const TypeInfo*> AllTypes() const;

    void Clear();

private:
    TypeRegistry() = default;
    std::unordered_map<TypeId, std::unique_ptr<TypeInfo>> types_;
    std::unordered_map<std::string, TypeInfo*> byName_;
    std::unordered_map<std::string, std::unique_ptr<EnumInfo>> enums_;
};

/// Derive a PropertyKind from a C++ member type.
template <typename T>
struct PropertyKindOf {
    static constexpr PropertyKind value = PropertyKind::Struct;
};
template <>
struct PropertyKindOf<bool> {
    static constexpr PropertyKind value = PropertyKind::Bool;
};
template <>
struct PropertyKindOf<i32> {
    static constexpr PropertyKind value = PropertyKind::Int32;
};
template <>
struct PropertyKindOf<u32> {
    static constexpr PropertyKind value = PropertyKind::Uint32;
};
template <>
struct PropertyKindOf<i64> {
    static constexpr PropertyKind value = PropertyKind::Int64;
};
template <>
struct PropertyKindOf<f32> {
    static constexpr PropertyKind value = PropertyKind::Float;
};
template <>
struct PropertyKindOf<f64> {
    static constexpr PropertyKind value = PropertyKind::Double;
};
template <>
struct PropertyKindOf<math::Vec2> {
    static constexpr PropertyKind value = PropertyKind::Vec2;
};
template <>
struct PropertyKindOf<math::Vec3> {
    static constexpr PropertyKind value = PropertyKind::Vec3;
};
template <>
struct PropertyKindOf<math::Vec4> {
    static constexpr PropertyKind value = PropertyKind::Vec4;
};
template <>
struct PropertyKindOf<math::Quaternion> {
    static constexpr PropertyKind value = PropertyKind::Quaternion;
};
template <>
struct PropertyKindOf<math::Color> {
    static constexpr PropertyKind value = PropertyKind::Color;
};
template <>
struct PropertyKindOf<std::string> {
    static constexpr PropertyKind value = PropertyKind::String;
};

/// Fluent builder returned by RegisterType().
template <typename T>
class TypeBuilder {
public:
    explicit TypeBuilder(TypeInfo& info) noexcept : info_(&info) {}

    /// Register a direct member with the kind deduced from its C++ type.
    template <typename M>
    TypeBuilder& Property(std::string_view name, M T::*member) {
        static_assert(std::is_standard_layout_v<T>, "Reflected types must be standard layout");
        PropertyInfo property;
        property.name = std::string(name);
        property.kind = PropertyKindOf<std::remove_cv_t<M>>::value;
        property.offset = OffsetOf(member);
        property.size = sizeof(M);
        if constexpr (std::is_enum_v<M>) {
            property.kind = PropertyKind::Enum;
            property.enumInfo = TypeRegistry::Instance().FindEnum(EnumNameOf<M>());
        }
        info_->properties.push_back(std::move(property));
        return *this;
    }

    /// Register a member whose kind must be stated explicitly (asset ids,
    /// nested structs, enums whose name is not deducible).
    TypeBuilder& Property(std::string_view name, usize offset, usize size, PropertyKind kind) {
        PropertyInfo property;
        property.name = std::string(name);
        property.kind = kind;
        property.offset = offset;
        property.size = size;
        info_->properties.push_back(std::move(property));
        return *this;
    }

    /// Attach tooltip text to the most recently added property.
    TypeBuilder& Document(std::string_view text) {
        if (!info_->properties.empty()) {
            info_->properties.back().documentation = std::string(text);
        }
        return *this;
    }

    /// Bind the enum description used by the most recently added property.
    TypeBuilder& WithEnum(std::string_view enumName) {
        if (!info_->properties.empty()) {
            info_->properties.back().kind = PropertyKind::Enum;
            info_->properties.back().enumInfo = TypeRegistry::Instance().FindEnum(enumName);
        }
        return *this;
    }

    /// Mark the most recent property as a nested struct reference.
    TypeBuilder& WithStruct(const TypeInfo& nested) {
        if (!info_->properties.empty()) {
            info_->properties.back().kind = PropertyKind::Struct;
            info_->properties.back().structType = &nested;
        }
        return *this;
    }

    [[nodiscard]] TypeInfo& Get() const noexcept { return *info_; }

private:
    /// Offset of a member pointer.  A real, default constructed instance is
    /// used so nothing uninitialised is ever read (components are always
    /// default constructible; see the static_assert in Register).
    template <typename M>
    [[nodiscard]] static usize OffsetOf(M T::*member) noexcept {
        static_assert(std::is_standard_layout_v<T>, "Reflected types must be standard layout");
        static const T instance{};
        const M* memberAddress = &(instance.*member);
        return static_cast<usize>(reinterpret_cast<const unsigned char*>(memberAddress) -
                                  reinterpret_cast<const unsigned char*>(&instance));
    }

    /// Enum names cannot be deduced portably, so properties declare them with
    /// WithEnum().  This placeholder keeps Property() compiling for enum members.
    template <typename M>
    [[nodiscard]] static std::string_view EnumNameOf() noexcept {
        return "UnnamedEnum";
    }

    TypeInfo* info_;
};

/// Entry point: `RegisterType<MyComponent>("MyComponent").Property(...)`.
template <typename T>
[[nodiscard]] TypeBuilder<T> RegisterType(std::string_view name) {
    return TypeBuilder<T>(TypeRegistry::Instance().Register<T>(name));
}

} // namespace l3d::reflect
