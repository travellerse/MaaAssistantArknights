#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Utils/Algorithm.hpp"

namespace
{
using Oper = asst::battle::OperNameTag;
using GroupList = std::unordered_map<Oper, std::vector<Oper>>;
using CharSet = std::unordered_set<Oper>;

Oper op(std::string name, asst::battle::Role role = asst::battle::Role::Unknown)
{
    return { .role = role, .name = std::move(name) };
}

void require_valid_allocation(const GroupList& group_list, const CharSet& char_set,
                              const asst::algorithm::CharAllocationResult& result)
{
    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::Success);
    REQUIRE(result.has_value());
    REQUIRE(result.allocation.size() == group_list.size());

    for (const auto& [group_name, allocated_char] : result.allocation) {
        INFO("group_name=" << group_name.name);
        REQUIRE(group_list.contains(group_name));
        REQUIRE(char_set.contains(allocated_char));
    }

    std::unordered_set<Oper> used_chars;
    for (const auto& [group_name, candidates] : group_list) {
        INFO("group_name=" << group_name.name);

        const auto allocation_it = result.allocation.find(group_name);
        REQUIRE(allocation_it != result.allocation.end());

        const auto& assigned_char = allocation_it->second;
        REQUIRE(char_set.contains(assigned_char));
        REQUIRE(std::ranges::find(candidates, assigned_char) != candidates.end());
        REQUIRE(used_chars.emplace(assigned_char).second);
    }
}
} // namespace

TEST_CASE("Empty group list returns empty success result")
{
    const GroupList groups;
    const CharSet chars { op("Amiya") };

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::Success);
    REQUIRE(result.has_value());
    REQUIRE(result.allocation.empty());
}

TEST_CASE("Empty char set returns no solution")
{
    const GroupList groups { { op("先锋"), { op("德克萨斯") } } };
    const CharSet chars;

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::NoSolution);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Exact matching returns expected allocation")
{
    const GroupList groups {
        { op("先锋"), { op("德克萨斯") } },
        { op("术师"), { op("阿米娅") } },
    };
    const CharSet chars { op("德克萨斯"), op("阿米娅") };

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::Success);
    REQUIRE(result.has_value());
    REQUIRE(result.allocation == std::unordered_map<Oper, Oper> {
                                   { op("先锋"), op("德克萨斯") },
                                   { op("术师"), op("阿米娅") },
                               });
}

TEST_CASE("Role is part of an operator allocation identity")
{
    const GroupList groups {
        { op("近卫"), { op("同名干员", asst::battle::Role::Warrior) } },
        { op("术师"), { op("同名干员", asst::battle::Role::Caster) } },
    };
    const CharSet chars {
        op("同名干员", asst::battle::Role::Warrior),
        op("同名干员", asst::battle::Role::Caster),
    };

    require_valid_allocation(groups, chars, asst::algorithm::get_char_allocation_for_each_group(groups, chars));
}

TEST_CASE("Duplicate candidates do not break matching")
{
    const GroupList groups {
        { op("先锋"), { op("德克萨斯"), op("德克萨斯") } },
        { op("术师"), { op("阿米娅"), op("阿米娅") } },
    };
    const CharSet chars { op("德克萨斯"), op("阿米娅") };

    require_valid_allocation(groups, chars, asst::algorithm::get_char_allocation_for_each_group(groups, chars));
}

TEST_CASE("Unowned candidates are filtered before matching")
{
    const GroupList groups {
        { op("先锋"), { op("风笛"), op("德克萨斯") } },
        { op("术师"), { op("刻俄柏"), op("阿米娅") } },
    };
    const CharSet chars { op("德克萨斯"), op("阿米娅") };

    require_valid_allocation(groups, chars, asst::algorithm::get_char_allocation_for_each_group(groups, chars));
}

TEST_CASE("Conflicting groups return no solution")
{
    const GroupList groups {
        { op("先锋"), { op("推进之王") } },
        { op("近卫"), { op("推进之王") } },
    };
    const CharSet chars { op("推进之王") };

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::NoSolution);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Multiple groups can find a valid allocation")
{
    const GroupList groups {
        { op("先锋"), { op("德克萨斯"), op("桃金娘") } },
        { op("术师"), { op("阿米娅"), op("伊芙利特") } },
        { op("医疗"), { op("闪灵"), op("夜莺") } },
    };
    const CharSet chars { op("桃金娘"), op("阿米娅"), op("夜莺") };

    require_valid_allocation(groups, chars, asst::algorithm::get_char_allocation_for_each_group(groups, chars));
}

TEST_CASE("Multiple groups allocation ignores extra owned chars")
{
    const GroupList groups {
        { op("先锋"), { op("德克萨斯"), op("桃金娘") } },
        { op("术师"), { op("阿米娅"), op("伊芙利特") } },
        { op("医疗"), { op("闪灵"), op("夜莺") } },
    };
    const CharSet chars {
        op("桃金娘"), op("阿米娅"), op("夜莺"), op("德克萨斯"), op("伊芙利特"), op("闪灵"), op("能天使")
    };

    require_valid_allocation(groups, chars, asst::algorithm::get_char_allocation_for_each_group(groups, chars));
}

TEST_CASE("Group without any owned candidate returns no solution")
{
    const GroupList groups {
        { op("先锋"), { op("德克萨斯") } },
        { op("术师"), { op("阿米娅") } },
    };
    const CharSet chars { op("德克萨斯") };

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::NoSolution);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Group with empty candidate list returns no solution")
{
    const GroupList groups {
        { op("先锋"), {} },
        { op("术师"), { op("阿米娅") } },
    };
    const CharSet chars { op("阿米娅") };

    const auto result = asst::algorithm::get_char_allocation_for_each_group(groups, chars);

    REQUIRE(result.status == asst::algorithm::CharAllocationStatus::NoSolution);
    REQUIRE_FALSE(result.has_value());
}
