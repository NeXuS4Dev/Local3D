#include "local3d/scene/SceneSerializer.hpp"

#include "local3d/assets/AssetId.hpp"
#include "local3d/serialization/Json.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace l3d::scene {
namespace {

using serial::JsonValue;

[[nodiscard]] JsonValue Vec3ToJson(math::Vec3 value) {
    JsonValue array = JsonValue::MakeArray();
    array.Push(JsonValue(static_cast<f64>(value.x)));
    array.Push(JsonValue(static_cast<f64>(value.y)));
    array.Push(JsonValue(static_cast<f64>(value.z)));
    return array;
}

[[nodiscard]] JsonValue Vec4ToJson(math::Vec4 value) {
    JsonValue array = JsonValue::MakeArray();
    array.Push(JsonValue(static_cast<f64>(value.x)));
    array.Push(JsonValue(static_cast<f64>(value.y)));
    array.Push(JsonValue(static_cast<f64>(value.z)));
    array.Push(JsonValue(static_cast<f64>(value.w)));
    return array;
}

[[nodiscard]] math::Vec3 JsonToVec3(const JsonValue& value, math::Vec3 fallback) {
    if (!value.IsArray() || value.Size() < 3) {
        return fallback;
    }
    return math::Vec3{static_cast<f32>(value[0].AsNumber(fallback.x)),
                      static_cast<f32>(value[1].AsNumber(fallback.y)),
                      static_cast<f32>(value[2].AsNumber(fallback.z))};
}

[[nodiscard]] math::Vec4 JsonToVec4(const JsonValue& value, math::Vec4 fallback) {
    if (!value.IsArray() || value.Size() < 4) {
        return fallback;
    }
    return math::Vec4{static_cast<f32>(value[0].AsNumber(fallback.x)),
                      static_cast<f32>(value[1].AsNumber(fallback.y)),
                      static_cast<f32>(value[2].AsNumber(fallback.z)),
                      static_cast<f32>(value[3].AsNumber(fallback.w))};
}

[[nodiscard]] math::Quaternion JsonToQuaternion(const JsonValue& value) {
    if (!value.IsArray() || value.Size() < 4) {
        return math::Quaternion::Identity();
    }
    math::Quaternion rotation{static_cast<f32>(value[0].AsNumber(0.0)),
                              static_cast<f32>(value[1].AsNumber(0.0)),
                              static_cast<f32>(value[2].AsNumber(0.0)),
                              static_cast<f32>(value[3].AsNumber(1.0))};
    // A hand edited file can hold a non unit quaternion; normalising keeps the
    // world matrices orthonormal instead of silently scaling the node.
    return rotation.Normalized();
}

/// An empty string means "no asset", so a null id round trips as "".
[[nodiscard]] std::string AssetIdToJsonText(assets::AssetId id) {
    return id.IsNull() ? std::string{} : id.ToString();
}

[[nodiscard]] assets::AssetId JsonTextToAssetId(std::string_view text) {
    if (text.empty()) {
        return assets::kNullAssetId;
    }
    auto parsed = assets::AssetId::Parse(text);
    return parsed.HasValue() ? *parsed : assets::kNullAssetId;
}

[[nodiscard]] std::string_view LightTypeToString(LightComponent::Type type) noexcept {
    switch (type) {
        case LightComponent::Type::Directional: return "Directional";
        case LightComponent::Type::Point: return "Point";
        case LightComponent::Type::Spot: return "Spot";
    }
    return "Directional";
}

[[nodiscard]] LightComponent::Type LightTypeFromString(std::string_view text) noexcept {
    if (text == "Point") {
        return LightComponent::Type::Point;
    }
    if (text == "Spot") {
        return LightComponent::Type::Spot;
    }
    return LightComponent::Type::Directional;
}

// --- Writing ---------------------------------------------------------------

[[nodiscard]] JsonValue MeshRendererToJson(const MeshRendererComponent& renderer) {
    JsonValue json = JsonValue::MakeObject();

    JsonValue lods = JsonValue::MakeArray();
    for (const assets::AssetId& lod : renderer.lods) {
        lods.Push(JsonValue(AssetIdToJsonText(lod)));
    }
    json.Set("lods", std::move(lods));

    JsonValue coverage = JsonValue::MakeArray();
    for (const f32 value : renderer.lodSwitchCoverage) {
        coverage.Push(JsonValue(static_cast<f64>(value)));
    }
    json.Set("lodSwitchCoverage", std::move(coverage));

    json.Set("material", JsonValue(AssetIdToJsonText(renderer.material)));
    json.Set("tint", Vec4ToJson(renderer.tint));
    json.Set("castsShadow", JsonValue(renderer.castsShadow));
    json.Set("visible", JsonValue(renderer.visible));
    return json;
}

[[nodiscard]] JsonValue LightToJson(const LightComponent& light) {
    JsonValue json = JsonValue::MakeObject();
    json.Set("type", JsonValue(std::string(LightTypeToString(light.type))));
    json.Set("color", Vec3ToJson(light.color));
    json.Set("intensity", JsonValue(static_cast<f64>(light.intensity)));
    json.Set("range", JsonValue(static_cast<f64>(light.range)));
    json.Set("innerAngleDegrees", JsonValue(static_cast<f64>(light.innerAngleDegrees)));
    json.Set("outerAngleDegrees", JsonValue(static_cast<f64>(light.outerAngleDegrees)));
    json.Set("castsShadow", JsonValue(light.castsShadow));
    return json;
}

[[nodiscard]] JsonValue CameraToJson(const CameraComponent& camera) {
    JsonValue json = JsonValue::MakeObject();
    json.Set("fovYDegrees", JsonValue(static_cast<f64>(camera.fovYDegrees)));
    json.Set("nearPlane", JsonValue(static_cast<f64>(camera.nearPlane)));
    json.Set("farPlane", JsonValue(static_cast<f64>(camera.farPlane)));
    JsonValue clear = JsonValue::MakeArray();
    clear.Push(JsonValue(static_cast<f64>(camera.clearColor.r)));
    clear.Push(JsonValue(static_cast<f64>(camera.clearColor.g)));
    clear.Push(JsonValue(static_cast<f64>(camera.clearColor.b)));
    clear.Push(JsonValue(static_cast<f64>(camera.clearColor.a)));
    json.Set("clearColor", std::move(clear));
    json.Set("active", JsonValue(camera.active));
    return json;
}

// --- Reading ---------------------------------------------------------------

[[nodiscard]] Result<MeshRendererComponent> MeshRendererFromJson(const JsonValue& json) {
    MeshRendererComponent component;
    const JsonValue& lods = json["lods"];
    if (lods.IsArray()) {
        const usize count = lods.Size() < MeshRendererComponent::kMaxLods
                                ? lods.Size()
                                : MeshRendererComponent::kMaxLods;
        for (usize i = 0; i < count; ++i) {
            component.lods[i] = JsonTextToAssetId(lods[i].AsString());
        }
    }
    const JsonValue& coverage = json["lodSwitchCoverage"];
    if (coverage.IsArray()) {
        const usize count = coverage.Size() < component.lodSwitchCoverage.size()
                                ? coverage.Size()
                                : component.lodSwitchCoverage.size();
        for (usize i = 0; i < count; ++i) {
            component.lodSwitchCoverage[i] =
                static_cast<f32>(coverage[i].AsNumber(component.lodSwitchCoverage[i]));
        }
    }
    component.material = JsonTextToAssetId(json["material"].AsString());
    component.tint = JsonToVec4(json["tint"], component.tint);
    component.castsShadow = json["castsShadow"].AsBool(component.castsShadow);
    component.visible = json["visible"].AsBool(component.visible);

    // A zero or negative first threshold would make every item pick the coarsest
    // lod at any distance, which reads as "my model lost all its detail".
    if (component.HasMesh() && component.lodSwitchCoverage[0] <= 0.0f) {
        return Unexpected(Status{StatusCode::InvalidArgument,
                                 "Node lod switch coverage must be positive"});
    }
    return component;
}

[[nodiscard]] LightComponent LightFromJson(const JsonValue& json) {
    LightComponent component;
    component.type = LightTypeFromString(json["type"].AsString("Directional"));
    component.color = JsonToVec3(json["color"], component.color);
    component.intensity = static_cast<f32>(json["intensity"].AsNumber(component.intensity));
    component.range = static_cast<f32>(json["range"].AsNumber(component.range));
    component.innerAngleDegrees =
        static_cast<f32>(json["innerAngleDegrees"].AsNumber(component.innerAngleDegrees));
    component.outerAngleDegrees =
        static_cast<f32>(json["outerAngleDegrees"].AsNumber(component.outerAngleDegrees));
    component.castsShadow = json["castsShadow"].AsBool(component.castsShadow);
    return component;
}

[[nodiscard]] CameraComponent CameraFromJson(const JsonValue& json) {
    CameraComponent component;
    component.fovYDegrees = static_cast<f32>(json["fovYDegrees"].AsNumber(component.fovYDegrees));
    component.nearPlane = static_cast<f32>(json["nearPlane"].AsNumber(component.nearPlane));
    component.farPlane = static_cast<f32>(json["farPlane"].AsNumber(component.farPlane));
    const JsonValue& clear = json["clearColor"];
    if (clear.IsArray() && clear.Size() >= 4) {
        component.clearColor = math::Color{static_cast<f32>(clear[0].AsNumber(0.0f)),
                                          static_cast<f32>(clear[1].AsNumber(0.0f)),
                                          static_cast<f32>(clear[2].AsNumber(0.0f)),
                                          static_cast<f32>(clear[3].AsNumber(1.0f))};
    }
    component.active = json["active"].AsBool(false);
    return component;
}

} // namespace

