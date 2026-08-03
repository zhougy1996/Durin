#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"

namespace
{
	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<const std::vector<T>*>(Container)->size(); }
	template<typename T>
	auto VectorGet(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorGetMutable(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorResize(void* Container, Durin::uint64 Num) -> void { static_cast<std::vector<T>*>(Container)->resize(Num); }

	const Durin::DurinCodeGen::FArrayPropertyHelper GIntVectorHelper = {
		&VectorNum<Durin::int32>, &VectorGet<Durin::int32>, &VectorGetMutable<Durin::int32>, &VectorResize<Durin::int32>
	};
	const Durin::DurinCodeGen::FArrayPropertyHelper GGuidVectorHelper = {
		&VectorNum<Durin::FGuid>, &VectorGet<Durin::FGuid>, &VectorGetMutable<Durin::FGuid>, &VectorResize<Durin::FGuid>
	};

	using FScoreMap = std::unordered_map<std::string, Durin::int32>;
	auto MapNum(const void* Container) -> Durin::uint64 { return static_cast<const FScoreMap*>(Container)->size(); }
	auto MapKey(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->first;
	}
	auto MapValue(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto MapMutableValue(void* Container, Durin::uint64 Index) -> void*
	{
		auto It = static_cast<FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto MapClear(void* Container) -> void { static_cast<FScoreMap*>(Container)->clear(); }
	auto CreateString() -> void* { return new std::string(); }
	auto CopyString(const void* Value) -> void* { return new std::string(*static_cast<const std::string*>(Value)); }
	auto DestroyString(void* Value) -> void { delete static_cast<std::string*>(Value); }
	auto CreateInt() -> void* { return new Durin::int32(); }
	auto DestroyInt(void* Value) -> void { delete static_cast<Durin::int32*>(Value); }
	auto MapInsert(void* Container, const void* Key, const void* Value) -> void
	{
		static_cast<FScoreMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const Durin::int32*>(Value));
	}
	auto MapContains(const void* Container, const void* Key) -> bool
	{
		return static_cast<const FScoreMap*>(Container)->contains(*static_cast<const std::string*>(Key));
	}
	auto MapRenameKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<FScoreMap*>(Container);
		const std::string OldKeyCopy = *static_cast<const std::string*>(OldKey);
		const std::string NewKeyCopy = *static_cast<const std::string*>(NewKey);
		if (OldKeyCopy == NewKeyCopy || Map->contains(NewKeyCopy)) return false;
		auto Node = Map->extract(OldKeyCopy);
		if (Node.empty()) return false;
		Node.key() = NewKeyCopy;
		Map->insert(std::move(Node));
		return true;
	}
	auto MapRemove(void* Container, const void* Key) -> bool
	{
		return static_cast<FScoreMap*>(Container)->erase(*static_cast<const std::string*>(Key)) != 0;
	}
	const Durin::DurinCodeGen::FMapPropertyHelper GScoreMapHelper = {
		&MapNum, &MapKey, &MapValue, &MapMutableValue, &MapClear, &CreateString, &CopyString, &DestroyString,
		&CreateInt, &DestroyInt, &MapInsert, &MapContains, &MapRenameKey, &MapRemove
	};
	bool GReportNonUpgradeMutationOnPostLoad = false;

	class DPackageAssetForTest : public Durin::DObject
	{
	public:
		explicit DPackageAssetForTest(const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			DefaultChild = Durin::NewObject<Durin::DObject>(this, "DefaultChild");
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X) { new (X.GetObj()) DPackageAssetForTest(X); }

		auto PostLoad(std::string& OutError) -> bool override
		{
			if (!DObject::PostLoad(OutError)) return false;
			if (GReportNonUpgradeMutationOnPostLoad)
				Durin::Asset::ReportAssetLoadMutation(
					this,
					"Tests.NonUpgradePostLoadMutation",
					"Test-only non-upgrade mutation.");
			return true;
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor, "DPackageAssetForTest", sizeof(DPackageAssetForTest), alignof(DPackageAssetForTest), Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None, Durin::EClassCastFlags::DClass, (Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DPackageAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DPackageAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FPropertyParamsBase ValueProp = {"Value", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Value)), sizeof(Value), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase LabelProp = {"Label", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Label)), sizeof(Label), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidProp = {"PersistentId", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, PersistentId)), sizeof(PersistentId), Durin::DurinCodeGen::EPropertyGenFlags::Guid};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidInner = {"RelatedIds_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::FGuid), Durin::DurinCodeGen::EPropertyGenFlags::Guid};
			static const Durin::DurinCodeGen::FArrayPropertyParams GuidsProp = {"RelatedIds", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, RelatedIds)), sizeof(RelatedIds), Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &GuidInner, nullptr, nullptr, false, &GGuidVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoreInner = {"Scores_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoresProp = {"Scores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Scores)), sizeof(Scores), Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &ScoreInner, nullptr, nullptr, false, &GIntVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapKeyProp = {"NamedScores_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapValueProp = {"NamedScores_Value", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase NamedScoresProp = {"NamedScores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, NamedScores)), sizeof(NamedScores), Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &MapKeyProp, &MapValueProp, false, nullptr, &GScoreMapHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase ChildProp = {"DefaultChild", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, DefaultChild)), sizeof(DefaultChild), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase ExternalProp = {"ExternalReference", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, ExternalReference)), sizeof(ExternalReference), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&ValueProp, &LabelProp, &GuidProp, &GuidsProp, &ScoresProp, &NamedScoresProp, &ChildProp, &ExternalProp};
			static const Durin::DurinCodeGen::FClassParams Params = {&StaticClassNoRegister, "Tests::DPackageAssetForTest", "DPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::int32 Value = 0;
		std::string Label;
		Durin::FGuid PersistentId;
		std::vector<Durin::FGuid> RelatedIds;
		std::vector<Durin::int32> Scores;
		FScoreMap NamedScores;
		Durin::TObjectPtr<Durin::DObject> DefaultChild;
		Durin::TObjectPtr<Durin::DObject> ExternalReference;
	};

	auto RegisterTestDeleteContributor() -> void
	{
		static const bool Registered = [] {
			Durin::Asset::RegisterAssetDeleteContributor(DPackageAssetForTest::StaticClass(), [](const Durin::Asset::FAssetData&,
				const Durin::Asset::FAssetPackageInspection& Inspection,
				Durin::Asset::FAssetDeleteContribution& Out) -> Durin::Asset::FAssetResult {
				const Durin::Asset::FAssetPackageField* LabelField = Inspection.FindField("Label");
				std::string Label;
				if (LabelField && LabelField->TryReadString(Label) && Label.starts_with("companion:"))
					Out.Files.emplace_back(Label.substr(10));
				return {};
			});
			return true;
		}();
		(void)Registered;
	}

	auto InitializeAssetTests() -> void
	{
		static const bool Initialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::FNameInit();
			Durin::DObjectInit();
			const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(
				(Durin::Testing::GetTestWorkDirectory() / "DerivedDataCache").generic_string());
			Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
	}

	auto ShutdownAssetManagerForRestart() -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::FAssetManager::Get().Initialize();
	}

	auto WriteTestBytes(const std::filesystem::path& Path, std::span<const Durin::uint8> Bytes) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto RefreshAssetData(
		const Durin::FAssetPath& Path,
		const std::filesystem::path& PhysicalPath) -> Durin::Asset::FAssetData
	{
		const Durin::Asset::FAssetData* Registered =
			Durin::Asset::GetAssetRegistry().FindAsset(Path);
		EXPECT_NE(Registered, nullptr);
		if (!Registered) return {};
		Durin::Asset::FAssetData Data = *Registered;
		std::error_code ErrorCode;
		Data.FileSize = std::filesystem::file_size(PhysicalPath, ErrorCode);
		EXPECT_FALSE(ErrorCode);
		Data.LastWriteTime = std::filesystem::last_write_time(PhysicalPath, ErrorCode);
		EXPECT_FALSE(ErrorCode);
		Data.LastWriteTimeTicks =
			Durin::DerivedDataCache::FileTimeToStableTicks(Data.LastWriteTime);
		return Data;
	}

	auto RenameSerializedString(
		std::vector<Durin::uint8>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		std::vector<Durin::uint8> Pattern(sizeof(Durin::uint64) + OldValue.size());
		const Durin::uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		const auto It = std::search(Bytes.begin(), Bytes.end(), Pattern.begin(), Pattern.end());
		if (It == Bytes.end()) return false;
		std::copy(NewValue.begin(), NewValue.end(), It + sizeof(Length));
		return true;
	}
}

