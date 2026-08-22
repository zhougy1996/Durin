#include <gtest/gtest.h>

#include "Asset/PackageV4Writer.h"
#include "Asset/PackageV4Reader.h"
#include "PackageV4ReferenceModel.h"

#include <algorithm>
#include <bit>

namespace
{
	using Durin::uint8;
	using Durin::uint32;
	using Durin::uint64;
	namespace Production = Durin::Asset::DastV4;
	namespace Reference = Durin::Testing::DastV4;
	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		Result.reserve(Values.size());
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto BuildReferencePackage(bool Reverse) -> std::vector<std::byte>
	{
		auto I32 = Reference::MakeType(Reference::ETypeOpcode::I32);
		auto String = Reference::MakeType(Reference::ETypeOpcode::String);
		auto Map = Reference::MakeType(Reference::ETypeOpcode::Map, {}, 0, {String, I32});
		Reference::FTableInput Input;
		Input.PublicDependencyCount = 1;
		Input.AdditionalNames = {"Named", "/Game/Soft"};
		Input.Types = {I32, String, Map};
		Input.Schemas = {{"Example::Asset", {{"Count", I32, 0}, {"Lookup", Map, 0}}}};
		Input.CustomVersions = {{{2, 0, 0, 0}, 7}, {{1, 0, 0, 0}, 3}};
		Input.Objects = {{"Root", {}, "Example::Asset", "Root"}};
		if (Reverse)
		{
			std::ranges::reverse(Input.Types);
			std::ranges::reverse(Input.Schemas.front().Fields);
			std::ranges::reverse(Input.CustomVersions);
		}
		Reference::FFrozenTables Tables;
		std::string Error;
		EXPECT_TRUE(Reference::FreezeTables(Input, Tables, Error)) << Error;
		Reference::FValue MapValue;
		MapValue.Elements = {
			Reference::FValue{.Text = "B"}, Reference::FValue{.Signed = 2},
			Reference::FValue{.Text = "A"}, Reference::FValue{.Signed = 1}};
		std::vector<Reference::FObjectValueInput> Values = {{"Root", {
			{.SchemaName = "Example::Asset", .FieldName = "Count", .Value = Reference::FValue{.Signed = -7}, .LoadedExplicit = true},
			{.SchemaName = "Example::Asset", .FieldName = "Lookup", .Value = std::move(MapValue), .LoadedExplicit = true},
		}}};
		std::array<std::vector<std::byte>, 4> TableSections;
		std::vector<std::byte> ValueSection;
		EXPECT_TRUE(Reference::EncodeTableSections(Tables, TableSections, Error)) << Error;
		EXPECT_TRUE(Reference::EncodeValueSection(Values, Tables, ValueSection, Error)) << Error;
		Reference::FPublicSummary Summary{
			.AssetClass = "Example::Asset", .EntryKind = 0,
			.Dependencies = {"/Game/Dependency"}, .ObjectCount = 1};
		std::array<std::vector<std::byte>, Reference::SectionCount> Sections{
			TableSections[0], TableSections[1], TableSections[2], TableSections[3], ValueSection};
		std::vector<std::byte> Bytes;
		EXPECT_TRUE(Reference::EncodeEnvelope(Summary, Sections, Bytes, Error)) << Error;
		return Bytes;
	}

	auto BuildProductionInput(bool Reverse) -> Production::FPackageInput
	{
		auto I32 = Production::MakeType(Production::ETypeOpcode::I32);
		auto String = Production::MakeType(Production::ETypeOpcode::String);
		auto Map = Production::MakeType(Production::ETypeOpcode::Map, {}, 0, {String, I32});
		Production::FValue MapValue;
		MapValue.Elements = {
			Production::FValue{.Text = "B"}, Production::FValue{.Signed = 2},
			Production::FValue{.Text = "A"}, Production::FValue{.Signed = 1}};
		Production::FPackageInput Input{
			.AssetClass = "Example::Asset",
			.Dependencies = {"/Game/Dependency"},
			.AdditionalNames = {"Named", "/Game/Soft"},
			.Types = {I32, String, Map},
			.Schemas = {{"Example::Asset", {{"Count", I32, 0}, {"Lookup", Map, 0}}}},
			.CustomVersions = {{{2, 0, 0, 0}, 7}, {{1, 0, 0, 0}, 3}},
			.Objects = {{"Root", {}, "Example::Asset", "Root"}},
			.ObjectValues = {{"Root", {
				{.SchemaName = "Example::Asset", .FieldName = "Count", .Provenance = Durin::EDefaultDeltaProvenance::Explicit, .Value = Production::FValue{.Signed = -7}},
				{.SchemaName = "Example::Asset", .FieldName = "Lookup", .Provenance = Durin::EDefaultDeltaProvenance::Explicit, .Value = std::move(MapValue)},
			}}},
		};
		if (Reverse)
		{
			std::ranges::reverse(Input.Types);
			std::ranges::reverse(Input.Schemas.front().Fields);
			std::ranges::reverse(Input.CustomVersions);
			std::ranges::reverse(Input.ObjectValues.front().KnownOverrides);
		}
		return Input;
	}