Result<std::string> SceneSerializer::ToJson(const Scene& scene, u32 indent) {
    const std::vector<ecs::Entity> nodes = scene.Nodes();

    JsonValue root = JsonValue::MakeObject();
    root.Set("format", JsonValue(kFormatVersion));
    root.Set("name", JsonValue(scene.Name()));

    JsonValue nodeArray = JsonValue::MakeArray();
    i64 activeCameraIndex = -1;
    for (usize i = 0; i < nodes.size(); ++i) {
        const ecs::Entity entity = nodes[i];
        JsonValue json = JsonValue::MakeObject();

        i64 parentIndex = -1;
        const ecs::Entity parent = scene.Parent(entity);
        if (parent.IsValid()) {
            const auto found = std::find(nodes.begin(), nodes.end(), parent);
            if (found != nodes.end()) {
                parentIndex = static_cast<i64>(found - nodes.begin());
            }
        }
        json.Set("parent", JsonValue(parentIndex));

        const NameComponent* name = scene.World().Get<NameComponent>(entity);
        json.Set("name", JsonValue(name != nullptr ? name->name : std::string{}));

        if (const math::Transform* local = scene.LocalTransform(entity)) {
            json.Set("position", Vec3ToJson(local->position));
            JsonValue rotation = JsonValue::MakeArray();
            rotation.Push(JsonValue(static_cast<f64>(local->rotation.x)));
            rotation.Push(JsonValue(static_cast<f64>(local->rotation.y)));
            rotation.Push(JsonValue(static_cast<f64>(local->rotation.z)));
            rotation.Push(JsonValue(static_cast<f64>(local->rotation.w)));
            json.Set("rotation", std::move(rotation));
            json.Set("scale", Vec3ToJson(local->scale));
        }

        if (const auto* renderer = scene.World().Get<MeshRendererComponent>(entity)) {
            json.Set("meshRenderer", MeshRendererToJson(*renderer));
        }
        if (const auto* light = scene.World().Get<LightComponent>(entity)) {
            json.Set("light", LightToJson(*light));
        }
        if (const auto* camera = scene.World().Get<CameraComponent>(entity)) {
            json.Set("camera", CameraToJson(*camera));
            if (scene.ActiveCamera() == entity) {
                activeCameraIndex = static_cast<i64>(i);
            }
        }
        nodeArray.Push(std::move(json));
    }
    root.Set("nodes", std::move(nodeArray));
    root.Set("activeCamera", JsonValue(activeCameraIndex));

    return root.Dump(indent);
}