TEST(FPackageAssetTests, HeaderReaderStopsBeforeLargeObjectPayload)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LargeHeaderOnly", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Scores.resize(1024 * 1024, 7);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LargeHeaderOnly.dasset";
	ASSERT_GT(std::filesystem::file_size(File), 4u * 1024u * 1024u);

	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(File.generic_string(), Header));
	EXPECT_EQ(Header.AssetClassName, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Header.FormatVersion, 2u);
	EXPECT_EQ(Header.ObjectCount, 2u);
	EXPECT_LT(Header.BytesRead, 1024u);
}

TEST(FPackageAssetTests, WriterEmitsFrozenVersionTwoPrefix)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/VersionTwoPrefix", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "VersionTwoPrefix.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	constexpr std::array<Durin::uint8, 8> ExpectedPrefix = {
		0x44, 0x41, 0x53, 0x54,
		0x02, 0x00, 0x00, 0x00};
	ASSERT_GE(Bytes.size(), ExpectedPrefix.size());
	EXPECT_TRUE(std::ranges::equal(
		ExpectedPrefix,
		std::span<const Durin::uint8>(Bytes).first(ExpectedPrefix.size())));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, HeaderReaderRejectsMalformedAndUnboundedDeclarations)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/HeaderValidationSource", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto Source = Root / "HeaderValidationSource.dasset";
	std::vector<Durin::uint8> Valid;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Valid, Source.generic_string()));
	ASSERT_GT(Valid.size(), 16u);
	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(Source.generic_string(), Header));

	auto Truncated = std::span<const Durin::uint8>(Valid).first(4);
	const auto TruncatedFile = Root / "HeaderTruncated.dasset";
	WriteTestBytes(TruncatedFile, Truncated);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(TruncatedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Corrupt = Valid;
	Corrupt[0] ^= 0xff;
	const auto CorruptFile = Root / "HeaderCorrupt.dasset";
	WriteTestBytes(CorruptFile, Corrupt);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(CorruptFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Unsupported = Valid;
	const Durin::uint32 OldVersion = 1;
	std::memcpy(Unsupported.data() + sizeof(Durin::uint32), &OldVersion, sizeof(OldVersion));
	const auto UnsupportedFile = Root / "HeaderUnsupported.dasset";
	WriteTestBytes(UnsupportedFile, Unsupported);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(UnsupportedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::UnsupportedVersion);

	auto Oversized = Valid;
	const Durin::uint64 OversizedString = 1024 * 1024 + 1;
	std::memcpy(Oversized.data() + sizeof(Durin::uint32) * 2, &OversizedString, sizeof(OversizedString));
	const auto OversizedFile = Root / "HeaderOversized.dasset";
	WriteTestBytes(OversizedFile, Oversized);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(OversizedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_LE(Header.BytesRead, 16u);
}

TEST(FPackageAssetTests, SavesLoadsContainersReferencesAndRegistryMetadata)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RoundTrip", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	Asset->Label = "RoundTrip";
	Asset->PersistentId = Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	Asset->RelatedIds = {Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)};
	Asset->Scores = {3, 5, 8};
	Asset->NamedScores = {{"Alpha", 11}, {"Beta", 17}};
	ASSERT_NE(Asset->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Asset->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");

	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Package));
	ASSERT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 42);
	EXPECT_EQ(Loaded->Label, "RoundTrip");
	EXPECT_EQ(Loaded->PersistentId, (Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff)));
	EXPECT_EQ(Loaded->RelatedIds, (std::vector<Durin::FGuid>{Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)}));
	EXPECT_EQ(Loaded->Scores, (std::vector<Durin::int32>{3, 5, 8}));
	EXPECT_EQ(Loaded->NamedScores.at("Alpha"), 11);
	EXPECT_EQ(Loaded->NamedScores.at("Beta"), 17);
	ASSERT_NE(Loaded->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Durin::GDObjectArray.GetObjectsWithOuter(Loaded).size(), 1u);
	EXPECT_EQ(Loaded->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(Path), Loaded->GetPackage());

	EXPECT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));
}

