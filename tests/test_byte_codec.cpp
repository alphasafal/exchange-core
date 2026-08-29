#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "xc/util/byte_codec.hpp"

namespace xc {
namespace {

TEST(ByteCodec, RoundTripsEveryWidth) {
    std::vector<std::uint8_t> buffer;
    ByteWriter writer(buffer);
    writer.u8(0xAB);
    writer.u16(0xBEEF);
    writer.u32(0xDEADBEEF);
    writer.u64(0x0123456789ABCDEFULL);
    writer.i64(-9'223'372'036'854'775'807LL);
    writer.boolean(true);
    writer.string("AAPL");

    ByteReader reader(buffer);
    EXPECT_EQ(reader.u8(), 0xAB);
    EXPECT_EQ(reader.u16(), 0xBEEF);
    EXPECT_EQ(reader.u32(), 0xDEADBEEFU);
    EXPECT_EQ(reader.u64(), 0x0123456789ABCDEFULL);
    EXPECT_EQ(reader.i64(), -9'223'372'036'854'775'807LL);
    EXPECT_TRUE(reader.boolean());
    EXPECT_EQ(reader.string(), "AAPL");
    EXPECT_TRUE(reader.fully_consumed());
}

TEST(ByteCodec, EncodesLittleEndianRegardlessOfHost) {
    std::vector<std::uint8_t> buffer;
    ByteWriter writer(buffer);
    writer.u32(0x01020304);

    // Pinned explicitly. If this ever depended on the host's byte order, a
    // journal written on one machine would be unreadable on another and the
    // failure would look like corruption.
    ASSERT_EQ(buffer.size(), 4u);
    EXPECT_EQ(buffer[0], 0x04);
    EXPECT_EQ(buffer[1], 0x03);
    EXPECT_EQ(buffer[2], 0x02);
    EXPECT_EQ(buffer[3], 0x01);
}

TEST(ByteCodec, RoundTripsExtremeSignedValues) {
    std::vector<std::uint8_t> buffer;
    ByteWriter writer(buffer);
    writer.i64(std::numeric_limits<std::int64_t>::min());
    writer.i64(std::numeric_limits<std::int64_t>::max());
    writer.i64(-1);
    writer.i64(0);

    ByteReader reader(buffer);
    EXPECT_EQ(reader.i64(), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(reader.i64(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(reader.i64(), -1);
    EXPECT_EQ(reader.i64(), 0);
    EXPECT_TRUE(reader.fully_consumed());
}

TEST(ByteCodec, ReadingPastTheEndFailsAndStaysFailed) {
    const std::vector<std::uint8_t> buffer{1, 2, 3};
    ByteReader reader(buffer);

    EXPECT_EQ(reader.u16(), 0x0201);
    EXPECT_TRUE(reader.ok());

    // Only one byte is left, so this cannot be satisfied.
    EXPECT_EQ(reader.u32(), 0u);
    EXPECT_FALSE(reader.ok());

    // Failure latches. A decoder written as a run of reads followed by one
    // check must not be able to recover partway through and produce a
    // half-parsed message that looks valid.
    EXPECT_EQ(reader.u8(), 0u);
    EXPECT_FALSE(reader.ok());
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(ByteCodec, ALyingStringLengthCannotReadPastTheBuffer) {
    // A 16-bit length of 60000 followed by three bytes: the shape of a hostile
    // or corrupt message. It must fail rather than read whatever follows in
    // memory.
    const std::vector<std::uint8_t> buffer{0x60, 0xEA, 'a', 'b', 'c'};
    ByteReader reader(buffer);
    EXPECT_TRUE(reader.string().empty());
    EXPECT_FALSE(reader.ok());
}

TEST(ByteCodec, RejectsABooleanThatIsNeitherZeroNorOne) {
    const std::vector<std::uint8_t> buffer{2};
    ByteReader reader(buffer);
    // A byte that is neither did not come from this encoder, and accepting it
    // would let a malformed message become a plausible one.
    EXPECT_FALSE(reader.boolean());
    EXPECT_FALSE(reader.ok());
}

TEST(ByteCodec, RejectsAnEnumerationOutsideItsRange) {
    enum class Colour : std::uint8_t { Red = 0, Green = 1, Blue = 2 };
    const std::vector<std::uint8_t> buffer{2, 3};

    ByteReader reader(buffer);
    EXPECT_EQ(reader.enumeration<Colour>(2), Colour::Blue);
    EXPECT_TRUE(reader.ok());

    // Casting 3 would produce a value no switch handles, and the behaviour
    // would depend on which branch happened to fall through.
    reader.enumeration<Colour>(2);
    EXPECT_FALSE(reader.ok());
}

TEST(ByteCodec, TrailingBytesAreNotFullyConsumed) {
    const std::vector<std::uint8_t> buffer{1, 2, 3, 4, 5};
    ByteReader reader(buffer);
    reader.u32();
    EXPECT_TRUE(reader.ok());

    // Leftover bytes usually mean a newer sender appended a field. Treating the
    // message as valid would silently ignore it.
    EXPECT_FALSE(reader.fully_consumed());
    EXPECT_EQ(reader.remaining(), 1u);
}

TEST(ByteCodec, SurvivesAnEmptyBuffer) {
    ByteReader reader(std::span<const std::uint8_t>{});
    EXPECT_TRUE(reader.ok()) << "reading nothing from nothing has not failed yet";
    EXPECT_TRUE(reader.fully_consumed());
    EXPECT_EQ(reader.u8(), 0u);
    EXPECT_FALSE(reader.ok());
}

TEST(ByteCodec, WriterReportsOnlyWhatItAppended) {
    std::vector<std::uint8_t> buffer{9, 9, 9};
    ByteWriter writer(buffer);
    writer.u32(0);
    // Reports its own contribution, not the buffer's total, so a caller
    // appending to a shared buffer can compute its own payload length.
    EXPECT_EQ(writer.written(), 4u);
    EXPECT_EQ(buffer.size(), 7u);
}

}  // namespace
}  // namespace xc
