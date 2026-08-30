#include <gtest/gtest.h>

#include "PackageObjectStreamReferenceModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	using namespace Durin;
	using namespace Durin::Testing::PackageObjectStream;

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		Result.reserve(Values.size());
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto Scalar(ETypeOpcode Opcode) -> FTypePtr
	{
		return MakeType(Opcode);
	}

	auto Freeze(const FTableInput& Input) -> FFrozenTables
	{
		FFrozenTables Tables;
		std::string Error;
		EXPECT_TRUE(FreezeTables(Input, Tables, Error)) << Error;
		return Tables;
	}

	auto MakeComprehensiveInput() -> FTableInput
	{
		const FTypePtr I32 = Scalar(ETypeOpcode::I32);
		const FTypePtr String = Scalar(ETypeOpcode::String);
		const FTypePtr Inner = MakeType(ETypeOpcode::Struct, "Example::Inner");
		const FTypePtr ArrayI32 = MakeType(ETypeOpcode::Array, "", 0, {I32});
		const FTypePtr NestedArray = MakeType(ETypeOpcode::Array, "", 0, {ArrayI32});
		const FTypePtr FixedI32 = MakeType(ETypeOpcode::FixedArray, "", 2, {I32});
		const FTypePtr Map = MakeType(ETypeOpcode::Map, "", 0, {String, I32});
		FTableInput Input;
		Input.PublicDependencyCount = 2;
		Input.AdditionalNames = {"/Game/Soft", "NamedValue"};
		for (uint8 Opcode = uint8(ETypeOpcode::Bool); Opcode <= uint8(ETypeOpcode::Bytes); ++Opcode)
		{
			const ETypeOpcode TypeOpcode = ETypeOpcode(Opcode);
			if (TypeOpcode == ETypeOpcode::Enum)
				Input.Types.push_back(MakeType(TypeOpcode, "Example::Mode", uint8(ETypeOpcode::I16)));
			else if (TypeOpcode == ETypeOpcode::Intrinsic)
				for (uint8 Layout = 1; Layout <= 6; ++Layout)
					Input.Types.push_back(MakeType(TypeOpcode, "", Layout));
			else if (TypeOpcode == ETypeOpcode::Struct)
				Input.Types.push_back(Inner);
			else if (TypeOpcode == ETypeOpcode::FixedArray)
				Input.Types.push_back(FixedI32);
			else if (TypeOpcode == ETypeOpcode::Array)
				Input.Types.push_back(ArrayI32);
			else if (TypeOpcode == ETypeOpcode::Map)
				Input.Types.push_back(Map);
			else if (TypeOpcode == ETypeOpcode::HardRef || TypeOpcode == ETypeOpcode::SoftRef)
				Input.Types.push_back(MakeType(TypeOpcode, "DObject"));
			else
				Input.Types.push_back(Scalar(TypeOpcode));
		}
		Input.Types.push_back(NestedArray);
		Input.Schemas = {
			{"Example::Inner", {{"X", I32, 0}, {"Label", String, 0}}},
			{"Example::Asset", {{"Inner", Inner, 0}, {"Values", ArrayI32, 0}, {"Lookup", Map, 0}}},
		};
		Input.CustomVersions = {
			{{2, 0, 0, 0}, 7},
			{{1, 9, 0, 0}, 3},
		};
		Input.Objects = {
			{"Root/Child", "Root", "Example::Child", "Child"},
			{"Root", "", "Example::Asset", "Root"},
		};
		return Input;
	}

	auto Encode(const FTypePtr& Type, const FValue& Value, const FFrozenTables& Tables) -> std::vector<std::byte>
	{
		std::vector<std::byte> Encoded;
		std::string Error;
		EXPECT_TRUE(EncodeValue(*Type, Value, Tables, Encoded, Error)) << Error;
		return Encoded;
	}

	auto ExpectDecodeFailure(
		const FTypePtr& Type,
		std::span<const std::byte> BytesValue,
		const FFrozenTables& Tables,
		std::string_view Category) -> void
	{
		FValue Value;
		std::string Error;
		EXPECT_FALSE(DecodeValue(*Type, BytesValue, Tables, Value, Error));
		EXPECT_NE(Error.find(Category), std::string::npos) << Error;
	}
}