TEST(FPackageAssetTests, CompleteInspectionContainsEveryObjectAndContentFingerprint)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CompleteInspection", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 17;
	Asset->DefaultChild->Rename("InspectedChild");
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "CompleteInspection.dasset";
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_EQ(Inspection.Objects.size(), 2u);
	EXPECT_EQ(Inspection.Header.ObjectCount, 2u);
	EXPECT_EQ(Inspection.Objects[0].Id, 1u);
	EXPECT_EQ(Inspection.Objects[0].ClassName, "Tests::DPackageAssetForTest");
	EXPECT_NE(Inspection.Objects[0].FindField("Value"), nullptr);
	EXPECT_EQ(Inspection.Objects[1].Id, 2u);
	EXPECT_EQ(Inspection.Objects[1].OuterId, 1u);
	EXPECT_EQ(Inspection.Objects[1].ObjectName, "InspectedChild");
	EXPECT_EQ(Inspection.Fingerprint.FileSize, std::filesystem::file_size(File));
	EXPECT_FALSE(Inspection.Fingerprint.ContentHash.IsZero());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, ObjectFreeAuditReportsUnknownFieldsWithoutLoadingPackage)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AuditUnknown", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AuditUnknown.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Stale"));
	WriteTestBytes(File, Bytes);

	const Durin::Asset::FAssetData Data = RefreshAssetData(Path, File);
	Durin::Asset::FAssetPackageAuditReport Report;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Report));
	EXPECT_EQ(Report.State, Durin::Asset::EAssetPackageAuditState::RiskyUpgrade);
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(Report.CompatibilityIssues.front().ObjectPath, Path.ToString());
	EXPECT_EQ(
		Report.CompatibilityIssues.front().Classification,
		Durin::Asset::EAssetCompatibilityClassification::UnknownIncompatible);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);

	Durin::Asset::FAssetPackageUpgradeResult Upgrade;
	EXPECT_EQ(
		Durin::Asset::ExecutePackageUpgrade(Report, Upgrade).Error,
		Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::ExecutePackageUpgrade(
		Report,
		Upgrade,
		{.bAllowCompatibilityDataLoss = true}))
		<< Upgrade.Diagnostic;
	EXPECT_TRUE(Upgrade.bSaved);
	EXPECT_EQ(Upgrade.State, Durin::Asset::EAssetPackageAuditState::UpToDate);
	EXPECT_TRUE(Upgrade.LoadReport.HasRiskItems());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

TEST(FPackageAssetTests, UpgradeSessionSortsPackagesAndCountsTerminalStates)
{
	InitializeAssetTests();
	Durin::FAssetPath First;
	Durin::FAssetPath Second;
	Durin::FAssetPath Third;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/A", First));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/B", Second));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/C", Third));
	Durin::Asset::FAssetUpgradeSessionReport Session{
		.RegistryRevision = 42,
		.Packages = {
			{.PackagePath = Third, .State = Durin::Asset::EAssetPackageAuditState::AuditFailed},
			{.PackagePath = First, .State = Durin::Asset::EAssetPackageAuditState::SafeUpgrade},
			{.PackagePath = Second, .State = Durin::Asset::EAssetPackageAuditState::NotAudited}}};
	Session.RebuildProgressAndSort();
	ASSERT_EQ(Session.Packages.size(), 3u);
	EXPECT_EQ(Session.Packages[0].PackagePath, First);
	EXPECT_EQ(Session.Packages[1].PackagePath, Second);
	EXPECT_EQ(Session.Packages[2].PackagePath, Third);
	EXPECT_EQ(Session.Progress.Total, 3u);
	EXPECT_EQ(Session.Progress.Completed, 2u);
	EXPECT_EQ(Session.Progress.Safe, 1u);
	EXPECT_EQ(Session.Progress.Failed, 1u);
	EXPECT_EQ(Session.FindPackage(Second), &Session.Packages[1]);
}