Result<void> SceneSerializer::FromJson(std::string_view text, Scene& out) {
    auto parsed = JsonValue::Parse(text);
    if (parsed.IsError()) {
        return Unexpected(parsed.Error());
    }
    const JsonValue& root = *parsed;
    if (!root.IsObject()) {
        return Unexpected(Status{StatusCode::ParseError, "Scene file must be a JSON object"});
    }

    const i64 format = root["format"].AsInt(kFormatVersion);
    if (format > static_cast<i64>(kFormatVersion)) {
        return Unexpected(Status{StatusCode::Unsupported, "Scene file is from a newer version"});
    }
    const JsonValue& nodeArray = root["nodes"];
    if (!nodeArray.IsArray()) {
        return Unexpected(Status{StatusCode::ParseError, "Scene file has no node array"});
    }

    out.Clear();
    out.SetName(std::string(root["name"].AsString(out.Name())));

    // Pass one: create every node unparented.  Parents can have a higher entity
    // index than their children, so the links cannot be made while creating.
    std::vector<ecs::Entity> entities;
    entities.reserve(nodeArray.Size());
    for (usize i = 0; i < nodeArray.Size(); ++i) {
        const JsonValue& node = nodeArray[i];
        auto created = out.CreateNode(std::string(node["name"].AsString()));
        if (created.IsError()) {
            return Unexpected(created.Error());
        }
        entities.push_back(*created);
    }

    // Pass two: the hierarchy.
    for (usize i = 0; i < nodeArray.Size(); ++i) {
        const i64 parentIndex = nodeArray[i]["parent"].AsInt(-1);
        if (parentIndex < 0) {
            continue;
        }
        if (static_cast<usize>(parentIndex) >= entities.size()) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Node parent index out of range"});
        }
        auto reparented = out.SetParent(entities[i], entities[static_cast<usize>(parentIndex)]);
        if (reparented.IsError()) {
            return Unexpected(reparented.Error());
        }
    }

    // Pass three: transforms and components.
    for (usize i = 0; i < nodeArray.Size(); ++i) {
        const JsonValue& node = nodeArray[i];
        const ecs::Entity entity = entities[i];

        math::Transform local;
        local.position = JsonToVec3(node["position"], local.position);
        local.rotation = JsonToQuaternion(node["rotation"]);
        local.scale = JsonToVec3(node["scale"], local.scale);
        auto transformed = out.SetLocalTransform(entity, local);
        if (transformed.IsError()) {
            return Unexpected(transformed.Error());
        }

        if (node.Contains("meshRenderer")) {
            auto renderer = MeshRendererFromJson(node["meshRenderer"]);
            if (renderer.IsError()) {
                return Unexpected(renderer.Error());
            }
            out.World().Emplace<MeshRendererComponent>(entity, std::move(*renderer));
        }
        if (node.Contains("light")) {
            out.World().Emplace<LightComponent>(entity, LightFromJson(node["light"]));
        }
        if (node.Contains("camera")) {
            out.World().Emplace<CameraComponent>(entity, CameraFromJson(node["camera"]));
        }
    }

    const i64 activeCameraIndex = root["activeCamera"].AsInt(-1);
    if (activeCameraIndex >= 0) {
        if (static_cast<usize>(activeCameraIndex) >= entities.size()) {
            return Unexpected(Status{StatusCode::InvalidArgument, "Active camera index out of range"});
        }
        auto activated = out.SetActiveCamera(entities[static_cast<usize>(activeCameraIndex)]);
        if (activated.IsError()) {
            return Unexpected(activated.Error());
        }
    }

    out.UpdateTransforms();
    return {};
}

} // namespace l3d::scene