TEST(FPackageObjectStreamReferenceModelTests, MinimalCanonicalTablesHaveExactGoldenBytes)
{
	const FTypePtr Bool = Scalar(ETypeOpcode::Bool);
	FTableInput Input;
	Input.Types = {Bool};
	Input.Schemas = {{"A", {{"Field", Bool, 0}}}};
	const FFrozenTables Tables = Freeze(Input);
	std::array<std::vector<std::byte>, 4> Sections;
	std::string Error;
	ASSERT_TRUE(EncodeTableSections(Tables, Sections, Error)) << Error;
	EXPECT_EQ(Sections[0], Bytes({0x02, 0x01, 0x41, 0x05, 0x46, 0x69, 0x65, 0x6c, 0x64}));
	EXPECT_EQ(Sections[1], Bytes({0x01, 0x01, 0x01}));
	EXPECT_EQ(Sections[2], Bytes({0x00, 0x01, 0x05, 0x01, 0x01, 0x02, 0x01, 0x00}));
	EXPECT_EQ(Sections[3], Bytes({0x00}));

	FFrozenTables Decoded;
	ASSERT_TRUE(DecodeTableSections(Sections, Decoded, Error)) << Error;
	std::array<std::vector<std::byte>, 4> Reencoded;
	ASSERT_TRUE(EncodeTableSections(Decoded, Reencoded, Error)) << Error;
	EXPECT_EQ(Reencoded, Sections);
}

TEST(FPackageObjectStreamReferenceModelTests, DiscoveryOrderCannotChangeTablesOrIds)
{
	FTableInput Forward = MakeComprehensiveInput();
	FTableInput Reverse = Forward;
	std::reverse(Reverse.AdditionalNames.begin(), Reverse.AdditionalNames.end());
	std::reverse(Reverse.Types.begin(), Reverse.Types.end());
	std::reverse(Reverse.Schemas.begin(), Reverse.Schemas.end());
	for (FSchemaDescriptor& Schema : Reverse.Schemas)
		std::reverse(Schema.Fields.begin(), Schema.Fields.end());
	std::reverse(Reverse.CustomVersions.begin(), Reverse.CustomVersions.end());
	std::reverse(Reverse.Objects.begin(), Reverse.Objects.end());

	const FFrozenTables A = Freeze(Forward);
	const FFrozenTables B = Freeze(Reverse);
	EXPECT_EQ(A.Names.size(), 14);
	EXPECT_EQ(A.Types.size(), 29);
	EXPECT_EQ(A.Schemas.size(), 2);
	EXPECT_EQ(A.CustomVersions.size(), 2);
	EXPECT_EQ(A.Objects.size(), 2);
	std::array<std::vector<std::byte>, 4> ASections;
	std::array<std::vector<std::byte>, 4> BSections;
	std::string Error;
	ASSERT_TRUE(EncodeTableSections(A, ASections, Error)) << Error;
	ASSERT_TRUE(EncodeTableSections(B, BSections, Error)) << Error;
	EXPECT_EQ(ASections, BSections);
	EXPECT_EQ(A.Names, B.Names);
	EXPECT_EQ(A.SchemaId("Example::Asset"), B.SchemaId("Example::Asset"));
	EXPECT_EQ(A.FieldId(A.SchemaId("Example::Asset"), "Lookup"),
		B.FieldId(B.SchemaId("Example::Asset"), "Lookup"));
	EXPECT_EQ(A.ObjectId("Root/Child"), B.ObjectId("Root/Child"));

	FFrozenTables Decoded;
	ASSERT_TRUE(DecodeTableSections(ASections, Decoded, Error)) << Error;
	std::array<std::vector<std::byte>, 4> RoundTrip;
	ASSERT_TRUE(EncodeTableSections(Decoded, RoundTrip, Error)) << Error;
	EXPECT_EQ(RoundTrip, ASections);

	FDiscoveryRegistry Registry;
	ASSERT_TRUE(Registry.AddName("Before", Error));
	Registry.Freeze();
	EXPECT_FALSE(Registry.AddName("Late", Error));
	EXPECT_NE(Error.find("late discovery"), std::string::npos);
}