	auto EncodeReferenceValuePackage(const Reference::FTypePtr& Type,
		const Reference::FValue& Value,
		std::vector<Reference::FSchemaDescriptor> ExtraSchemas = {}) -> std::vector<std::byte>
	{
		Reference::FTableInput Input;
		Input.PublicDependencyCount = 1;
		Input.AdditionalNames = {"Named", "/Game/Soft"};
		Input.Types = {Type};
		Input.Schemas = std::move(ExtraSchemas);
		Input.Schemas.push_back({"Example::Owner", {{"Field", Type, 0}}});
		Input.Objects = {{"Root", {}, "Example::Owner", "Root"}, {"Root/Child", "Root", "Example::Owner", "Child"}};
		Reference::FFrozenTables Tables;
		std::string Error;
		EXPECT_TRUE(Reference::FreezeTables(Input, Tables, Error)) << Error;
		std::array<std::vector<std::byte>, 4> TableSections;
		std::vector<std::byte> ValueSection;
		EXPECT_TRUE(Reference::EncodeTableSections(Tables, TableSections, Error)) << Error;
		std::vector<Reference::FObjectValueInput> Values = {
			{"Root", {{.SchemaName = "Example::Owner", .FieldName = "Field", .Value = Value, .LoadedExplicit = true}}},
			{"Root/Child", {}}};
		EXPECT_TRUE(Reference::EncodeValueSection(Values, Tables, ValueSection, Error)) << Error;
		Reference::FPublicSummary Summary{.AssetClass = "Example::Owner", .Dependencies = {"/Game/Dep"}, .ObjectCount = 2};
		std::array<std::vector<std::byte>, Reference::SectionCount> Sections{
			TableSections[0], TableSections[1], TableSections[2], TableSections[3], ValueSection};
		std::vector<std::byte> Bytes;
		EXPECT_TRUE(Reference::EncodeEnvelope(Summary, Sections, Bytes, Error)) << Error;
		return Bytes;
	}

	auto EncodeProductionValuePackage(const Production::FTypePtr& Type,
		const Production::FValue& Value,
		std::vector<Production::FSchemaDescriptor> ExtraSchemas = {}) -> std::vector<std::byte>
	{
		Production::FPackageInput Input;
		Input.AssetClass = "Example::Owner";
		Input.Dependencies = {"/Game/Dep"};
		Input.AdditionalNames = {"Named", "/Game/Soft"};
		Input.Types = {Type};
		Input.Schemas = std::move(ExtraSchemas);
		Input.Schemas.push_back({"Example::Owner", {{"Field", Type, 0}}});
		Input.Objects = {{"Root", {}, "Example::Owner", "Root"}, {"Root/Child", "Root", "Example::Owner", "Child"}};
		Input.ObjectValues = {
			{"Root", {{.SchemaName = "Example::Owner", .FieldName = "Field", .Value = Value}}},
			{"Root/Child", {}}};
		std::vector<std::byte> Bytes;
		Production::FWriterDiagnostic Diagnostic;
		EXPECT_TRUE(Production::WritePackage(Input, Bytes, &Diagnostic)) << Diagnostic.Message;
		return Bytes;
	}
}

