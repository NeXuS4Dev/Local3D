#include "local3d/reflection/Reflection.hpp"

namespace l3d::reflect {

TypeRegistry& TypeRegistry::Instance() noexcept {
    static TypeRegistry registry;
    return registry;
}

const EnumInfo& TypeRegistry::RegisterEnum(std::string_view name,
                                           std::vector<std::pair<i64, std::string>> entries) {
    const std::string key(name);
    auto found = enums_.find(key);
    if (found != enums_.end()) {
        return *found->second;
    }
    auto info = std::make_unique<EnumInfo>();
    info->name = key;
    info->entries = std::move(entries);
    EnumInfo* raw = info.get();
    enums_.emplace(std::move(key), std::move(info));
    return *raw;
}

const EnumInfo* TypeRegistry::FindEnum(std::string_view name) const noexcept {
    const auto found = enums_.find(std::string(name));
    return found != enums_.end() ? found->second.get() : nullptr;
}

std::vector<const TypeInfo*> TypeRegistry::AllTypes() const {
    std::vector<const TypeInfo*> types;
    types.reserve(types_.size());
    for (const auto& entry : types_) {
        types.push_back(entry.second.get());
    }
    return types;
}

void TypeRegistry::Clear() {
    types_.clear();
    byName_.clear();
    enums_.clear();
}

} // namespace l3d::reflect