TEST(FPackageObjectStreamReferenceModelTests, ScalarsEnumsNamesGuidAndNaNsUseCanonicalPayloads)
{
	const FFrozenTables Tables = Freeze(MakeComprehensiveInput());
	struct FCase { FTypePtr Type; FValue Value; std::vector<std::byte> Golden; };
	FValue True; True.Bool = true;
	FValue MinusOne; MinusOne.Signed = -1;
	FValue U255; U255.Unsigned = 255;
	FValue NegativeZero; NegativeZero.Number = -0.0;
	FValue Infinity; Infinity.Number = std::numeric_limits<double>::infinity();
	FValue String; String.Text = "Hi";
	FValue Name; Name.Text = "NamedValue";
	FValue Guid; Guid.Guid = {0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00};
	FValue Enum; Enum.Signed = -2;
	FValue NotANumber; NotANumber.Number = std::numeric_limits<double>::quiet_NaN();
	FValue NotANumberF32 = NotANumber;
	const std::vector<FCase> Cases = {
		{Scalar(ETypeOpcode::Bool), True, Bytes({0x01})},
		{Scalar(ETypeOpcode::I8), MinusOne, Bytes({0x01})},
		{Scalar(ETypeOpcode::I16), MinusOne, Bytes({0x01})},
		{Scalar(ETypeOpcode::I32), MinusOne, Bytes({0x01})},
		{Scalar(ETypeOpcode::I64), MinusOne, Bytes({0x01})},
		{Scalar(ETypeOpcode::U8), U255, Bytes({0xff, 0x01})},
		{Scalar(ETypeOpcode::U16), U255, Bytes({0xff, 0x01})},
		{Scalar(ETypeOpcode::U32), U255, Bytes({0xff, 0x01})},
		{Scalar(ETypeOpcode::U64), U255, Bytes({0xff, 0x01})},
		{Scalar(ETypeOpcode::F32), NegativeZero, Bytes({0x00, 0x00, 0x00, 0x80})},
		{Scalar(ETypeOpcode::F64), Infinity, Bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x7f})},
		{Scalar(ETypeOpcode::String), String, Bytes({0x02, 0x48, 0x69})},
		{Scalar(ETypeOpcode::Guid), Guid, Bytes({
			0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
			0xcc, 0xbb, 0xaa, 0x99, 0x00, 0xff, 0xee, 0xdd})},
		{MakeType(ETypeOpcode::Enum, "Example::Mode", uint8(ETypeOpcode::I16)), Enum, Bytes({0x03})},
		{Scalar(ETypeOpcode::F32), NotANumberF32, Bytes({0x00, 0x00, 0xc0, 0x7f})},
		{Scalar(ETypeOpcode::F64), NotANumber, Bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x7f})},
	};
	for (const FCase& Case : Cases)
	{
		SCOPED_TRACE(uint8(Case.Type->Opcode));
		const std::vector<std::byte> Encoded = Encode(Case.Type, Case.Value, Tables);
		EXPECT_EQ(Encoded, Case.Golden);
		FValue Decoded;
		std::string Error;
		EXPECT_TRUE(DecodeValue(*Case.Type, Encoded, Tables, Decoded, Error)) << Error;
	}
	const std::vector<std::byte> EncodedName = Encode(Scalar(ETypeOpcode::Name), Name, Tables);
	FValue DecodedName;
	std::string Error;
	ASSERT_TRUE(DecodeValue(*Scalar(ETypeOpcode::Name), EncodedName, Tables, DecodedName, Error)) << Error;
	EXPECT_EQ(DecodedName.Text, Name.Text);
	ExpectDecodeFailure(Scalar(ETypeOpcode::F32), Bytes({0x01, 0x00, 0xc0, 0x7f}), Tables, "noncanonical");
}

