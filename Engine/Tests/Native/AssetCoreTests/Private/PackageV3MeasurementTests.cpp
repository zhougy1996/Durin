#include <gtest/gtest.h>

#include "PackageV3Measurement.h"

#include "DObject/DurinPropertyTypes.h"
#include "Misc/FileHelper.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>

namespace
{
	struct FTrackedPackage
	{
		const char* Path;
		Durin::uint64 Size;
	};

	constexpr FTrackedPackage TrackedPackages[] = {
		{"Engine/Content/Materials/DefaultMaterial.dasset", 115479},
		{"Engine/Content/Materials/ImportedSurface.dasset", 115479},
		{"Engine/Content/Models/Box.dasset", 3312},
		{"Engine/Content/Models/Sphere.dasset", 3318},
		{"Engine/Content/Renderer/DefaultStudioEnvironment.dasset", 1457},
		{"Sandbox/Content/Levels/NewLevel.dasset", 9926},
		{"Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter.dasset", 37897},
		{"Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter_alpha.dasset", 44997},
		{"Sandbox/Content/Models/VintageLighter/Meshes/vintage_lighter_1k.dasset", 4130},
		{"Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor.dasset", 3421},
		{"Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity.dasset", 3479},
		{"Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic.dasset", 3503},
		{"Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness.dasset", 3504},
		{"Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal.dasset", 3422},
		{"Sandbox/Content/Models/VintageLighter/vintage_lighter_1k_Import.dasset", 14583},
		{"Sandbox/Content/Textures/TEXCUBE_PureSky_512x512.dasset", 6214},
		{"Sandbox/Content/Textures/TEX_StoneHead.dasset", 3369},
	};

	template<typename T>
	auto Write(std::vector<Durin::uint8>& Bytes, const T& Value) -> void
	{
		const auto* Data = reinterpret_cast<const Durin::uint8*>(&Value);
		Bytes.insert(Bytes.end(), Data, Data + sizeof(T));
	}

	auto WriteString(std::vector<Durin::uint8>& Bytes, std::string_view Value) -> void
	{
		Write(Bytes, Durin::uint64(Value.size()));
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	auto WriteField(
		std::vector<Durin::uint8>& Bytes,
		std::string_view DeclaringType,
		std::string_view Name,
		Durin::DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view Signature,
		std::span<const Durin::uint8> Payload) -> void
	{
		WriteString(Bytes, DeclaringType);
		WriteString(Bytes, Name);
		Write(Bytes, Durin::uint8(Kind));
		WriteString(Bytes, Signature);
		Write(Bytes, Durin::uint64(Payload.size()));
		Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
	}

	auto PrintMeasurement(
		std::string_view Path,
		const Durin::Testing::FV3PackageMeasurement& M) -> void
	{
		const auto& B = M.Bytes;
		std::cout << Path << '\t' << B.Total() << '\t' << B.Envelope << '\t'
			<< B.PublicSummaryFraming + B.PublicSummaryText << '\t'
			<< B.DependencyFraming + B.DependencyText << '\t'
			<< B.ObjectFraming + B.ObjectText << '\t'
			<< B.FieldFraming + B.FieldMetadataText << '\t'
			<< B.NestedStructFraming + B.NestedStructMetadataText << '\t'
			<< B.ContainerFraming << '\t'
			<< B.ReferenceFraming + B.ReferenceText << '\t'
			<< B.ScalarValues << '\t'
			<< B.StringValueFraming + B.StringValueText << '\t'
			<< M.Objects << '\t' << M.Fields << '\t' << M.NestedFields << '\t'
			<< M.ContainerElements << '\t' << M.References << '\t'
			<< M.UniqueMetadataStrings.size() << '\t'
			<< M.MetadataStringOccurrences << '\t'
			<< M.UniqueTypeSignatures.size() << '\t'
			<< M.TypeSignatureOccurrences << '\t'
			<< M.UniqueSchemas.size() << '\t' << M.SchemaOccurrences << '\t'
			<< M.MaximumNesting << '\t'
			<< M.ParseOperations << '\t' << M.AllocationInputs << '\n';
	}
}

TEST(FPackageV3MeasurementTests, ConservesEveryTrackedPackageByteRecursively)
{
	const std::filesystem::path Root = DAST_V3_CORPUS_ROOT;
	for (const FTrackedPackage& Package : TrackedPackages)
	{
		SCOPED_TRACE(Package.Path);
		std::vector<Durin::uint8> Bytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			Bytes, (Root / Package.Path).generic_string()));
		ASSERT_EQ(Bytes.size(), Package.Size);
		Durin::Testing::FV3PackageMeasurement Measurement;
		std::string Error;
		ASSERT_TRUE(Durin::Testing::MeasureDastV3(Bytes, Measurement, Error)) << Error;
		EXPECT_EQ(Measurement.Bytes.Total(), Bytes.size());
		EXPECT_EQ(Measurement.Bytes.Unclassified, 0);
		if (std::getenv("DURIN_DAST_V3_MEASUREMENT_REPORT"))
			PrintMeasurement(Package.Path, Measurement);
	}
}