TEST(FPackageV4WriterTests, ProductionBytesMatchIndependentReferenceAndDiscoveryOrder)
{
	const std::vector<std::byte> Expected = BuildReferencePackage(false);
	EXPECT_EQ(Expected, BuildReferencePackage(true));
	std::vector<std::byte> Forward;
	std::vector<std::byte> Reverse;
	Production::FWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Production::WritePackage(BuildProductionInput(false), Forward, &Diagnostic))
		<< Diagnostic.Message;
	ASSERT_TRUE(Production::WritePackage(BuildProductionInput(true), Reverse, &Diagnostic))
		<< Diagnostic.Message;
	EXPECT_EQ(Forward, Expected);
	EXPECT_EQ(Reverse, Expected);
}

TEST(FPackageV4WriterTests, MalformedInputAndLimitsPreserveDestination)
{
	auto Input = BuildProductionInput(false);
	Input.Schemas.front().Fields.push_back(Input.Schemas.front().Fields.front());
	std::vector<std::byte> Destination = Bytes({0xde, 0xad});
	Production::FWriterDiagnostic Diagnostic;
	EXPECT_FALSE(Production::WritePackage(Input, Destination, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::DuplicateInput);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));

	Input = BuildProductionInput(false);
	auto Cycle = Production::MakeType(Production::ETypeOpcode::Array);
	Cycle->Children = {Cycle};
	Input.Types.push_back(Cycle);
	EXPECT_FALSE(Production::WritePackage(Input, Destination, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::DescriptorCycle);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));

	Input = BuildProductionInput(false);
	Input.Dependencies.resize(Production::MaximumDependencies + 1, "/Game/Dependency");
	EXPECT_FALSE(Production::WritePackage(Input, Destination, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::LimitExceeded);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));
	EXPECT_FALSE(Production::WriteAssetPackage(nullptr, Destination, {}, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::InvalidTopology);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));
	auto Name = Production::MakeType(Production::ETypeOpcode::Name);
	Production::FPackageInput Late{
		.AssetClass = "Example::Asset", .Types = {Name},
		.Schemas = {{"Example::Asset", {{"Late", Name, 0}}}},
		.Objects = {{"Root", {}, "Example::Asset", "Root"}},
		.ObjectValues = {{"Root", {{.SchemaName = "Example::Asset", .FieldName = "Late",
			.Value = Production::FValue{.Text = "Undiscovered"}}}}}};
	EXPECT_FALSE(Production::WritePackage(Late, Destination, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::MissingDiscovery);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));
	auto Opaque = Production::MakeType(Production::ETypeOpcode::Struct, "Example::Opaque");
	Opaque->bHasDeterministicStructOperations = false;
	Production::FPackageInput Unsupported{
		.AssetClass = "Example::Asset", .Types = {Opaque},
		.Schemas = {{"Example::Asset", {{"Opaque", Opaque, 0}}}, {"Example::Opaque", {}}},
		.Objects = {{"Root", {}, "Example::Asset", "Root"}},
		.ObjectValues = {{"Root", {{.SchemaName = "Example::Asset", .FieldName = "Opaque"}}}}};
	EXPECT_FALSE(Production::WritePackage(Unsupported, Destination, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::UnsupportedType);
	EXPECT_EQ(Destination, Bytes({0xde, 0xad}));
}

TEST(FPackageV4WriterTests, RetainedUnknownClosureAndPayloadAreExactAndValidated)
{
	auto I32 = Reference::MakeType(Reference::ETypeOpcode::I32);
	Reference::FTableInput ClosureInput;
	ClosureInput.Types = {I32};
	ClosureInput.Schemas = {{"Unknown::Owner", {{"Mystery", I32, 0}}}};
	Reference::FFrozenTables ClosureTables;
	std::string Error;
	ASSERT_TRUE(Reference::FreezeTables(ClosureInput, ClosureTables, Error)) << Error;
	std::vector<std::byte> Closure;
	ASSERT_TRUE(Reference::EncodeRetainedClosure(ClosureTables, 1, 1, Closure, Error)) << Error;
	const std::vector<std::byte> Payload = Bytes({0x80, 0x00, 0xde, 0xad, 0xbe, 0xef});

	auto Input = BuildProductionInput(false);
	Input.ObjectValues.front().KnownOverrides.clear();
	Input.ObjectValues.front().RetainedUnknownOverrides.push_back({
		.SchemaName = "Example::Asset", .FieldName = "Count",
		.DescriptorClosure = Closure, .Payload = Payload});
	std::vector<std::byte> Bytes;
	Production::FWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Production::WritePackage(Input, Bytes, &Diagnostic)) << Diagnostic.Message;
	EXPECT_NE(std::search(Bytes.begin(), Bytes.end(), Closure.begin(), Closure.end()), Bytes.end());
	EXPECT_NE(std::search(Bytes.begin(), Bytes.end(), Payload.begin(), Payload.end()), Bytes.end());

	Input.ObjectValues.front().RetainedUnknownOverrides.front().DescriptorClosure.back() = std::byte{2};
	const std::vector<std::byte> Before = Bytes;
	EXPECT_FALSE(Production::WritePackage(Input, Bytes, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EWriterFailure::InvalidRetainedClosure);
	EXPECT_EQ(Bytes, Before);
}