TEST(FPackageObjectStreamReferenceModelTests, IntrinsicsContainersStructsAndReferencesRoundTripCanonically)
{
	const FFrozenTables Tables = Freeze(MakeComprehensiveInput());
	std::string Error;
	for (uint8 Layout = 1; Layout <= 6; ++Layout)
	{
		const FTypePtr Type = MakeType(ETypeOpcode::Intrinsic, "", Layout);
		FValue Value;
		const uint64 Count = Layout == 1 ? 2 : Layout == 2 ? 3 : Layout == 5 ? 10 : 4;
		for (uint64 Index = 0; Index < Count; ++Index)
			Value.Components.push_back(double(Index) + 0.25);
		const std::vector<std::byte> Encoded = Encode(Type, Value, Tables);
		EXPECT_EQ(Encoded.size(), Count * (Layout == 6 ? 4 : 8));
		FValue Decoded;
		ASSERT_TRUE(DecodeValue(*Type, Encoded, Tables, Decoded, Error)) << Error;
		EXPECT_EQ(Decoded.Components, Value.Components);
	}

	const FTypePtr I32 = Scalar(ETypeOpcode::I32);
	const FTypePtr String = Scalar(ETypeOpcode::String);
	const FTypePtr Fixed = MakeType(ETypeOpcode::FixedArray, "", 2, {I32});
	FValue FixedValue;
	FixedValue.Elements = {FValue{.Signed = -1}, FValue{.Signed = 1}};
	EXPECT_EQ(Encode(Fixed, FixedValue, Tables), Bytes({0x01, 0x02}));

	const FTypePtr Array = MakeType(ETypeOpcode::Array, "", 0, {I32});
	FValue ArrayValue;
	ArrayValue.Elements = {FValue{.Signed = 3}, FValue{.Signed = 4}};
	EXPECT_EQ(Encode(Array, ArrayValue, Tables), Bytes({0x02, 0x06, 0x08}));

	const FTypePtr Nested = MakeType(ETypeOpcode::Array, "", 0, {Array});
	FValue NestedValue;
	NestedValue.Elements = {ArrayValue, FValue{.Elements = {FValue{.Signed = 5}}}};
	const std::vector<std::byte> NestedBytes = Encode(Nested, NestedValue, Tables);
	FValue DecodedNested;
	ASSERT_TRUE(DecodeValue(*Nested, NestedBytes, Tables, DecodedNested, Error)) << Error;
	EXPECT_EQ(DecodedNested, NestedValue);

	const FTypePtr Map = MakeType(ETypeOpcode::Map, "", 0, {String, I32});
	FValue MapForward;
	MapForward.Elements = {
		FValue{.Text = "B"}, FValue{.Signed = 2},
		FValue{.Text = "A"}, FValue{.Signed = 1},
	};
	FValue MapReverse;
	MapReverse.Elements = {
		FValue{.Text = "A"}, FValue{.Signed = 1},
		FValue{.Text = "B"}, FValue{.Signed = 2},
	};
	const std::vector<std::byte> MapGolden = Bytes({0x02, 0x01, 0x41, 0x02, 0x01, 0x42, 0x04});
	EXPECT_EQ(Encode(Map, MapForward, Tables), MapGolden);
	EXPECT_EQ(Encode(Map, MapReverse, Tables), MapGolden);
	FValue DecodedMap;
	ASSERT_TRUE(DecodeValue(*Map, MapGolden, Tables, DecodedMap, Error)) << Error;
	EXPECT_EQ(DecodedMap, MapReverse);
	FValue DuplicateMap = MapForward;
	DuplicateMap.Elements[2].Text = "B";
	std::vector<std::byte> Rejected = Bytes({0xee});
	EXPECT_FALSE(EncodeValue(*Map, DuplicateMap, Tables, Rejected, Error));
	EXPECT_EQ(Rejected, Bytes({0xee}));
	EXPECT_NE(Error.find("duplicate"), std::string::npos);

	const FTypePtr Struct = MakeType(ETypeOpcode::Struct, "Example::Inner");
	FValue StructValue;
	StructValue.FieldIds = {Tables.FieldId(Tables.SchemaId("Example::Inner"), "X")};
	StructValue.Provenances = {0};
	StructValue.Elements = {FValue{.Signed = 7}};
	EXPECT_EQ(Encode(Struct, StructValue, Tables), Bytes({0x01, 0x02, 0x00, 0x01, 0x0e}));

	const FTypePtr HardRef = MakeType(ETypeOpcode::HardRef, "DObject");
	EXPECT_EQ(Encode(HardRef, FValue{}, Tables), Bytes({0x00}));
	EXPECT_EQ(Encode(HardRef, FValue{.ReferenceTag = 1, .ReferenceId = 1}, Tables),
		Bytes({0x01, 0x01}));
	EXPECT_EQ(Encode(HardRef, FValue{.ReferenceTag = 2, .ReferenceId = 1}, Tables),
		Bytes({0x02, 0x01}));
	std::vector<std::byte> InvalidReference = Bytes({0xee});
	EXPECT_FALSE(EncodeValue(*HardRef, FValue{.ReferenceTag = 2, .ReferenceId = 3},
		Tables, InvalidReference, Error));
	EXPECT_EQ(InvalidReference, Bytes({0xee}));
	ExpectDecodeFailure(HardRef, Bytes({0x02, 0x03}), Tables, "out of range");
	const FTypePtr SoftRef = MakeType(ETypeOpcode::SoftRef, "DObject");
	FValue SoftValue{.Text = "/Game/Soft", .ReferenceTag = 1};
	const std::vector<std::byte> SoftBytes = Encode(SoftRef, SoftValue, Tables);
	FValue DecodedSoft;
	ASSERT_TRUE(DecodeValue(*SoftRef, SoftBytes, Tables, DecodedSoft, Error)) << Error;
	EXPECT_EQ(DecodedSoft.Text, SoftValue.Text);

	FValue ByteValue;
	ByteValue.ByteData = Bytes({0xaa, 0xbb, 0xcc});
	EXPECT_EQ(Encode(Scalar(ETypeOpcode::Bytes), ByteValue, Tables),
		Bytes({0x03, 0xaa, 0xbb, 0xcc}));
}