TEST(FPackageAssetTests, InspectionUpgraderMakesRecognizedPackageBatchSafe)
{
	InitializeAssetTests();
	Durin::Asset::RegisterAssetStructureUpgrader(
		DPackageAssetForTest::StaticClass(),
		"Tests.SafeInspectionCleanup",
		[](Durin::DObject*, std::span<const Durin::Asset::FAssetLegacyField> Fields,
			const Durin::Asset::FAssetMigrationContext&,
			std::vector<Durin::Asset::FAssetCompatibilityIssue>& OutIssues)
			-> Durin::Asset::FAssetResult
		{
			const auto It =
				std::ranges::find(Fields, "Clean", &Durin::Asset::FAssetLegacyField::Name);
			if (It == Fields.end()) return {};
			OutIssues.push_back({
				.DeclaringClass = It->DeclaringClass,
				.LegacyFields = {*It},
				.Classification = Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
				.MigrationSummary = "Removed an empty legacy scalar.",
				.Risk = Durin::Asset::EAssetCompatibilityRisk::None});
			return {};
		});
	Durin::Asset::RegisterAssetStructureInspectionUpgrader(
		"Tests::DPackageAssetForTest",
		"Tests.SafeInspectionCleanup",
		[](const Durin::Asset::FAssetPackageInspection&,
			const Durin::Asset::FAssetPackageObjectInspection&,
			std::span<const Durin::Asset::FAssetLegacyField> Fields,
			std::vector<Durin::Asset::FAssetCompatibilityIssue>& OutIssues)
			-> Durin::Asset::FAssetResult
		{
			const auto It =
				std::ranges::find(Fields, "Clean", &Durin::Asset::FAssetLegacyField::Name);
			if (It == Fields.end()) return {};
			Durin::int32 Value = 1;
			if (It->Payload.size() != sizeof(Value))
				return {Durin::Asset::EAssetError::CorruptFile, "Invalid legacy scalar."};
			std::memcpy(&Value, It->Payload.data(), sizeof(Value));
			if (Value != 0) return {};
			OutIssues.push_back({
				.DeclaringClass = It->DeclaringClass,
				.LegacyFields = {*It},
				.Classification = Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
				.MigrationSummary = "Removed an empty legacy scalar.",
				.Risk = Durin::Asset::EAssetCompatibilityRisk::None});
			return {};
		});

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AuditSafe", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AuditSafe.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Clean"));
	WriteTestBytes(File, Bytes);

	const Durin::Asset::FAssetData Data = RefreshAssetData(Path, File);
	Durin::Asset::FAssetPackageAuditReport Report;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Report));
	EXPECT_EQ(Report.State, Durin::Asset::EAssetPackageAuditState::SafeUpgrade);
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(
		Report.CompatibilityIssues.front().HandlerId,
		"Tests.SafeInspectionCleanup");
	EXPECT_FALSE(Report.HasRiskItems());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);

	const std::filesystem::perms OriginalPermissions =
		std::filesystem::status(File).permissions();
	std::error_code PermissionError;
	std::filesystem::permissions(
		File,
		std::filesystem::perms::owner_write
			| std::filesystem::perms::group_write
			| std::filesystem::perms::others_write,
		std::filesystem::perm_options::remove,
		PermissionError);
	ASSERT_FALSE(PermissionError);
	Durin::Asset::FAssetPackageAuditReport ReadOnlyAudit;
	const Durin::Asset::FAssetData ReadOnlyData = RefreshAssetData(Path, File);
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(ReadOnlyData, ReadOnlyAudit));
	std::filesystem::permissions(
		File,
		OriginalPermissions,
		std::filesystem::perm_options::replace,
		PermissionError);
	ASSERT_FALSE(PermissionError);
	EXPECT_EQ(
		ReadOnlyAudit.State,
		Durin::Asset::EAssetPackageAuditState::BlockedReadOnly);

	std::vector<Durin::uint8> AuditedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AuditedBytes, File.generic_string()));
	const auto AuditedWriteTime = std::filesystem::last_write_time(File);
	std::vector<Durin::uint8> ChangedBytes = AuditedBytes;
	ASSERT_TRUE(RenameSerializedString(ChangedBytes, "Label", "Other"));
	WriteTestBytes(File, ChangedBytes);
	Durin::Asset::FAssetPackageUpgradeResult UpgradeResult;
	EXPECT_EQ(
		Durin::Asset::ExecutePackageUpgrade(Report, UpgradeResult).Error,
		Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(UpgradeResult.State, Durin::Asset::EAssetPackageAuditState::Stale);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	WriteTestBytes(File, AuditedBytes);
	std::filesystem::last_write_time(File, AuditedWriteTime);

	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Asset));
	EXPECT_EQ(
		Durin::Asset::ExecutePackageUpgrade(Report, UpgradeResult).Error,
		Durin::Asset::EAssetError::InUse);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	GReportNonUpgradeMutationOnPostLoad = true;
	const Durin::Asset::FAssetResult BlockedResult =
		Durin::Asset::ExecutePackageUpgrade(Report, UpgradeResult);
	GReportNonUpgradeMutationOnPostLoad = false;
	EXPECT_EQ(BlockedResult.Error, Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_EQ(
		UpgradeResult.State,
		Durin::Asset::EAssetPackageAuditState::BlockedLoadMutation);
	EXPECT_FALSE(UpgradeResult.bSaved);
	EXPECT_TRUE(UpgradeResult.LoadReport.HasNonUpgradeMutations());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);

	ASSERT_TRUE(Durin::Asset::ExecutePackageUpgrade(Report, UpgradeResult))
		<< UpgradeResult.Diagnostic;
	EXPECT_TRUE(UpgradeResult.bSaved);
	EXPECT_EQ(UpgradeResult.State, Durin::Asset::EAssetPackageAuditState::UpToDate);
	ASSERT_EQ(UpgradeResult.LoadReport.Mutations.size(), 1u);
	EXPECT_EQ(
		UpgradeResult.LoadReport.Mutations.front().Kind,
		Durin::Asset::EAssetLoadMutationKind::Upgrade);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	Durin::Asset::FAssetPackageInspection UpgradedInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), UpgradedInspection));
	EXPECT_EQ(UpgradedInspection.FindField("Clean"), nullptr);
}

