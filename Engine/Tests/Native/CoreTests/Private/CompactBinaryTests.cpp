#include "Serialization/CompactBinary.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	auto Bytes(std::initializer_list<uint8> Values) -> FSharedByteBuffer
	{
		FByteBuffer Buffer;
		Buffer.reserve(Values.size());
		for (const uint8 Value : Values) Buffer.push_back(std::byte(Value));
		return FSharedByteBuffer::Take(std::move(Buffer));
	}
}

TEST(FCompactBinaryTests, CanonicalObjectHasGoldenLittleEndianBytes)
{
	FCbWriter Writer;
	ASSERT_TRUE(Writer.AddBool(true, "a"));
	auto Object = Writer.SaveObject();
	ASSERT_TRUE(Object);
	const auto Expected = Bytes({
		0x0a, 0, 0, 0, 0, 0x13, 0, 0, 0, 0, 0, 0, 0,
		1, 0, 0, 0,
		0x01, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 'a', 1});
	EXPECT_TRUE(std::ranges::equal(Object->GetBytes().GetBytes(), Expected.GetBytes()));
}

TEST(FCompactBinaryTests, RoundTripsAllKindsAndCanonicalizesObjectOrder)
{
	FCbWriter ArrayWriter;
	ASSERT_TRUE(ArrayWriter.AddNull());
	ASSERT_TRUE(ArrayWriter.AddString("item"));
	auto Array = ArrayWriter.SaveArray();
	ASSERT_TRUE(Array);

	FCbObjectId Id{};
	Id.Bytes[11] = 7;
	FCbWriter Writer;
	ASSERT_TRUE(Writer.AddArray(*Array, "z"));
	ASSERT_TRUE(Writer.AddUInt64(42, "u"));
	ASSERT_TRUE(Writer.AddInt64(-42, "i"));
	ASSERT_TRUE(Writer.AddFloat32(1.25f, "f32"));
	ASSERT_TRUE(Writer.AddFloat64(2.5, "f64"));
	ASSERT_TRUE(Writer.AddBinary(Bytes({1, 2, 3}), "bin"));
	ASSERT_TRUE(Writer.AddObjectId(Id, "id"));
	auto Object = Writer.SaveObject();
	ASSERT_TRUE(Object);

	auto Loaded = FCbObject::TryLoad(Object->GetBytes());
	ASSERT_TRUE(Loaded);
	const auto View = Loaded->GetView();
	EXPECT_EQ(View.Num(), 7u);
	EXPECT_EQ(View.Find("u").AsUInt64(), 42u);
	EXPECT_EQ(View.Find("i").AsInt64(), -42);
	EXPECT_EQ(View.Find("f32").AsFloat32(), 1.25f);
	EXPECT_EQ(View.Find("f64").AsFloat64(), 2.5);
	EXPECT_EQ(View.Find("id").AsObjectId(), Id);
	EXPECT_EQ(View.Find("z").AsArray().At(1).AsString(), "item");
	const auto Fields = View.GetFields();
	for (size_t Index = 1; Index < Fields.size(); ++Index)
		EXPECT_LT(Fields[Index - 1].GetName(), Fields[Index].GetName());
}

TEST(FCompactBinaryTests, ViewsRetainOwnedStorage)
{
	std::optional<FSharedByteBuffer> Payload;
	{
		FCbWriter Writer;
		ASSERT_TRUE(Writer.AddBinary(Bytes({4, 5, 6}), "payload"));
		auto Object = Writer.SaveObject();
		ASSERT_TRUE(Object);
		const auto Owner = Object->GetBytes();
		Payload = Object->GetView().Find("payload").AsBinary();
		ASSERT_TRUE(Payload);
		EXPECT_TRUE(Payload->SharesStorageWith(Owner));
	}
	ASSERT_TRUE(Payload);
	EXPECT_TRUE(std::ranges::equal(Payload->GetBytes(), Bytes({4, 5, 6}).GetBytes()));
}

TEST(FCompactBinaryTests, RejectsMalformedNonCanonicalAndBoundedData)
{
	EXPECT_FALSE(FCbField::TryLoad(Bytes({0x0a})));
	FCbWriter InvalidUtf8;
	EXPECT_FALSE(InvalidUtf8.AddString(std::string("\xed\xa0\x80", 3), "x"));
	EXPECT_FALSE(InvalidUtf8.SaveObject());
	FCbWriter InvalidFloat;
	EXPECT_FALSE(InvalidFloat.AddFloat64(std::numeric_limits<double>::infinity(), "x"));
	EXPECT_FALSE(InvalidFloat.SaveObject());

	FCbWriter Duplicate;
	ASSERT_TRUE(Duplicate.AddBool(true, "a"));
	ASSERT_TRUE(Duplicate.AddBool(false, "a"));
	EXPECT_FALSE(Duplicate.SaveObject());

	FCbWriter NamedArray;
	ASSERT_TRUE(NamedArray.AddBool(true, "named"));
	EXPECT_FALSE(NamedArray.SaveArray());

	FCbWriter Limited({.MaximumEncodedBytes = 64, .MaximumDepth = 2,
		.MaximumFields = 1, .MaximumStringBytes = 3, .MaximumBinaryBytes = 2});
	EXPECT_FALSE(Limited.AddString("four", "x"));
	EXPECT_FALSE(Limited.SaveObject());

	FCbWriter PayloadLimited({.MaximumEncodedBytes = 64, .MaximumDepth = 2,
		.MaximumFields = 2, .MaximumStringBytes = 8, .MaximumBinaryBytes = 2});
	EXPECT_FALSE(PayloadLimited.AddBinary(Bytes({1, 2, 3}), "x"));
}
