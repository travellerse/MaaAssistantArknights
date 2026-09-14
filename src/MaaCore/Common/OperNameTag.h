#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>

namespace asst::battle
{
enum class Role
{
    Unknown,
    Pioneer, // 先锋
    Warrior, // 近卫
    Tank,    // 重装
    Sniper,  // 狙击
    Caster,  // 术士
    Medic,   // 医疗
    Support, // 辅助
    Special, // 特种
    Drone    // 无人机
};

struct OperNameTag
{
    Role role = Role::Unknown; // 干员职业
    std::string name;          // 干员名

    auto operator<=>(const OperNameTag&) const = default;

    std::string to_string() const;

    explicit operator std::string() const;
};
} // namespace asst::battle

namespace asst
{
inline std::string enum_to_string(const battle::Role role, const bool en = false)
{
    using battle::Role;
    switch (role) {
    case Role::Pioneer:
        return en ? "Pioneer" : "先锋";
    case Role::Warrior:
        return en ? "Warrior" : "近卫";
    case Role::Tank:
        return en ? "Tank" : "重装";
    case Role::Sniper:
        return en ? "Sniper" : "狙击";
    case Role::Caster:
        return en ? "Caster" : "术师";
    case Role::Medic:
        return en ? "Medic" : "医疗";
    case Role::Support:
        return en ? "Support" : "辅助";
    case Role::Special:
        return en ? "Special" : "特种";
    case Role::Drone:
        return en ? "Drone" : "无人机";
    case Role::Unknown:
        return "Unknown";
    }
    return "Unknown";
}
} // namespace asst

namespace asst::battle
{
inline std::string OperNameTag::to_string() const
{
    return "(" + asst::enum_to_string(role) + ", " + name + ")";
}

inline OperNameTag::operator std::string() const
{
    return to_string();
}
} // namespace asst::battle

namespace std
{
template <>
struct hash<asst::battle::OperNameTag>
{
    std::size_t operator()(const asst::battle::OperNameTag& k) const noexcept
    {
        return std::hash<std::string> {}(k.name) ^ (std::hash<int> {}(static_cast<int>(k.role)) << 1);
    }
};
} // namespace std