TEST(FPackageV4WriterTests, EverySupportedValueOpcodeMatchesIndependentReference)
{
	auto Expect = [&](const Reference::FTypePtr& ReferenceType, const Reference::FValue& ReferenceValue,
		const Production::FTypePtr& ProductionType, const Production::FValue& ProductionValue,
		std::vector<Reference::FSchemaDescriptor> ReferenceSchemas = {},
		std::vector<Production::FSchemaDescriptor> ProductionSchemas = {})
	{
		const std::vector<std::byte> ProductionBytes = EncodeProductionValuePackage(
			ProductionType, ProductionValue, std::move(ProductionSchemas));
		EXPECT_EQ(ProductionBytes,
			EncodeReferenceValuePackage(ReferenceType, ReferenceValue, std::move(ReferenceSchemas)));
		Production::FDecodedPackage Decoded;
		Production::FReaderDiagnostic ReaderDiagnostic;
		ASSERT_TRUE(Production::DecodePackage(ProductionBytes, Decoded, {}, &ReaderDiagnostic))
			<< ReaderDiagnostic.Message;
		std::vector<std::byte> RoundTrip;
		ASSERT_TRUE(Production::ReencodePackage(Decoded, RoundTrip, &ReaderDiagnostic))
			<< ReaderDiagnostic.Message;
		EXPECT_EQ(RoundTrip, ProductionBytes);
	};
	using RO = Reference::ETypeOpcode;
	using PO = Production::ETypeOpcode;
	Reference::FValue RBool; RBool.Bool = true;
	Production::FValue PBool; PBool.Bool = true;
	Expect(Reference::MakeType(RO::Bool), RBool, Production::MakeType(PO::Bool), PBool);
	for (const auto [R, P] : {std::pair{RO::I8, PO::I8}, {RO::I16, PO::I16}, {RO::I32, PO::I32}, {RO::I64, PO::I64}})
		Expect(Reference::MakeType(R), Reference::FValue{.Signed = -7}, Production::MakeType(P), Production::FValue{.Signed = -7});
	for (const auto [R, P] : {std::pair{RO::U8, PO::U8}, {RO::U16, PO::U16}, {RO::U32, PO::U32}, {RO::U64, PO::U64}})
		Expect(Reference::MakeType(R), Reference::FValue{.Unsigned = 255}, Production::MakeType(P), Production::FValue{.Unsigned = 255});
	Expect(Reference::MakeType(RO::F32), Reference::FValue{.Number = -0.0},
		Production::MakeType(PO::F32), Production::FValue{.FloatingBits = 0x80000000u});
	Expect(Reference::MakeType(RO::F64), Reference::FValue{.Number = 1.25},
		Production::MakeType(PO::F64), Production::FValue{.FloatingBits = std::bit_cast<uint64>(1.25)});
	Expect(Reference::MakeType(RO::String), Reference::FValue{.Text = "Text"},
		Production::MakeType(PO::String), Production::FValue{.Text = "Text"});
	Expect(Reference::MakeType(RO::Name), Reference::FValue{.Text = "Named"},
		Production::MakeType(PO::Name), Production::FValue{.Text = "Named"});
	Reference::FValue RGuid; RGuid.Guid = {1, 2, 3, 4};
	Production::FValue PGuid; PGuid.Guid = {1, 2, 3, 4};
	Expect(Reference::MakeType(RO::Guid), RGuid, Production::MakeType(PO::Guid), PGuid);
	Expect(Reference::MakeType(RO::Enum, "Example::Mode", uint8(RO::I16)), Reference::FValue{.Signed = -2},
		Production::MakeType(PO::Enum, "Example::Mode", uint8(PO::I16)), Production::FValue{.Signed = -2});
	for (uint8 Layout = 1; Layout <= 6; ++Layout)
	{
		const uint64 Count = Layout == 1 ? 2 : Layout == 2 ? 3 : Layout == 5 ? 10 : 4;
		Reference::FValue RValue; Production::FValue PValue;
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			const double Component = double(Index) + 0.25; RValue.Components.push_back(Component);
			PValue.ComponentBits.push_back(Layout == 6
				? std::bit_cast<uint32>(float(Component)) : std::bit_cast<uint64>(Component));
		}
		Expect(Reference::MakeType(RO::Intrinsic, {}, Layout), RValue,
			Production::MakeType(PO::Intrinsic, {}, Layout), PValue);
	}
	auto RI32 = Reference::MakeType(RO::I32); auto PI32 = Production::MakeType(PO::I32);
	Reference::FValue RStruct; RStruct.FieldIds = {1}; RStruct.Provenances = {0}; RStruct.Elements = {Reference::FValue{.Signed = 7}};
	Production::FValue PStruct; PStruct.FieldNames = {"X"}; PStruct.Provenances = {Durin::EDefaultDeltaProvenance::Explicit}; PStruct.Elements = {Production::FValue{.Signed = 7}};
	Expect(Reference::MakeType(RO::Struct, "Example::Inner"), RStruct,
		Production::MakeType(PO::Struct, "Example::Inner"), PStruct,
		{{"Example::Inner", {{"X", RI32, 0}}}}, {{"Example::Inner", {{"X", PI32, 0}}}});
	Expect(Reference::MakeType(RO::FixedArray, {}, 2, {RI32}), Reference::FValue{.Elements = {{.Signed = 1}, {.Signed = 2}}},
		Production::MakeType(PO::FixedArray, {}, 2, {PI32}), Production::FValue{.Elements = {{.Signed = 1}, {.Signed = 2}}});
	Expect(Reference::MakeType(RO::Array, {}, 0, {RI32}), Reference::FValue{.Elements = {{.Signed = 1}, {.Signed = 2}}},
		Production::MakeType(PO::Array, {}, 0, {PI32}), Production::FValue{.Elements = {{.Signed = 1}, {.Signed = 2}}});
	auto RString = Reference::MakeType(RO::String); auto PString = Production::MakeType(PO::String);
	Expect(Reference::MakeType(RO::Map, {}, 0, {RString, RI32}), Reference::FValue{.Elements = {{.Text = "B"}, {.Signed = 2}, {.Text = "A"}, {.Signed = 1}}},
		Production::MakeType(PO::Map, {}, 0, {PString, PI32}), Production::FValue{.Elements = {{.Text = "B"}, {.Signed = 2}, {.Text = "A"}, {.Signed = 1}}});
	Expect(Reference::MakeType(RO::HardRef, "DObject"), Reference::FValue{.ReferenceTag = 1, .ReferenceId = 2},
		Production::MakeType(PO::HardRef, "DObject"), Production::FValue{.ReferenceTag = 1, .ReferenceId = 2});
	Expect(Reference::MakeType(RO::HardRef, "DObject"), Reference::FValue{.ReferenceTag = 2, .ReferenceId = 1},
		Production::MakeType(PO::HardRef, "DObject"), Production::FValue{.ReferenceTag = 2, .ReferenceId = 1});
	Expect(Reference::MakeType(RO::SoftRef, "DObject"), Reference::FValue{.Text = "/Game/Soft", .ReferenceTag = 1},
		Production::MakeType(PO::SoftRef, "DObject"), Production::FValue{.Text = "/Game/Soft", .ReferenceTag = 1});
	Reference::FValue RBytes; RBytes.ByteData = Bytes({1, 2, 3});
	Production::FValue PBytes; PBytes.Bytes = Bytes({1, 2, 3});
	Expect(Reference::MakeType(RO::Bytes), RBytes, Production::MakeType(PO::Bytes), PBytes);
}
