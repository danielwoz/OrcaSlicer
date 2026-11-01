#include <catch2/catch_all.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Exception.hpp"

using namespace Slic3r;

// GCode::encode_label_ids_to_base64 turns a set of label object ids into the base64 bitset of an M624
// line, indexing each id by its position in the sorted list of known ids.

TEST_CASE("An empty label id set encodes to an empty string", "[GCode]")
{
    CHECK(GCode::encode_label_ids_to_base64({}, {}).empty());
    CHECK(GCode::encode_label_ids_to_base64({}, { 0, 1, 2, 3 }).empty());
}

TEST_CASE("Known label ids encode to a set-like bitset", "[GCode]")
{
    const std::vector<size_t> known{ 0, 1, 2, 3 };

    CHECK_FALSE(GCode::encode_label_ids_to_base64({ 0 }, known).empty());
    CHECK_FALSE(GCode::encode_label_ids_to_base64({ 3 }, known).empty());
    // Repeating an id does not change the encoding.
    CHECK(GCode::encode_label_ids_to_base64({ 1 }, known) == GCode::encode_label_ids_to_base64({ 1, 1, 1 }, known));
    // Different sets encode differently.
    const std::string only_0 = GCode::encode_label_ids_to_base64({ 0 }, known);
    const std::string only_1 = GCode::encode_label_ids_to_base64({ 1 }, known);
    const std::string both   = GCode::encode_label_ids_to_base64({ 0, 1 }, known);
    CHECK(only_0 != only_1);
    CHECK(only_0 != both);
    CHECK(only_1 != both);
}

TEST_CASE("An unknown label id is rejected", "[GCode]")
{
    const std::vector<size_t> known{ 0, 1, 2 };
    CHECK_THROWS_AS(GCode::encode_label_ids_to_base64({ 99 }, known), Slic3r::LogicError);
    CHECK_THROWS_AS(GCode::encode_label_ids_to_base64({ 0, 5 }, known), Slic3r::LogicError);
}
