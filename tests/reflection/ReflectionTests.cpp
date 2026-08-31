// Reflection tests: registration, property offsets, enum metadata and the
// registry lookups the editor and serializer rely on.
#include "doctest.h"

#include "local3d/reflection/Reflection.hpp"

#include <string>

using namespace l3d;
using namespace l3d::reflect;

namespace {

enum class LightType : i32 { Directional = 0, Point = 1, Spot = 2 };

struct MaterialParams {
    f32 roughness = 0.5f;
    f32 metallic = 0.0f;
};

struct LightComponent {
    LightType type = LightType::Directional;
    math::Color color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    std::string debugName = "light";
    i32 shadowResolution = 1024;
};

const TypeInfo& RegisterLight() {
    auto& registry = TypeRegistry::Instance();
    registry.RegisterEnum("LightType", {{0, "Directional"}, {1, "Point"}, {2, "Spot"}});
    return RegisterType<LightComponent>("LightComponent")
        .Property("type", &LightComponent::type)
        .WithEnum("LightType")
        .Property("color", &LightComponent::color)
        .Property("intensity", &LightComponent::intensity)
        .Document("Radiant intensity in candela")
        .Property("position", &LightComponent::position)
        .Property("debugName", &LightComponent::debugName)
        .Property("shadowResolution", &LightComponent::shadowResolution)
        .Get();
}

} // namespace

TEST_SUITE("reflection.registry") {
    TEST_CASE("registers a type with properties") {
        const TypeInfo& info = RegisterLight();
        CHECK(info.name == "LightComponent");
        CHECK(info.size == sizeof(LightComponent));
        CHECK(info.properties.size() == 6);

        const PropertyInfo* intensity = info.FindProperty("intensity");
        REQUIRE(intensity != nullptr);
        CHECK(intensity->kind == PropertyKind::Float);
        CHECK(intensity->documentation == "Radiant intensity in candela");
        CHECK(info.FindProperty("nope") == nullptr);
    }

    TEST_CASE("property offsets address the right memory") {
        const TypeInfo& info = RegisterLight();
        LightComponent component;
        component.intensity = 42.0f;
        component.shadowResolution = 2048;
        component.debugName = "sun";

        const PropertyInfo* intensity = info.FindProperty("intensity");
        REQUIRE(intensity != nullptr);
        CHECK(*static_cast<f32*>(intensity->Address(&component)) == 42.0f);

        const PropertyInfo* shadow = info.FindProperty("shadowResolution");
        REQUIRE(shadow != nullptr);
        CHECK(*static_cast<i32*>(shadow->Address(&component)) == 2048);

        const PropertyInfo* name = info.FindProperty("debugName");
        REQUIRE(name != nullptr);
        CHECK(*static_cast<std::string*>(name->Address(&component)) == "sun");

        // Writing through the property must be visible on the struct.
        *static_cast<f32*>(intensity->Address(&component)) = 7.5f;
        CHECK(component.intensity == doctest::Approx(7.5f));
    }

    TEST_CASE("kinds are deduced from member types") {
        const TypeInfo& info = RegisterLight();
        CHECK(info.FindProperty("color")->kind == PropertyKind::Color);
        CHECK(info.FindProperty("position")->kind == PropertyKind::Vec3);
        CHECK(info.FindProperty("debugName")->kind == PropertyKind::String);
        CHECK(info.FindProperty("type")->kind == PropertyKind::Enum);
    }

    TEST_CASE("lookup by id and by name") {
        const TypeInfo& info = RegisterLight();
        auto& registry = TypeRegistry::Instance();
        CHECK(registry.Find(info.id) == &info);
        CHECK(registry.FindByName("LightComponent") == &info);
        CHECK(registry.FindByName("DoesNotExist") == nullptr);
        CHECK(registry.Find(12345) == nullptr);

        const auto all = registry.AllTypes();
        CHECK_FALSE(all.empty());
    }

    TEST_CASE("enums expose names and values") {
        RegisterLight();
        const EnumInfo* info = TypeRegistry::Instance().FindEnum("LightType");
        REQUIRE(info != nullptr);
        CHECK(info->NameOf(1) == "Point");
        CHECK(info->NameOf(99) == "");

        i64 value = -1;
        REQUIRE(info->ValueOf("Spot", value));
        CHECK(value == 2);
        CHECK_FALSE(info->ValueOf("Laser", value));
    }

    TEST_CASE("construct, destruct and copy hooks work") {
        const TypeInfo& info = RegisterLight();
        alignas(LightComponent) unsigned char memory[sizeof(LightComponent)]{};
        info.construct(memory);
        auto* component = reinterpret_cast<LightComponent*>(memory); // NOLINT
        component->debugName = "constructed";
        CHECK(component->intensity == doctest::Approx(1.0f));

        LightComponent copy;
        info.copy(&copy, memory);
        CHECK(copy.debugName == "constructed");

        info.destruct(memory);
        CHECK(copy.debugName.size() > 0);
    }

    TEST_CASE("registration is idempotent") {
        const TypeInfo& first = RegisterLight();
        const TypeInfo& second = RegisterLight();
        CHECK(&first == &second);
    }
}