TEST(FPackageObjectStreamReferenceModelTests, DefaultsExplicitAndForcedOverridesPreserveIntent)
{
	const FFrozenTables Tables = Freeze(MakeComprehensiveInput());
	FOverrideCandidate Candidate{
		.SchemaName = "Example::Inner",
		.FieldName = "X",
		.Value = FValue{.Signed = 0},
		.DefaultValue = FValue{.Signed = 0},
	};
	std::vector<std::byte> Encoded;
	std::string Error;
	ASSERT_TRUE(EncodeOverrideBlock(std::span(&Candidate, 1), Tables, Encoded, Error)) << Error;
	EXPECT_EQ(Encoded, Bytes({0x00}));

	Candidate.Value.Signed = 7;
	Candidate.DefaultValue.Signed = 5;
	ASSERT_TRUE(EncodeOverrideBlock(std::span(&Candidate, 1), Tables, Encoded, Error)) << Error;
	EXPECT_EQ(Encoded, Bytes({0x01, 0x02, 0x02, 0x00, 0x01, 0x0e}));

	Candidate.Value.Signed = 0;
	Candidate.DefaultValue.Signed = 0;
	Candidate.LoadedExplicit = true;
	ASSERT_TRUE(EncodeOverrideBlock(std::span(&Candidate, 1), Tables, Encoded, Error)) << Error;
	EXPECT_EQ(Encoded, Bytes({0x01, 0x02, 0x02, 0x00, 0x01, 0x00}));

	Candidate.LoadedExplicit = false;
	Candidate.Forced = true;
	ASSERT_TRUE(EncodeOverrideBlock(std::span(&Candidate, 1), Tables, Encoded, Error)) << Error;
	EXPECT_EQ(Encoded, Bytes({0x01, 0x02, 0x02, 0x01, 0x01, 0x00}));

	FOverrideCandidate NestedDefault{
		.SchemaName = "Example::Asset",
		.FieldName = "Inner",
	};
	ASSERT_TRUE(EncodeOverrideBlock(std::span(&NestedDefault, 1), Tables, Encoded, Error)) << Error;
	EXPECT_EQ(Encoded, Bytes({0x00}));

	for (const bool CustomSerializer : {false, true})
	{
		const FTypePtr Unsupported = MakeType(ETypeOpcode::Struct, "Example::Opaque");
		Unsupported->HasDeterministicStructOperations = CustomSerializer;
		Unsupported->HasCustomSerializer = CustomSerializer;
		FTableInput Input;
		Input.Schemas = {
			{"Example::Opaque", {}},
			{"Example::Owner", {{"Opaque", Unsupported, 0}}},
		};
		const FFrozenTables UnsupportedTables = Freeze(Input);
		FOverrideCandidate Opaque{
			.SchemaName = "Example::Owner",
			.FieldName = "Opaque",
		};
		Encoded = Bytes({0xee});
		EXPECT_FALSE(EncodeOverrideBlock(std::span(&Opaque, 1), UnsupportedTables, Encoded, Error));
		EXPECT_EQ(Encoded, Bytes({0xee}));
		EXPECT_NE(Error.find(CustomSerializer ? "custom serializer" : "operations"), std::string::npos);
	}
}

