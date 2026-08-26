#include <gtest/gtest.h>

#include "Asset/AssetImportData.h"
#include "AssetForge/Persistence/AssetForgeImportData.h"
#include "AssetTools.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/Paths.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

namespace
{
	class DAssetImportOwnerForTest : public Durin::DObject
	{
	public:
		explicit DAssetImportOwnerForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DAssetImportOwnerForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DAssetImportOwnerForTest",
					sizeof(DAssetImportOwnerForTest), alignof(DAssetImportOwnerForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DAssetImportOwnerForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DAssetImportOwnerForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const FObjectPropertyParams ImportDataProperty =
				FObjectPropertyParams::ObjectPtr<AssetImport::DAssetImportData>(
					"AssetImportData", EPropertyFlags::EditorOnly, 1,
					STRUCT_OFFSET_UINT16(DAssetImportOwnerForTest, AssetImportData),
					&AssetImport::DAssetImportData::StaticClass);
			static const FPropertyParamsBase* Properties[] = {&ImportDataProperty};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DAssetImportOwnerForTest",
				"DAssetImportOwnerForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		Durin::TObjectPtr<Durin::AssetImport::DAssetImportData> AssetImportData;
	};

	auto MakeSource(
		std::string Identity,
		std::string Role,
		std::string Path,
		std::string_view Bytes) -> Durin::AssetImport::FSourceFile
	{
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(Bytes);
		return {
			.StableIdentity = std::move(Identity),
			.Role = std::move(Role),
			.DisplayLabel = "Source fixture",
			.SourcePath = {.Path = std::move(Path)},
			.ContentHashLow = Hash.HashLow,
			.ContentHashHigh = Hash.HashHigh,
			.ByteCount = Bytes.size(),
			.LastWriteTime = 123};
	}

	auto MakeState() -> Durin::AssetForge::FAssetForgeImportState
	{
		using namespace Durin::AssetForge;
		FAssetForgeImportState State;
		State.SourceData.Sources = {
			MakeSource("zeta", "dependency", "/TestSources/zeta.bin", "zeta"),
			MakeSource("root", "source", "/TestSources/root.png", "root")};
		std::string Error;
		const std::array SettingsBytes = {std::byte{1}, std::byte{2}, std::byte{3}};
		EXPECT_TRUE(MakeAssetImportPayload(
			"tests.translator-settings", 2, SettingsBytes,
			MaximumAssetImportSettingsBytes, State.Translator.Settings, Error)) << Error;
		State.Translator.ComponentId = "tests.translator";
		State.Translator.ContractVersion = 4;
		State.SourceReferences = {{"root"}, {"zeta"}};
		State.OutputMappings = {{"node.root", "output.root", "/TestAssets/Imported"}};
		State.SourceGraphFingerprintLow = 11;
		State.SourceGraphFingerprintHigh = 12;
		State.BuildGraphFingerprintLow = 21;
		State.BuildGraphFingerprintHigh = 22;
		State.AuthoredOutputFingerprint = "authored-output-v1";
		return State;
	}

	class FAssetImportDataTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Durin::Testing::InitializeDObjectSystemForTests();
			(void)Durin::AssetImport::DAssetImportData::StaticClass();
			(void)Durin::AssetForge::DAssetForgeImportData::StaticClass();
			(void)DAssetImportOwnerForTest::StaticClass();
			Durin::Asset::ShutdownAssetManager();
			Durin::CollectGarbage();
			const std::filesystem::path Root =
				Durin::Testing::GetTestWorkDirectory() / "Assets";
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::PathUtilities::RegisterMountPointForTests(
				"/TestAssets/", Root.generic_string() + "/");
			ASSERT_TRUE(Durin::Asset::InitializeAssetManager());
			ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog(
				Durin::Asset::EAssetRegistryScanMode::FullValidation));
		}

		void TearDown() override
		{
			Durin::Asset::ShutdownAssetManager();
			Durin::CollectGarbage();
		}
	};
}

TEST_F(FAssetImportDataTests, SourceInfoNormalizesValidatesLooksUpAndFingerprints)
{
	using namespace Durin::AssetImport;
	FAssetImportInfo Info;
	Info.Sources = {
		MakeSource("zeta", "dependency", "/TestSources/zeta.bin", "zeta"),
		MakeSource("root", "source", "/TestSources/root.png", "root")};
	std::string Error;
	EXPECT_FALSE(Info.Validate(Error));
	Info.Normalize();
	ASSERT_TRUE(Info.Validate(Error)) << Error;
	ASSERT_NE(Info.FindByStableIdentity("root"), nullptr);
	EXPECT_EQ(Info.FindByStableIdentity("root")->Role, "source");
	ASSERT_NE(Info.FindByRole("dependency"), nullptr);
	EXPECT_EQ(Info.FindByRole("dependency")->StableIdentity, "zeta");
	EXPECT_FALSE(Info.GetFingerprint().IsZero());

	FAssetImportInfo Same = Info;
	EXPECT_EQ(Same.GetFingerprint(), Info.GetFingerprint());
	Same.Sources[0].ContentHashHigh = 0;
	EXPECT_FALSE(Same.Validate(Error));
	Same = Info;
	Same.Sources.push_back(Same.Sources.front());
	EXPECT_FALSE(Same.Validate(Error));
	FSourceFile Partial;
	Partial.StableIdentity = "partial";
	EXPECT_FALSE(Partial.Validate(Error));
	FSourceFile Empty;
	EXPECT_TRUE(Empty.Validate(Error));
	FAssetImportInfo TooMany;
	TooMany.Sources.resize(static_cast<size_t>(MaximumAssetImportSources) + 1);
	EXPECT_FALSE(TooMany.Validate(Error));
}