TEST(FPackageV3MeasurementTests, SeparatesNestedMetadataFromContainerValues)
{
	std::vector<Durin::uint8> ArrayPayload;
	Write(ArrayPayload, Durin::uint64{3});
	Write(ArrayPayload, Durin::int32{11});
	Write(ArrayPayload, Durin::int32{22});
	Write(ArrayPayload, Durin::int32{33});

	std::vector<Durin::uint8> StructPayload;
	WriteString(StructPayload, "Example::Outer");
	Write(StructPayload, Durin::uint64{1});
	const std::string ScalarSignature = std::format(
		"{}:4", Durin::uint32(Durin::DurinCodeGen::EPropertyGenFlags::Int32));
	const std::string ArraySignature = std::format("Array<{}>", ScalarSignature);
	WriteField(StructPayload, "Example::Outer", "Values",
		Durin::DurinCodeGen::EPropertyGenFlags::Array, ArraySignature, ArrayPayload);

	std::vector<Durin::uint8> Package;
	Write(Package, Durin::uint32{0x54534144});
	Write(Package, Durin::uint32{3});
	WriteString(Package, "Example::Asset");
	Write(Package, Durin::uint8{0});
	WriteString(Package, "");
	Write(Package, Durin::uint64{0});
	Write(Package, Durin::uint64{1});
	Write(Package, Durin::uint64{1});
	Write(Package, Durin::uint64{0});
	WriteString(Package, "Example::Asset");
	WriteString(Package, "Synthetic");
	Write(Package, Durin::uint64{1});
	WriteField(Package, "Example::Asset", "Outer",
		Durin::DurinCodeGen::EPropertyGenFlags::Struct, "Struct<Example::Outer>", StructPayload);

	Durin::Testing::FV3PackageMeasurement Measurement;
	std::string Error;
	ASSERT_TRUE(Durin::Testing::MeasureDastV3(Package, Measurement, Error)) << Error;
	EXPECT_EQ(Measurement.Bytes.Total(), Package.size());
	EXPECT_GT(Measurement.Bytes.NestedStructMetadataText, 0);
	EXPECT_EQ(Measurement.Bytes.ContainerFraming, sizeof(Durin::uint64));
	EXPECT_EQ(Measurement.Bytes.ScalarValues, 3 * sizeof(Durin::int32));
	EXPECT_EQ(Measurement.NestedFields, 1);
	EXPECT_EQ(Measurement.ContainerElements, 3);
}

TEST(FPackageV3MeasurementTests, RejectsTruncationWithoutUnboundedParsing)
{
	const std::filesystem::path File = std::filesystem::path(DAST_V3_CORPUS_ROOT)
		/ "Engine/Content/Models/Box.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	Bytes.pop_back();
	Durin::Testing::FV3PackageMeasurement Measurement;
	std::string Error;
	EXPECT_FALSE(Durin::Testing::MeasureDastV3(Bytes, Measurement, Error));
	EXPECT_FALSE(Error.empty());
}