TEST(FPackageObjectStreamReferenceModelTests, ValueSectionOrdersObjectsAndValidatesKnownAndUnknownRecords)
{
	const FFrozenTables Tables = Freeze(MakeComprehensiveInput());
	FOverrideCandidate Changed{
		.SchemaName = "Example::Inner",
		.FieldName = "X",
		.Value = FValue{.Signed = 9},
		.DefaultValue = FValue{.Signed = 0},
	};
	const std::vector<FObjectValueInput> Forward = {
		{"Root", {Changed}},
		{"Root/Child", {}},
	};
	const std::vector<FObjectValueInput> Reverse = {
		{"Root/Child", {}},
		{"Root", {Changed}},
	};
	std::vector<std::byte> A;
	std::vector<std::byte> B;
	std::string Error;
	ASSERT_TRUE(EncodeValueSection(Forward, Tables, A, Error)) << Error;
	ASSERT_TRUE(EncodeValueSection(Reverse, Tables, B, Error)) << Error;
	EXPECT_EQ(A, B);
	EXPECT_TRUE(ValidateValueSection(A, Tables, Error)) << Error;
	EXPECT_EQ(A, Bytes({
		0x02,
		0x06, 0x01, 0x02, 0x02, 0x00, 0x01, 0x12,
		0x01, 0x00,
	}));

	std::vector<std::byte> Unchanged = Bytes({0xee});
	EXPECT_FALSE(EncodeValueSection(std::span(Forward).first(1), Tables, Unchanged, Error));
	EXPECT_EQ(Unchanged, Bytes({0xee}));
	const std::vector<FObjectValueInput> Duplicate = {Forward[0], Forward[0]};
	EXPECT_FALSE(EncodeValueSection(Duplicate, Tables, Unchanged, Error));
	EXPECT_NE(Error.find("duplicate"), std::string::npos);

	FTableInput MiniInput;
	const FTypePtr I32 = Scalar(ETypeOpcode::I32);
	MiniInput.Types = {I32};
	MiniInput.Schemas = {{"Unknown::Owner", {{"Mystery", I32, 0}}}};
	const FFrozenTables MiniTables = Freeze(MiniInput);
	std::vector<std::byte> Closure;
	ASSERT_TRUE(EncodeRetainedClosure(MiniTables, 1, 1, Closure, Error)) << Error;
	std::vector<std::byte> UnknownBody;
	ASSERT_TRUE(EncodeUnknownValueBody(Closure, Bytes({0xca, 0xfe}), UnknownBody, Error)) << Error;
	FWireWriter Block;
	Block.WriteVarUInt(1);
	Block.WriteVarUInt(Tables.SchemaId("Example::Inner"));
	Block.WriteVarUInt(Tables.FieldId(Tables.SchemaId("Example::Inner"), "X"));
	Block.WriteU8(2);
	Block.WriteVarUInt(UnknownBody.size());
	Block.WriteBytes(UnknownBody);
	FWireWriter UnknownSection;
	UnknownSection.WriteVarUInt(2);
	UnknownSection.WriteVarUInt(Block.Bytes().size());
	UnknownSection.WriteBytes(Block.Bytes());
	UnknownSection.WriteVarUInt(1);
	UnknownSection.WriteU8(0);
	EXPECT_TRUE(ValidateValueSection(UnknownSection.Bytes(), Tables, Error)) << Error;
	std::vector<std::byte> WithTrailing = UnknownSection.Bytes();
	WithTrailing.push_back(std::byte{0xff});
	EXPECT_FALSE(ValidateValueSection(WithTrailing, Tables, Error));
	EXPECT_NE(Error.find("unconsumed"), std::string::npos);
}

TEST(FPackageObjectStreamReferenceModelTests, UnknownPayloadAndDescriptorClosureRemainByteExact)
{
	const FTypePtr I32 = Scalar(ETypeOpcode::I32);
	FTableInput MiniInput;
	MiniInput.Types = {I32};
	MiniInput.Schemas = {{"Unknown::Owner", {{"Mystery", I32, 0}}}};
	const FFrozenTables MiniTables = Freeze(MiniInput);
	std::vector<std::byte> Closure;
	std::string Error;
	ASSERT_TRUE(EncodeRetainedClosure(MiniTables, 1, 1, Closure, Error)) << Error;
	EXPECT_TRUE(ValidateRetainedClosure(Closure, Error)) << Error;

	const std::vector<std::byte> Payload = Bytes({0x80, 0x00, 0xde, 0xad, 0xbe, 0xef});
	std::vector<std::byte> Body;
	ASSERT_TRUE(EncodeUnknownValueBody(Closure, Payload, Body, Error)) << Error;
	std::vector<std::byte> DecodedClosure;
	std::vector<std::byte> DecodedPayload;
	ASSERT_TRUE(ValidateUnknownValueBody(Body, DecodedClosure, DecodedPayload, Error)) << Error;
	EXPECT_EQ(DecodedClosure, Closure);
	EXPECT_EQ(DecodedPayload, Payload);
	std::vector<std::byte> Reencoded;
	ASSERT_TRUE(EncodeUnknownValueBody(DecodedClosure, DecodedPayload, Reencoded, Error)) << Error;
	EXPECT_EQ(Reencoded, Body);

	FTableInput Forward = MakeComprehensiveInput();
	FTableInput Reverse = Forward;
	std::reverse(Reverse.Types.begin(), Reverse.Types.end());
	const FFrozenTables KnownA = Freeze(Forward);
	const FFrozenTables KnownB = Freeze(Reverse);
	EXPECT_EQ(KnownA.Names, KnownB.Names);
	std::vector<std::byte> BodyAfterKnownReorder;
	ASSERT_TRUE(EncodeUnknownValueBody(Closure, Payload, BodyAfterKnownReorder, Error)) << Error;
	EXPECT_EQ(BodyAfterKnownReorder, Body);

	std::vector<std::byte> InvalidClosure = Closure;
	InvalidClosure.back() = std::byte{2};
	EXPECT_FALSE(ValidateRetainedClosure(InvalidClosure, Error));
	EXPECT_NE(Error.find("field id"), std::string::npos);
	InvalidClosure = Closure;
	InvalidClosure.push_back(std::byte{0xff});
	EXPECT_FALSE(ValidateRetainedClosure(InvalidClosure, Error));
	EXPECT_NE(Error.find("unconsumed"), std::string::npos);
}