TEST_F(FAssetImportDataTests, StateValidationPayloadBoundsAndClonePreserveValuesAndOwner)
{
	using namespace Durin;
	using namespace Durin::AssetImport;
	using namespace Durin::AssetForge;
	std::string Error;
	auto* Owner = NewObject<DObject>(nullptr, "ImportDataOwner");
	auto* Data = NewObject<DAssetForgeImportData>(Owner, "AssetImportData");
	EXPECT_TRUE(Data->Validate(Error)) << Error;
	ASSERT_NE(FSourceFile::StaticStruct()->FindPropertyByName("StableIdentity"), nullptr);
	ASSERT_NE(FAssetImportInfo::StaticStruct()->FindPropertyByName("Sources"), nullptr);
	ASSERT_NE(DAssetImportData::StaticClass()->FindPropertyByName("SourceData"), nullptr);
	ASSERT_NE(DAssetForgeImportData::StaticClass()->FindPropertyByName(
		"Translator"), nullptr);
	ASSERT_NE(DAssetForgeImportData::StaticClass()->FindPropertyByName(
		"ReplaySchemaVersion"), nullptr);
	FAssetForgeImportState State = MakeState();
	ASSERT_TRUE(Data->SetState(State, Error)) << Error;
	State.SourceData.Normalize();
	EXPECT_EQ(Data->GetAssetForgeState(), State);
	EXPECT_TRUE(Data->Validate(Error)) << Error;

	auto* CloneOwner = NewObject<DObject>(nullptr, "ImportDataCloneOwner");
	auto* Clone = Cast<DAssetForgeImportData>(
		Data->CloneToOwner(CloneOwner, "AssetImportData", Error));
	ASSERT_NE(Clone, nullptr) << Error;
	EXPECT_EQ(Clone->GetOuter(), CloneOwner);
	EXPECT_EQ(Clone->GetAssetForgeState(), Data->GetAssetForgeState());

	FAssetForgeImportState Invalid = State;
	Invalid.SourceReferences.pop_back();
	EXPECT_FALSE(Data->SetState(std::move(Invalid), Error));
	EXPECT_EQ(Data->GetAssetForgeState(), State);
	Invalid = State;
	Invalid.SchemaVersion = AssetImportDataSchemaVersion + 1;
	EXPECT_FALSE(Data->SetState(std::move(Invalid), Error));
	EXPECT_EQ(Data->GetAssetForgeState(), State);
	Invalid = State;
	Invalid.ReplaySchemaVersion = AssetForgeImportDataSchemaVersion + 1;
	EXPECT_FALSE(Data->SetState(std::move(Invalid), Error));
	EXPECT_EQ(Data->GetAssetForgeState(), State);
	Invalid = State;
	Invalid.Translator.ComponentId = "invalid component id";
	EXPECT_FALSE(Data->SetState(std::move(Invalid), Error));
	EXPECT_EQ(Data->GetAssetForgeState(), State);
	FAssetImportPayload Sentinel;
	const std::array Oversize = {std::byte{1}, std::byte{2}};
	EXPECT_FALSE(MakeAssetImportPayload(
		"tests.oversize", 1, Oversize, 1, Sentinel, Error));
	EXPECT_TRUE(Sentinel.IsEmpty());
	FAssetImportPayload Corrupt = State.Translator.Settings;
	Corrupt.ContentHashLow ^= 1;
	EXPECT_FALSE(Corrupt.Validate(MaximumAssetImportSettingsBytes, Error));
	FAssetForgeImportState Copied = State;
	FAssetForgeImportState Moved = std::move(Copied);
	EXPECT_EQ(Moved, State);
}

TEST_F(FAssetImportDataTests, AuthoredPackageRoundTripsAndInspectsCommonSourceData)
{
	using namespace Durin;
	using namespace Durin::AssetImport;
	using namespace Durin::AssetForge;
	FAssetPath Path;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/ImportDataRoundTrip", Path));
	DAssetImportOwnerForTest* Owner = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(Path, Owner));
	auto* Data = NewObject<DAssetForgeImportData>(Owner, "AssetImportData");
	std::string Error;
	ASSERT_TRUE(Data->SetState(MakeState(), Error)) << Error;
	Owner->AssetImportData = Data;
	const FAssetForgeImportState ExpectedState = Data->GetAssetForgeState();
	ASSERT_TRUE(Asset::SavePackage(Owner->GetPackage()));

	const Asset::FAssetCatalogEntry Catalog = Asset::FindAssetExact(Path);
	ASSERT_TRUE(Catalog);
	Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Asset::InspectAssetPackage(Catalog->PhysicalPath, Inspection));
	FAssetImportInfo Inspected;
	ASSERT_TRUE(InspectAssetImportInfo(Inspection, Inspected, Error)) << Error;
	EXPECT_EQ(Inspected, Data->GetSourceData());
	ASSERT_TRUE(Asset::UnloadPackage(Path));

	DAssetImportOwnerForTest* Loaded = nullptr;
	ASSERT_TRUE(Asset::LoadAsset(Path, Loaded));
	auto* LoadedData = Cast<DAssetForgeImportData>(Loaded->AssetImportData.Get());
	ASSERT_NE(LoadedData, nullptr);
	EXPECT_EQ(LoadedData->GetOuter(), Loaded);
	EXPECT_EQ(LoadedData->GetAssetForgeState(), ExpectedState);
}