TEST(FPackageAssetTests, ExpectedFingerprintRejectsExternallyChangedPackage)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/StaleSave", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "StaleSave.dasset";
	const Durin::Asset::FAssetData Data = RefreshAssetData(Path, File);
	Durin::Asset::FAssetPackageAuditReport Audit;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Audit));
	ASSERT_EQ(Audit.State, Durin::Asset::EAssetPackageAuditState::UpToDate);

	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Asset));
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Label", "Other"));
	WriteTestBytes(File, Bytes);
	EXPECT_EQ(
		Durin::Asset::SavePackage(
			Asset->GetPackage(),
			{.ExpectedFingerprint = Audit.Fingerprint}).Error,
		Durin::Asset::EAssetError::StaleData);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, ReportsUnknownFieldsWithoutMarkingPackageDirty)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ObsoleteField", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "ObsoleteField.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Stale"));
	WriteTestBytes(File, Bytes);

	testing::internal::CaptureStderr();
	DPackageAssetForTest* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	const Durin::Asset::FAssetResult LoadResult = Durin::Asset::LoadAsset(Path, Loaded, &Report);
	const std::string Warning = testing::internal::GetCapturedStderr();
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 0);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(Report.PackagePath, Path);
	EXPECT_EQ(Report.GetAffectedObjectCount(), 1u);
	EXPECT_EQ(Report.GetLegacyFieldCount(), 1u);
	EXPECT_EQ(Report.GetMigratedDataCount(), 0u);
	EXPECT_EQ(Report.GetRiskItemCount(), 1u);
	EXPECT_TRUE(Report.HasRiskItems());
	const Durin::Asset::FAssetCompatibilityIssue& Issue = Report.CompatibilityIssues.front();
	EXPECT_EQ(Issue.ObjectPath, "/TestAssets/ObsoleteField");
	EXPECT_EQ(Issue.DeclaringClass, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Issue.Classification, Durin::Asset::EAssetCompatibilityClassification::UnknownIncompatible);
	EXPECT_EQ(Issue.Risk, Durin::Asset::EAssetCompatibilityRisk::UnknownNewerSchema);
	ASSERT_EQ(Issue.LegacyFields.size(), 1u);
	EXPECT_EQ(Issue.LegacyFields.front().Name, "Stale");
	EXPECT_NE(Warning.find("on object '/TestAssets/ObsoleteField'"), std::string::npos);
	EXPECT_EQ(Warning.find("Resave the package"), std::string::npos);
	EXPECT_EQ(
		Durin::Asset::SavePackage(Loaded->GetPackage()).Error,
		Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_TRUE(Durin::Asset::SavePackage(
		Loaded->GetPackage(),
		{.bAllowCompatibilityDataLoss = true}));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, RegisteredSafeCleanupProducesStructuredReportAndDirtyPackage)
{
	InitializeAssetTests();
	Durin::Asset::RegisterAssetStructureUpgrader(
		DPackageAssetForTest::StaticClass(),
		"Tests.SafeScalarCleanup",
		[](Durin::DObject*, std::span<const Durin::Asset::FAssetLegacyField> Fields,
			const Durin::Asset::FAssetMigrationContext&,
			std::vector<Durin::Asset::FAssetCompatibilityIssue>& OutIssues) -> Durin::Asset::FAssetResult
		{
			const auto It = std::ranges::find(Fields, "Clean", &Durin::Asset::FAssetLegacyField::Name);
			if (It == Fields.end()) return {};
			Durin::int32 Value = 1;
			if (It->Payload.size() != sizeof(Value)) return {Durin::Asset::EAssetError::CorruptFile, "Invalid legacy scalar."};
			std::memcpy(&Value, It->Payload.data(), sizeof(Value));
			if (Value != 0) return {};
			OutIssues.push_back({
				.DeclaringClass = It->DeclaringClass,
				.LegacyFields = {*It},
				.Classification = Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
				.MigrationSummary = "Removed an empty legacy scalar.",
				.Risk = Durin::Asset::EAssetCompatibilityRisk::None});
			return {};
		});

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SafeCleanup", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "SafeCleanup.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Clean"));
	WriteTestBytes(File, Bytes);

	DPackageAssetForTest* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->GetPackage()->IsDirty());
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_FALSE(Report.HasRiskItems());
	EXPECT_EQ(Report.GetLegacyFieldCount(), 1u);
	EXPECT_EQ(Report.CompatibilityIssues.front().HandlerId, "Tests.SafeScalarCleanup");
	ASSERT_EQ(Report.Mutations.size(), 1u);
	EXPECT_EQ(Report.Mutations.front().PackagePath, Path);
	EXPECT_EQ(Report.Mutations.front().HandlerId, "Tests.SafeScalarCleanup");
	EXPECT_EQ(
		Report.Mutations.front().Kind,
		Durin::Asset::EAssetLoadMutationKind::Upgrade);
	EXPECT_FALSE(Report.HasNonUpgradeMutations());
	EXPECT_EQ(
		Report.CompatibilityIssues.front().Classification,
		Durin::Asset::EAssetCompatibilityClassification::SafeCleanup);
	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
	Report = {};
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	EXPECT_FALSE(Report.HasCompatibilityIssues());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, RejectsInvalidPaths)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("TestAssets/Relative", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/../Escape", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/Unknown/Asset", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/With.dasset", Path));
}