TEST(FPackageObjectStreamReferenceModelTests, CustomVersionsSortRetainAndFailClosed)
{
	FTableInput Input;
	Input.CustomVersions = {
		{{9, 0, 0, 0}, 11},
		{{1, 2, 3, 4}, 7},
	};
	const FFrozenTables Tables = Freeze(Input);
	ASSERT_EQ(Tables.CustomVersions.size(), 2);
	EXPECT_EQ(Tables.CustomVersions[0].Guid, (FGuidValue{1, 2, 3, 4}));
	std::array<std::vector<std::byte>, 4> Sections;
	std::string Error;
	ASSERT_TRUE(EncodeTableSections(Tables, Sections, Error)) << Error;
	FFrozenTables Decoded;
	ASSERT_TRUE(DecodeTableSections(Sections, Decoded, Error)) << Error;
	std::array<std::vector<std::byte>, 4> Reencoded;
	ASSERT_TRUE(EncodeTableSections(Decoded, Reencoded, Error)) << Error;
	EXPECT_EQ(Reencoded, Sections);

	auto ExpectFreezeFailure = [&](FTableInput Invalid, std::string_view Category)
	{
		FFrozenTables Unchanged;
		Unchanged.Names = {"sentinel"};
		EXPECT_FALSE(FreezeTables(Invalid, Unchanged, Error));
		EXPECT_EQ(Unchanged.Names, std::vector<std::string>({"sentinel"}));
		EXPECT_NE(Error.find(Category), std::string::npos) << Error;
	};
	FTableInput Duplicate;
	Duplicate.CustomVersions = {{{1, 0, 0, 0}, 1}, {{1, 0, 0, 0}, 2}};
	ExpectFreezeFailure(Duplicate, "duplicate");
	FTableInput Unsupported;
	Unsupported.CustomVersions = {{{1, 0, 0, 0}, 3, {}, 2, true, false}};
	ExpectFreezeFailure(Unsupported, "unsupported");
	FTableInput Mismatch;
	Mismatch.CustomVersions = {{{1, 0, 0, 0}, 3, 4}};
	ExpectFreezeFailure(Mismatch, "mismatch");
	FTableInput Required;
	Required.CustomVersions = {{{1, 0, 0, 0}, 3, {}, {}, false, true}};
	ExpectFreezeFailure(Required, "unknown required");
	FTableInput Excess;
	for (uint32 Index = 0; Index <= MaximumCustomVersions; ++Index)
		Excess.CustomVersions.push_back({{Index, 0, 0, 0}, 0});
	ExpectFreezeFailure(Excess, "count exceeds");

	std::array<std::vector<std::byte>, 4> OutOfRange = {
		Bytes({0x00}), Bytes({0x00}),
		Bytes({0x01,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x80, 0x80, 0x80, 0x80, 0x10, 0x00}),
		Bytes({0x00}),
	};
	EXPECT_FALSE(DecodeTableSections(OutOfRange, Decoded, Error));
	EXPECT_NE(Error.find("uint32"), std::string::npos) << Error;
}

