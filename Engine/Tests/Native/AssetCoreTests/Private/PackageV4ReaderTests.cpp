#include <gtest/gtest.h>

#include "AssetPackageV4Reader.h"
#include "AssetRedirector.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "PackageV4ReferenceModel.h"
#include "HAL/PlatformLTS.h"

namespace
{
	namespace Production = Durin::Asset::DastV4;
	namespace Reference = Durin::Testing::DastV4;
	using Durin::uint8;

	auto BuildPackage(bool bRetainedUnknown = false) -> std::vector<uint8>
	{
		auto I32 = Production::MakeType(Production::ETypeOpcode::I32);
		auto String = Production::MakeType(Production::ETypeOpcode::String);
		auto Map = Production::MakeType(Production::ETypeOpcode::Map, {}, 0, {String, I32});
		Production::FPackageInput Input{
			.AssetClass = "Example::Asset",
			.Dependencies = {},
			.AdditionalNames = {"Named", "/Game/Soft"},
			.Types = {I32, String, Map},
			.Schemas = {{"Example::Asset", {{"Count", I32, 0}, {"Lookup", Map, 0}}}},
			.CustomVersions = {{{1, 2, 3, 4}, 7}},
			.Objects = {{"Root", {}, "Example::Asset", "Root"}},
			.ObjectValues = {{"Root", {
				{.SchemaName = "Example::Asset", .FieldName = "Count", .Value = Production::FValue{.Signed = -7}},
				{.SchemaName = "Example::Asset", .FieldName = "Lookup", .Value = Production::FValue{.Elements = {
					{.Text = "B"}, {.Signed = 2}, {.Text = "A"}, {.Signed = 1}}}},
			}}},
		};
		if (bRetainedUnknown)
		{
			Reference::FTableInput ClosureInput;
			auto ReferenceI32 = Reference::MakeType(Reference::ETypeOpcode::I32);
			ClosureInput.Types = {ReferenceI32};
			ClosureInput.Schemas = {{"Unknown::Owner", {{"Mystery", ReferenceI32, 0}}}};
			Reference::FFrozenTables Tables;
			std::string Error;
			EXPECT_TRUE(Reference::FreezeTables(ClosureInput, Tables, Error)) << Error;
			std::vector<uint8> Closure;
			EXPECT_TRUE(Reference::EncodeRetainedClosure(Tables, 1, 1, Closure, Error)) << Error;
			Input.ObjectValues.front().KnownOverrides.clear();
			Input.ObjectValues.front().RetainedUnknownOverrides.push_back({
				.SchemaName = "Example::Asset", .FieldName = "Count",
				.DescriptorClosure = std::move(Closure), .Payload = {0xde, 0xad, 0xbe, 0xef}});
		}
		std::vector<uint8> Bytes;
		Production::FWriterDiagnostic Diagnostic;
		EXPECT_TRUE(Production::WritePackage(Input, Bytes, &Diagnostic)) << Diagnostic.Message;
		return Bytes;
	}