TEST(FPackageAssetTests, RejectsSavingCppPackages)
{
	InitializeAssetTests();
	Durin::DPackage* Package = Durin::FindOrCreateCppPackage("AssetCoreTests");
	ASSERT_NE(Package, nullptr);
	EXPECT_EQ(Durin::Asset::SavePackage(Package).Error, Durin::Asset::EAssetError::InvalidPackageType);
}

TEST(FPackageAssetTests, PackageEditRevisionAdvancesForRepeatedDirtyEdits)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/EditRevision", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);

	const Durin::uint64 CreatedRevision = Package->GetEditRevision();
	Package->ClearDirty();
	EXPECT_EQ(Package->GetEditRevision(), CreatedRevision);

	Package->MarkDirty();
	const Durin::uint64 FirstEditRevision = Package->GetEditRevision();
	EXPECT_GT(FirstEditRevision, CreatedRevision);
	EXPECT_TRUE(Package->IsDirty());

	Package->MarkDirty();
	EXPECT_GT(Package->GetEditRevision(), FirstEditRevision);
	EXPECT_TRUE(Package->IsDirty());
}

TEST(FPackageAssetTests, SequentialPackageSavesPublishEarlierPackagesBeforeLaterFailure)
{
	InitializeAssetTests();
	Durin::FAssetPath FirstPath;
	Durin::FAssetPath BlockedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Stage0First", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Stage0Blocked/Second", BlockedPath));

	DPackageAssetForTest* First = nullptr;
	DPackageAssetForTest* Second = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, First));
	ASSERT_TRUE(Durin::Asset::CreateAsset(BlockedPath, Second));
	First->Value = 1;
	Second->Value = 2;

	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path Blocker = Root / "Stage0Blocked";
	{
		std::ofstream Stream(Blocker);
		ASSERT_TRUE(Stream.is_open());
		Stream << "blocks destination directory creation";
	}

	ASSERT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
	const Durin::Asset::FAssetResult SecondResult = Durin::Asset::SavePackage(Second->GetPackage());
	EXPECT_EQ(SecondResult.Error, Durin::Asset::EAssetError::IoError);

	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "Stage0First.dasset"));
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(FirstPath), nullptr);
	EXPECT_FALSE(First->GetPackage()->IsDirty());

	EXPECT_FALSE(std::filesystem::exists(Root / "Stage0Blocked" / "Second.dasset"));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(BlockedPath), nullptr);
	EXPECT_TRUE(Second->GetPackage()->IsDirty());
}

TEST(FPackageAssetTests, AtomicBundleSaveRestoresFilesRegistryAndDirtyStateOnFailure)
{
	InitializeAssetTests();
	Durin::FAssetPath ExistingPath;
	Durin::FAssetPath NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AtomicBundleExisting", ExistingPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AtomicBundleNew", NewPath));

	DPackageAssetForTest* Existing = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ExistingPath, Existing));
	Existing->Value = 11;
	ASSERT_TRUE(Durin::Asset::SavePackage(Existing->GetPackage()));
	const Durin::Asset::FAssetData ExistingRegistry =
		*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath);
	std::vector<Durin::uint8> ExistingBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExistingBytes, ExistingRegistry.PhysicalPath));

	DPackageAssetForTest* Added = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(NewPath, Added));
	Added->Value = 22;
	Existing->Value = 33;
	Existing->MarkPackageDirty();
	const std::array Packages = {
		Existing->GetPackage(),
		Added->GetPackage()};
	const Durin::Asset::FAssetResult Result = Durin::Asset::SavePackagesAtomically(
		Packages,
		{
			.RootPackage = Added->GetPackage(),
			.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
				return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRegistry;
			}});
	EXPECT_FALSE(Result);

	std::vector<Durin::uint8> RestoredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RestoredBytes, ExistingRegistry.PhysicalPath));
	EXPECT_EQ(RestoredBytes, ExistingBytes);
	EXPECT_EQ(*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath), ExistingRegistry);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(NewPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AtomicBundleNew.dasset"));
	EXPECT_TRUE(Existing->GetPackage()->IsDirty());
	EXPECT_TRUE(Added->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(Added->GetPackage()));
}

TEST(FPackageAssetTests, LoadsExternalDependenciesAndPreventsPrematureUnload)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Dependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Owner", OwnerPath));

	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	Dependency->Label = "Dependency";
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Dependency->GetPackage()));

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Owner->GetPackage()));
	ASSERT_EQ(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath)->Dependencies.size(), 1u);
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));

	DPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner));
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(), "/TestAssets/Dependency");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(DependencyPath)->GetAsset(), LoadedOwner->ExternalReference.Get());

	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().GetRegistry().ScanMountedContent());
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath), nullptr);
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, RejectsTruncatedPackagesWithoutCachingPartialObjects)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Corrupt", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Corrupt.dasset";
	std::filesystem::resize_file(File, 12);
	Durin::DObject* Loaded = nullptr;
	const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Loaded);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

