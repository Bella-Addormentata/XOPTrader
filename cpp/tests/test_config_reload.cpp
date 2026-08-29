// ---------------------------------------------------------------------------
// [RELOAD] Config hot-reload diff: what a saved config may change live.
//
// The contract under test: a pair going enabled -> disabled is the ONLY
// live-applicable change; enables and structural changes are reported, not
// applied. The diff must be exact -- a wrong name in to_disable would
// cancel the book of a healthy pair.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/config_reload.hpp"

#include <string>
#include <vector>

namespace {

// The template only requires .name and .enabled -- the engine instantiates
// it with the real PairConfig, so field compatibility is enforced at
// compile time in engine.cpp; behaviour is verified here.
struct Pair {
    std::string name;
    bool enabled{true};
};

std::vector<Pair> pairs(std::initializer_list<Pair> ps) { return ps; }

}  // namespace

TEST(ConfigReloadDiff, identical_configs_change_nothing)
{
    const auto running = pairs({{"A/XCH", true}, {"B/XCH", false}});
    const auto d = xop::diff_pair_enables(running, running);
    EXPECT_TRUE(d.to_disable.empty());
    EXPECT_TRUE(d.to_enable.empty());
    EXPECT_TRUE(d.structural.empty());
}

TEST(ConfigReloadDiff, a_disable_is_detected_by_name)
{
    const auto running = pairs({{"A/XCH", true}, {"B/XCH", true}});
    const auto saved   = pairs({{"A/XCH", true}, {"B/XCH", false}});
    const auto d = xop::diff_pair_enables(running, saved);
    ASSERT_EQ(d.to_disable.size(), 1u);
    EXPECT_EQ(d.to_disable[0], "B/XCH");
    EXPECT_TRUE(d.to_enable.empty());
    EXPECT_TRUE(d.structural.empty());
}

TEST(ConfigReloadDiff, an_enable_is_reported_not_applied_material)
{
    // The diff itself is symmetric; the ASYMMETRY (apply vs report) lives
    // in the engine. This pins that an enable lands in to_enable and never
    // leaks into to_disable.
    const auto running = pairs({{"A/XCH", false}});
    const auto saved   = pairs({{"A/XCH", true}});
    const auto d = xop::diff_pair_enables(running, saved);
    EXPECT_TRUE(d.to_disable.empty());
    ASSERT_EQ(d.to_enable.size(), 1u);
    EXPECT_EQ(d.to_enable[0], "A/XCH");
}

TEST(ConfigReloadDiff, order_does_not_matter_names_do)
{
    const auto running = pairs({{"A/XCH", true}, {"B/XCH", true}});
    const auto saved   = pairs({{"B/XCH", false}, {"A/XCH", true}});
    const auto d = xop::diff_pair_enables(running, saved);
    ASSERT_EQ(d.to_disable.size(), 1u);
    EXPECT_EQ(d.to_disable[0], "B/XCH");
}

TEST(ConfigReloadDiff, a_removed_pair_is_structural_not_a_disable)
{
    const auto running = pairs({{"A/XCH", true}, {"B/XCH", true}});
    const auto saved   = pairs({{"A/XCH", true}});
    const auto d = xop::diff_pair_enables(running, saved);
    EXPECT_TRUE(d.to_disable.empty()) <<
        "a pair deleted from the file must NOT be treated as a live "
        "disable -- it still holds subsystem seats and needs a restart";
    ASSERT_EQ(d.structural.size(), 1u);
    EXPECT_EQ(d.structural[0], "B/XCH");
}

TEST(ConfigReloadDiff, an_added_pair_is_structural)
{
    const auto running = pairs({{"A/XCH", true}});
    const auto saved   = pairs({{"A/XCH", true}, {"C/XCH", true}});
    const auto d = xop::diff_pair_enables(running, saved);
    EXPECT_TRUE(d.to_enable.empty());
    ASSERT_EQ(d.structural.size(), 1u);
    EXPECT_EQ(d.structural[0], "C/XCH");
}

TEST(ConfigReloadDiff, mixed_changes_are_each_classified)
{
    const auto running = pairs({
        {"A/XCH", true},    // stays enabled
        {"B/XCH", true},    // disabled in saved
        {"C/XCH", false},   // enabled in saved
        {"D/XCH", true},    // removed in saved
    });
    const auto saved = pairs({
        {"A/XCH", true},
        {"B/XCH", false},
        {"C/XCH", true},
        {"E/XCH", true},    // added in saved
    });
    const auto d = xop::diff_pair_enables(running, saved);
    ASSERT_EQ(d.to_disable.size(), 1u);
    EXPECT_EQ(d.to_disable[0], "B/XCH");
    ASSERT_EQ(d.to_enable.size(), 1u);
    EXPECT_EQ(d.to_enable[0], "C/XCH");
    ASSERT_EQ(d.structural.size(), 2u);
    EXPECT_EQ(d.structural[0], "D/XCH");
    EXPECT_EQ(d.structural[1], "E/XCH");
}

TEST(ConfigReloadDiff, empty_saved_pairs_is_all_structural_never_disables)
{
    // A truncated or half-written file that parses to zero pairs must not
    // read as "disable everything".
    const auto running = pairs({{"A/XCH", true}, {"B/XCH", true}});
    const auto d = xop::diff_pair_enables(running, pairs({}));
    EXPECT_TRUE(d.to_disable.empty());
    EXPECT_EQ(d.structural.size(), 2u);
}