TEST(FPackageObjectStreamReferenceModelTests, MalformedTablesValuesCyclesAndDepthFailByCategory)
{
	const FTypePtr Bool = Scalar(ETypeOpcode::Bool);
	FTableInput Minimal;
	Minimal.Types = {Bool};
	Minimal.Schemas = {{"A", {{"Field", Bool, 0}}}};
	const FFrozenTables Tables = Freeze(Minimal);
	std::array<std::vector<std::byte>, 4> Sections;
	std::string Error;
	FFrozenTables Unchanged;
	ASSERT_TRUE(EncodeTableSections(Tables, Sections, Error)) << Error;
	auto ExpectTableFailure = [&](std::array<std::vector<std::byte>, 4> Invalid, std::string_view Category)
	{
		FFrozenTables Result;
		Result.Names = {"sentinel"};
		EXPECT_FALSE(DecodeTableSections(Invalid, Result, Error));
		EXPECT_EQ(Result.Names, std::vector<std::string>({"sentinel"}));
		EXPECT_NE(Error.find(Category), std::string::npos) << Error;
	};

	auto Invalid = Sections;
	Invalid[0] = Bytes({0x02, 0x05, 0x46, 0x69, 0x65, 0x6c, 0x64, 0x01, 0x41});
	ExpectTableFailure(Invalid, "canonical");
	Invalid = Sections;
	Invalid[1] = Bytes({0x01, 0x01, 0x18});
	ExpectTableFailure(Invalid, "opcode");
	Invalid = Sections;
	Invalid[2].back() = std::byte{1};
	ExpectTableFailure(Invalid, "flags");
	Invalid = Sections;
	Invalid[2][Invalid[2].size() - 2] = std::byte{2};
	ExpectTableFailure(Invalid, "out of range");
	Invalid = Sections;
	Invalid[0].push_back(std::byte{0xff});
	ExpectTableFailure(Invalid, "unconsumed");

	FTableInput ObjectInput;
	ObjectInput.Objects = {{"Root", "", "C", "Root"}};
	const FFrozenTables ObjectTables = Freeze(ObjectInput);
	std::array<std::vector<std::byte>, 4> ObjectSections;
	ASSERT_TRUE(EncodeTableSections(ObjectTables, ObjectSections, Error)) << Error;
	ObjectSections[3][2] = std::byte{1};
	ExpectTableFailure(ObjectSections, "outer");
	FTableInput DuplicateObjects;
	DuplicateObjects.Objects = {
		{"Root", "", "C", "Root"},
		{"Root/Child", "Root", "C", "Child"},
		{"Root/Child", "Root", "OtherC", "Child"},
	};
	EXPECT_FALSE(FreezeTables(DuplicateObjects, Unchanged, Error));
	EXPECT_NE(Error.find("duplicate sibling"), std::string::npos);

	FTableInput Cyclic;
	const FTypePtr Cycle = MakeType(ETypeOpcode::Array);
	Cycle->Children = {Cycle};
	Cyclic.Types = {Cycle};
	EXPECT_FALSE(FreezeTables(Cyclic, Unchanged, Error));
	EXPECT_NE(Error.find("cycle"), std::string::npos);
	FTableInput BadMap;
	BadMap.Types = {MakeType(ETypeOpcode::Map, "", 0,
		{MakeType(ETypeOpcode::Array, "", 0, {Scalar(ETypeOpcode::I32)}), Bool})};
	EXPECT_FALSE(FreezeTables(BadMap, Unchanged, Error));
	EXPECT_NE(Error.find("map key"), std::string::npos);

	const FFrozenTables Comprehensive = Freeze(MakeComprehensiveInput());
	const FTypePtr Map = MakeType(ETypeOpcode::Map, "", 0,
		{Scalar(ETypeOpcode::String), Scalar(ETypeOpcode::I32)});
	ExpectDecodeFailure(Map, Bytes({0x02, 0x01, 0x42, 0x04, 0x01, 0x41, 0x02}),
		Comprehensive, "map keys");
	const FTypePtr Array = MakeType(ETypeOpcode::Array, "", 0, {Bool});
	ExpectDecodeFailure(Array, Bytes({0x80, 0x80, 0x40}), Comprehensive, "count exceeds");
	const FTypePtr Struct = MakeType(ETypeOpcode::Struct, "Example::Inner");
	ExpectDecodeFailure(Struct, Bytes({0x01, 0x02, 0x02, 0x01, 0x00}),
		Comprehensive, "provenance");
	ExpectDecodeFailure(Struct, Bytes({0x01, 0x02, 0x00, 0x02, 0x00, 0xff}),
		Comprehensive, "unconsumed");
	ExpectDecodeFailure(Scalar(ETypeOpcode::Name), Bytes({0x00}), Comprehensive, "out of range");

	FTypePtr Deep = Bool;
	FValue DeepValue;
	DeepValue.Bool = true;
	for (uint32 Index = 0; Index < MaximumValueDepth + 1; ++Index)
	{
		Deep = MakeType(ETypeOpcode::Array, "", 0, {Deep});
		DeepValue = FValue{.Elements = {DeepValue}};
	}
	std::vector<std::byte> Output = Bytes({0xee});
	EXPECT_FALSE(EncodeValue(*Deep, DeepValue, Comprehensive, Output, Error));
	EXPECT_EQ(Output, Bytes({0xee}));
	EXPECT_NE(Error.find("depth"), std::string::npos);
}