TEST(FPackageAssetTests, VersionTwoDoesNotStoreItsOwnPathAndDirectoryMoveIsByteStable)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath, NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveSource", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Sub/MoveSource", NewPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Asset));
	Asset->Label = "movable";
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto OldFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "MoveSource.dasset";
	std::vector<Durin::uint8> Before;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Before, OldFile.generic_string()));
	EXPECT_EQ(*reinterpret_cast<const Durin::uint32*>(Before.data() + sizeof(Durin::uint32)), 2u);
	EXPECT_EQ(std::search(Before.begin(), Before.end(), OldPath.GetView().begin(), OldPath.GetView().end()), Before.end());

	ASSERT_TRUE(Durin::Asset::MoveAsset(OldPath, NewPath));
	const auto NewFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Sub" / "MoveSource.dasset";
	std::vector<Durin::uint8> After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, NewFile.generic_string()));
	EXPECT_EQ(Before, After);
	EXPECT_FALSE(std::filesystem::exists(OldFile));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(OldPath), nullptr);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(NewPath), nullptr);
}

TEST(FPackageAssetTests, MoveRewritesMountedReferrers)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath, NewPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveDependency", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RenamedDependency", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	ASSERT_TRUE(Durin::Asset::MoveAsset(OldPath, NewPath));
	const Durin::Asset::FAssetData* OwnerData = Durin::Asset::GetAssetRegistry().FindAsset(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_NE(std::ranges::find(OwnerData->Dependencies, NewPath), OwnerData->Dependencies.end());
	EXPECT_EQ(std::ranges::find(OwnerData->Dependencies, OldPath), OwnerData->Dependencies.end());
	EXPECT_EQ(Dependency->GetName(), NewPath.GetAssetName());
}

TEST(FPackageAssetTests, DeletesUnreferencedAssetAndRegistryEntry)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteMe", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteMe.dasset";
	ASSERT_TRUE(std::filesystem::exists(File));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

TEST(FPackageAssetTests, UnloadsAndDeletesLoadedUnreferencedAsset)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LoadedDelete", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_TRUE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());

	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "LoadedDelete.dasset"));
}

TEST(FPackageAssetTests, RejectsDeletingReferencedAssetWithoutChangingDisk)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteDependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(DependencyPath, Analysis));
	ASSERT_EQ(Analysis.DirectReferencers.size(), 1u);
	EXPECT_EQ(Analysis.DirectReferencers.front(), OwnerPath);
	EXPECT_FALSE(Analysis.CanDelete());
	EXPECT_EQ(Durin::Asset::DeleteAsset(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(DependencyPath), nullptr);
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(DependencyPath), nullptr);
	EXPECT_TRUE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteDependency.dasset"));
}

TEST(FPackageAssetTests, DeletesRegisteredCompanionFile)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteWithCompanion", Path));
	const std::filesystem::path Companion =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteWithCompanion.source";
	{
		std::ofstream Stream(Companion);
		Stream << "source";
	}
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Label = "companion:" + Companion.generic_string();
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	ASSERT_EQ(Analysis.CompanionFiles.size(), 1u);
	EXPECT_EQ(Analysis.CompanionFiles.front(), Companion);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_FALSE(std::filesystem::exists(Companion));
}

TEST(FPackageAssetTests, DeletesMainAssetWhenCompanionInspectionFails)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteCorruptPackage", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const std::filesystem::path File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteCorruptPackage.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_GT(Bytes.size(), 16u);
	WriteTestBytes(File, std::span<const Durin::uint8>(Bytes).first(16));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.Warning.empty());
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
}

TEST(FPackageAssetTests, DeleteAnalysisDoesNotLeaveTemporaryDependenciesLoaded)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AnalysisDependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AnalysisOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DependencyPath));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(OwnerPath, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(OwnerPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, VersionOneIsExplicitlyUnsupported)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyVersion", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LegacyVersion.dasset";
	std::fstream Stream(File, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(Stream.is_open());
	const Durin::uint32 Version = 1;
	Stream.seekp(sizeof(Durin::uint32));
	Stream.write(reinterpret_cast<const char*>(&Version), sizeof(Version));
	Stream.close();
	Durin::DObject* Loaded = nullptr;
	EXPECT_EQ(Durin::Asset::LoadAsset(Path, Loaded).Error, Durin::Asset::EAssetError::UnsupportedVersion);
	EXPECT_EQ(Loaded, nullptr);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
	ASSERT_FALSE(Durin::Asset::GetAssetRegistry().GetScanErrors().empty());
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetScanErrors().back().Error, Durin::Asset::EAssetError::UnsupportedVersion);
}