	auto InitializeLiveReaderTest() -> Durin::FAssetPath
	{
		static const bool Initialized = [] {
			if (!Durin::GIsGameThreadIdInitialized)
			{
				Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
				Durin::GIsGameThreadIdInitialized = true;
				Durin::FNameInit();
				Durin::DObjectInit();
			}
			(void)Durin::Asset::DAssetRedirector::StaticClass();
			const auto Root = Durin::Testing::GetTestWorkDirectory() / "V4ReaderAssets";
			Durin::PathUtilities::RegisterMountPointForTests("/V4Reader/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate("/V4Reader/Live", Path));
		return Path;
	}

	auto BuildLivePackage(bool bRetainedUnknown = false) -> std::vector<uint8>
	{
		const std::string ClassName = Durin::Asset::DAssetRedirector::StaticClass()
			->GetQualifiedName().ToString();
		const std::string ObjectClass = Durin::DObject::StaticClass()->GetQualifiedName().ToString();
		auto HardReference = Production::MakeType(Production::ETypeOpcode::HardRef, ObjectClass);
		Production::FPackageInput Input{
			.AssetClass = ClassName,
			.Types = {HardReference},
			.Schemas = {{ClassName, {{"DestinationObject", HardReference, 0}}}},
			.Objects = {{"Root", {}, ClassName, "Root"}},
			.ObjectValues = {{"Root", {{.SchemaName = ClassName, .FieldName = "DestinationObject",
				.Provenance = Durin::EDefaultDeltaProvenance::Explicit,
				.Value = Production::FValue{.ReferenceTag = 0}}}}},
		};
		if (bRetainedUnknown)
		{
			Reference::FTableInput ClosureInput;
			auto I32 = Reference::MakeType(Reference::ETypeOpcode::I32);
			ClosureInput.Types = {I32}; ClosureInput.Schemas = {{ClassName, {{"DestinationObject", I32, 0}}}};
			Reference::FFrozenTables Tables; std::string Error; std::vector<uint8> Closure;
			EXPECT_TRUE(Reference::FreezeTables(ClosureInput, Tables, Error)) << Error;
			EXPECT_TRUE(Reference::EncodeRetainedClosure(Tables, 1, 1, Closure, Error)) << Error;
			Input.ObjectValues.front().KnownOverrides.clear();
			Input.ObjectValues.front().RetainedUnknownOverrides.push_back({ClassName,
				"DestinationObject", std::move(Closure), {0xca, 0xfe}});
		}
		std::vector<uint8> Bytes;
		Production::FWriterDiagnostic Diagnostic;
		EXPECT_TRUE(Production::WritePackage(Input, Bytes, &Diagnostic)) << Diagnostic.Message;
		return Bytes;
	}

	auto BuildReferencePackage() -> std::vector<uint8>
	{
		const std::string ClassName = Durin::Asset::DAssetRedirector::StaticClass()
			->GetQualifiedName().ToString();
		auto Hard = Production::MakeType(Production::ETypeOpcode::HardRef, "DObject");
		auto Soft = Production::MakeType(Production::ETypeOpcode::SoftRef, "DObject");
		Production::FPackageInput Input{
			.AssetClass = ClassName,
			.Dependencies = {"/V4Reader/Target"},
			.AdditionalNames = {"/V4Reader/Soft"},
			.Types = {Hard, Soft},
			.Schemas = {{ClassName, {{"External", Hard, 0}, {"Internal", Hard, 0}, {"Soft", Soft, 0}}}},
			.Objects = {{"Root", {}, ClassName, "Root"}, {"Root/Child", "Root", ClassName, "Child"}},
			.ObjectValues = {{"Root", {
				{.SchemaName = ClassName, .FieldName = "External", .Value = Production::FValue{.ReferenceTag = 2, .ReferenceId = 1}},
				{.SchemaName = ClassName, .FieldName = "Internal", .Value = Production::FValue{.ReferenceTag = 1, .ReferenceId = 2}},
				{.SchemaName = ClassName, .FieldName = "Soft", .Value = Production::FValue{.Text = "/V4Reader/Soft", .ReferenceTag = 1}},
			}}, {"Root/Child", {}}},
		};
		std::vector<uint8> Bytes; Production::FWriterDiagnostic Diagnostic;
		EXPECT_TRUE(Production::WritePackage(Input, Bytes, &Diagnostic)) << Diagnostic.Message;
		return Bytes;
	}
}

TEST(FPackageV4ReaderTests, HeaderOnlyValidatesSummaryAndDirectoryWithoutPublishingOnFailure)
{
	const std::vector<uint8> Bytes = BuildPackage();
	Production::FValidatedHeader Header{.AssetClass = "sentinel"};
	Production::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Production::ReadHeader(Bytes, Header, {}, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Header.AssetClass, "Example::Asset");
	EXPECT_TRUE(Header.Dependencies.empty());
	EXPECT_EQ(Header.ObjectCount, 1);
	EXPECT_LT(Header.BytesRead, Bytes.size());
	EXPECT_EQ(Header.BytesRead, Header.Sections.front().Offset);

	std::vector<uint8> Malformed = Bytes;
	Malformed[Header.BytesRead - Production::RequiredSectionCount * 9] =
		uint8(Production::ESectionKind::Value);
	const Production::FValidatedHeader Before = Header;
	EXPECT_FALSE(Production::ReadHeader(Malformed, Header, {}, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EReaderFailure::InvalidDirectory);
	EXPECT_EQ(Header, Before);
}

TEST(FPackageV4ReaderTests, CompleteDecodeReconstructsAndReemitsCanonicalBytes)
{
	const std::vector<uint8> Bytes = BuildPackage();
	Production::FDecodedPackage Package;
	Production::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Production::DecodePackage(Bytes, Package, {}, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Package.Names.size(), 6);
	EXPECT_EQ(Package.Types.size(), 3);
	ASSERT_EQ(Package.Schemas.size(), 1);
	ASSERT_EQ(Package.ObjectValues.size(), 1);
	ASSERT_EQ(Package.ObjectValues.front().Overrides.size(), 2);
	std::vector<uint8> Reencoded = {0xaa};
	ASSERT_TRUE(Production::ReencodePackage(Package, Reencoded, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Reencoded, Bytes);
}

TEST(FPackageV4ReaderTests, RetainedClosureAndPayloadRemainExact)
{
	const std::vector<uint8> Bytes = BuildPackage(true);
	Production::FDecodedPackage Package;
	Production::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Production::DecodePackage(Bytes, Package, {}, &Diagnostic)) << Diagnostic.Message;
	ASSERT_EQ(Package.ObjectValues.front().Overrides.size(), 1);
	const auto& Override = Package.ObjectValues.front().Overrides.front();
	EXPECT_EQ(Override.Provenance, 2);
	EXPECT_FALSE(Override.DescriptorClosure.empty());
	EXPECT_EQ(Override.RetainedPayload, (std::vector<uint8>{0xde, 0xad, 0xbe, 0xef}));
	std::vector<uint8> Reencoded;
	ASSERT_TRUE(Production::ReencodePackage(Package, Reencoded, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Reencoded, Bytes);
}

TEST(FPackageV4ReaderTests, MalformedPrimitiveExtentAndLimitFailuresAreAtomic)
{
	const std::vector<uint8> Bytes = BuildPackage();
	Production::FDecodedPackage Package;
	Package.Header.AssetClass = "sentinel";
	Production::FReaderDiagnostic Diagnostic;
	for (size_t Size : {size_t(0), size_t(4), size_t(12), Bytes.size() - 1})
	{
		EXPECT_FALSE(Production::DecodePackage(std::span(Bytes).first(Size), Package, {}, &Diagnostic));
		EXPECT_EQ(Package.Header.AssetClass, "sentinel");
	}
	Production::FReaderLimits Limits;
	Limits.TableEntries = 1;
	EXPECT_FALSE(Production::DecodePackage(Bytes, Package, Limits, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EReaderFailure::LimitExceeded);
	EXPECT_EQ(Package.Header.AssetClass, "sentinel");
}

TEST(FPackageV4ReaderTests, ConstructFreeInspectionProjectsKnownAndRetainedFields)
{
	Durin::Asset::FAssetPackageInspection Inspection;
	Production::FReaderDiagnostic Diagnostic;
	const std::vector<uint8> Bytes = BuildPackage();
	ASSERT_TRUE(Production::InspectPackage(Bytes, Inspection, {}, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Inspection.Header.FormatVersion, 4);
	ASSERT_EQ(Inspection.Objects.size(), 1);
	ASSERT_EQ(Inspection.Objects.front().Fields.size(), 2);
	EXPECT_EQ(Inspection.Objects.front().FindField("Count")->SourceFormatVersion, 4);
	Durin::int32 Count = 0;
	EXPECT_TRUE(Inspection.Objects.front().FindField("Count")->TryReadScalar(Count));
	EXPECT_EQ(Count, -7);

	ASSERT_TRUE(Production::InspectPackage(BuildPackage(true), Inspection, {}, &Diagnostic)) << Diagnostic.Message;
	ASSERT_EQ(Inspection.Objects.front().Fields.size(), 1);
	const auto& Retained = Inspection.Objects.front().Fields.front();
	EXPECT_EQ(Retained.TypeSignature, "DASTv4:RetainedClosure");
	const std::vector<uint8> Expected = {0xde, 0xad, 0xbe, 0xef};
	EXPECT_NE(std::search(Retained.Payload.begin(), Retained.Payload.end(),
		Expected.begin(), Expected.end()), Retained.Payload.end());
}

TEST(FPackageV4ReaderTests, ConstructFreeCompatibilityReportsUnavailableClassAndBoundedCost)
{
	const std::vector<uint8> Bytes = BuildPackage();
	Durin::Asset::FAssetPackageCompatibilityRecord Record;
	Durin::Asset::FAssetCompatibilityProbeStats Stats;
	Production::FReaderDiagnostic Diagnostic;
	Durin::FAssetPath Path;
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	ASSERT_TRUE(Production::ProbeCompatibility(Bytes, Path, Catalog, Record, &Stats, {}, &Diagnostic))
		<< Diagnostic.Message;
	EXPECT_EQ(Record.FormatVersion, 4);
	EXPECT_EQ(Record.Inspection, Durin::Asset::EAssetCompatibilityInspection::Ready);
	EXPECT_EQ(Record.Compatibility, Durin::Asset::EAssetPackageCompatibility::Unsupported);
	ASSERT_EQ(Record.Findings.size(), 1);
	EXPECT_EQ(Record.Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::UnavailableClass);
	EXPECT_GT(Stats.MetadataBytesRead, 0);
	EXPECT_GT(Stats.PayloadBytesSkipped, 0);
	EXPECT_LT(Stats.MetadataBytesRead, Bytes.size());
}

TEST(FPackageV4ReaderTests, ExplicitLiveLoadPublishesOnlyAfterPostLoadAndRollsBackFailure)
{
	const Durin::FAssetPath Path = InitializeLiveReaderTest();
	const std::vector<uint8> Bytes = BuildLivePackage();
	Production::FLoadedAssetPackage Loaded;
	Durin::Asset::FAssetLoadReport Report;
	Production::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Production::LoadAssetPackage(Bytes, Path, Loaded, &Report, {}, {}, &Diagnostic))
		<< Diagnostic.Message;
	ASSERT_TRUE(Loaded);
	ASSERT_NE(Loaded.GetPackage()->GetAsset(), nullptr);
	EXPECT_TRUE(Loaded.GetPackage()->GetAsset()->IsA(Durin::Asset::DAssetRedirector::StaticClass()));
	EXPECT_FALSE(Loaded.GetPackage()->IsDirty());
	EXPECT_FALSE(Report.HasCompatibilityIssues());
	ASSERT_EQ(Loaded.GetPackage()->GetAsset()->GetAuthoredOverrideEntries().size(), 1);
	EXPECT_EQ(Loaded.GetPackage()->GetAsset()->GetAuthoredOverrideEntries().front().Provenance,
		Durin::EAuthoredOverrideProvenance::LoadedExplicit);

	Durin::DPackage* Published = Loaded.GetPackage();
	EXPECT_FALSE(Production::LoadAssetPackage(Bytes, Path, Loaded, &Report, {}, {}, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EReaderFailure::PublicationFailure);
	EXPECT_EQ(Loaded.GetPackage(), Published);
	Production::FLiveLoadOptions Failure;
	Failure.ShouldFail = [](Production::ELiveLoadPhase Phase, Durin::uint64) {
		return Phase == Production::ELiveLoadPhase::PostLoad;
	};
	Durin::FAssetPath FailurePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/V4Reader/Failure", FailurePath));
	EXPECT_FALSE(Production::LoadAssetPackage(Bytes, FailurePath, Loaded, &Report, Failure, {}, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Production::EReaderFailure::PostLoadFailure);
	EXPECT_EQ(Loaded.GetPackage(), Published);
	Loaded.Reset();
}

TEST(FPackageV4ReaderTests, ConstructFreeReferencesCoverInternalExternalAndSoftValues)
{
	const Durin::FAssetPath Source = InitializeLiveReaderTest();
	Production::FReaderDiagnostic Diagnostic;
	std::vector<Durin::Asset::FAssetReferenceEdge> References;
	const auto Result = Production::ExtractReferences(BuildReferencePackage(), Source, References, {}, &Diagnostic);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(References.size(), 3);
	EXPECT_TRUE(std::ranges::any_of(References, [](const auto& Edge) {
		return Edge.Kind == Durin::Asset::EAssetReferenceKind::HardObject
			&& Edge.TargetPath.GetView() == "/V4Reader/Target";
	}));
	EXPECT_TRUE(std::ranges::any_of(References, [](const auto& Edge) {
		return Edge.Kind == Durin::Asset::EAssetReferenceKind::HardObject
			&& Edge.TargetPath.GetView() == "/V4Reader/Live";
	}));
	EXPECT_TRUE(std::ranges::any_of(References, [](const auto& Edge) {
		return Edge.Kind == Durin::Asset::EAssetReferenceKind::SoftObject
			&& Edge.TargetPath.GetView() == "/V4Reader/Soft";
	}));
}

TEST(FPackageV4ReaderTests, LiveUnknownReportRetainsExactClosureAndPayload)
{
	InitializeLiveReaderTest();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/V4Reader/Unknown", Path));
	Production::FLoadedAssetPackage Loaded;
	Durin::Asset::FAssetLoadReport Report;
	Production::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Production::LoadAssetPackage(BuildLivePackage(true), Path, Loaded,
		&Report, {}, {}, &Diagnostic)) << Diagnostic.Message;
	ASSERT_TRUE(Report.HasCompatibilityIssues());
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1);
	ASSERT_EQ(Report.CompatibilityIssues.front().LegacyFields.size(), 1);
	const auto& Legacy = Report.CompatibilityIssues.front().LegacyFields.front();
	EXPECT_FALSE(Legacy.DescriptorClosure.empty());
	EXPECT_EQ(Legacy.RetainedPayload, (std::vector<uint8>{0xca, 0xfe}));
	EXPECT_EQ(Report.CompatibilityIssues.front().Risk,
		Durin::Asset::EAssetCompatibilityRisk::UnknownNewerSchema);
	Loaded.Reset();
}

TEST(FPackageV4ReaderTests, CompatibilityMatchesKnownAndRetainedLiveSchemas)
{
	InitializeLiveReaderTest();
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	Durin::Asset::FAssetPackageCompatibilityRecord Record;
	Production::FReaderDiagnostic Diagnostic;
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/V4Reader/Compatibility", Path));
	ASSERT_TRUE(Production::ProbeCompatibility(BuildLivePackage(), Path, Catalog,
		Record, nullptr, {}, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Record.Compatibility, Durin::Asset::EAssetPackageCompatibility::Compatible);
	EXPECT_TRUE(Record.Findings.empty());
	ASSERT_TRUE(Production::ProbeCompatibility(BuildLivePackage(true), Path, Catalog,
		Record, nullptr, {}, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Record.Compatibility, Durin::Asset::EAssetPackageCompatibility::Incompatible);
	ASSERT_EQ(Record.Findings.size(), 1);
	EXPECT_EQ(Record.Findings.front().Code,
		Durin::Asset::EAssetCompatibilityFindingCode::IncompatibleFieldSignature);
}

TEST(FPackageV4ReaderTests, OrdinaryValidationUsesTheProductionV4Reader)
{
	const std::vector<uint8> Bytes = BuildPackage();
	Production::FDecodedPackage Package;
	ASSERT_TRUE(Production::DecodePackage(Bytes, Package));
	const Durin::Asset::FAssetResult Ordinary = Durin::Asset::ValidateAssetPackageBytes(Bytes);
	EXPECT_TRUE(Ordinary) << Ordinary.Message;
}