TEST(FPackageAssetTests, PersistentRegistryReconcilesChangesAndRecoversFromInvalidCache)
{
	InitializeAssetTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "RegistryReconciliation";
	const auto OriginalAssets = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto ContentA = WorkRoot / "ContentA";
	const auto ContentB = WorkRoot / "ContentB";
	const auto CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(ContentA);
	Durin::FAssetPath SeedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RegistryReconciliationSeed", SeedPath));
	DPackageAssetForTest* SeedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SeedPath, SeedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SeedAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SeedPath));
	const auto ValidSource = OriginalAssets / "RegistryReconciliationSeed.dasset";
	std::filesystem::copy_file(ValidSource, ContentA / "Alpha.dasset");
	std::filesystem::copy_file(ValidSource, ContentA / "Beta.dasset");
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentA.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	auto& Registry = Durin::Asset::FAssetManager::Get().GetRegistry();

	const Durin::uint64 RevisionBeforeInitialScan = Registry.GetRevision();
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_GT(Registry.GetRevision(), RevisionBeforeInitialScan);
	EXPECT_EQ(Registry.GetLastScanStats().Enumerated, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 2u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);
	const auto CacheFile = CacheRoot / "AssetRegistry" / "Registry.bin";
	std::vector<Durin::uint8> FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstCache, CacheFile.generic_string()));

	const Durin::uint64 StableRevision = Registry.GetRevision();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetRevision(), StableRevision);
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	std::vector<Durin::uint8> SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondCache, CacheFile.generic_string()));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto Alpha = ContentA / "Alpha.dasset";
	std::filesystem::last_write_time(Alpha, std::filesystem::last_write_time(Alpha) + std::chrono::seconds(2));
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_GT(Registry.GetRevision(), StableRevision);
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 1u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);

	std::filesystem::copy_file(ValidSource, ContentA / "Gamma.dasset");
	std::filesystem::remove(ContentA / "Beta.dasset");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Removed, 1u);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);

	std::filesystem::rename(ContentA / "Gamma.dasset", ContentA / "Delta.dasset");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Removed, 1u);

	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 2u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);

	const std::array<Durin::uint8, 3> CorruptCache = {1, 2, 3};
	WriteTestBytes(CacheFile, CorruptCache);
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());

	std::vector<Durin::uint8> IncompatibleCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(IncompatibleCache, CacheFile.generic_string()));
	const Durin::uint32 IncompatibleSchema = 99;
	std::memcpy(IncompatibleCache.data() + sizeof(Durin::uint32), &IncompatibleSchema, sizeof(IncompatibleSchema));
	WriteTestBytes(CacheFile, IncompatibleCache);
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());

	std::filesystem::create_directories(ContentB);
	for (const auto& Source : {Alpha, ContentA / "Delta.dasset"})
	{
		const auto Destination = ContentB / Source.filename();
		std::filesystem::copy_file(Source, Destination);
		std::filesystem::last_write_time(Destination, std::filesystem::last_write_time(Source));
	}
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentB.generic_string() + "/");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	const auto AdditionalContent = WorkRoot / "AdditionalContent";
	std::filesystem::create_directories(AdditionalContent);
	Durin::PathUtilities::RegisterMountPointForTests("/Additional/", AdditionalContent.generic_string() + "/");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_NE(Registry.GetCacheWarning().find("mount manifest changed"), std::string::npos);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	WriteTestBytes(BlockedCacheRoot, CorruptCache);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Registry.GetAssets().size(), 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());
}

TEST(FPackageAssetTests, PersistentRegistryFlushesSuccessfulMutationsAndIgnoresWriteFailures)
{
	InitializeAssetTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "RegistryMutationLifecycle";
	const auto ContentRoot = WorkRoot / "Content";
	const auto CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(ContentRoot);
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	auto& Registry = Durin::Asset::GetAssetRegistry();
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const Durin::uint64 EmptyRegistryRevision = Registry.GetRevision();

	Durin::FAssetPath FirstPath;
	Durin::FAssetPath MovedPath;
	Durin::FAssetPath ImportedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleFirst", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleMoved", MovedPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleImported", ImportedPath));

	DPackageAssetForTest* FirstAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, FirstAsset));
	EXPECT_EQ(Registry.GetRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ASSERT_TRUE(Durin::Asset::SavePackage(FirstAsset->GetPackage()));
	EXPECT_GT(Registry.GetRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Registry.IsPersistentSnapshotDirty());
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	Durin::DObject* Reloaded = nullptr;
	const Durin::uint64 RevisionBeforeLoad = Registry.GetRevision();
	ASSERT_TRUE(Durin::Asset::LoadAsset(FirstPath, Reloaded));
	EXPECT_EQ(Registry.GetRevision(), RevisionBeforeLoad);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	ASSERT_TRUE(Durin::Asset::MoveAsset(FirstPath, MovedPath));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.FindAsset(FirstPath), nullptr);
	EXPECT_NE(Registry.FindAsset(MovedPath), nullptr);

	DPackageAssetForTest* ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ImportedPath, ImportedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	ASSERT_TRUE(Durin::Asset::DeleteAsset(MovedPath));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.FindAsset(MovedPath), nullptr);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	const std::array<Durin::uint8, 3> Blocker = {1, 2, 3};
	WriteTestBytes(BlockedCacheRoot, Blocker);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ImportedPath, ImportedAsset));
	ImportedAsset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	const auto AuthoredFile = ContentRoot / "LifecycleImported.dasset";
	std::vector<Durin::uint8> BeforeFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeFailedFlush, AuthoredFile.generic_string()));
	ShutdownAssetManagerForRestart();
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	EXPECT_FALSE(Registry.GetCacheWarning().empty());
	std::vector<Durin::uint8> AfterFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterFailedFlush, AuthoredFile.generic_string()));
	EXPECT_EQ(AfterFailedFlush, BeforeFailedFlush);

	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Registry.IsPersistentSnapshotDirty());
}
