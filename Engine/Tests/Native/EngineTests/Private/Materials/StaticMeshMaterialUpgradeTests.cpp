#include "MaterialTestSupport.h"
#include "Misc/DerivedDataCache.h"
#include "NativeTestSupport.h"

#include <unordered_set>

namespace
{
	struct FLegacyFieldRecord
	{
		std::string DeclaringClass;
		std::string Name;
		Durin::uint8 Kind = 0;
		std::string TypeSignature;
		std::vector<Durin::uint8> Payload;
	};

	struct FLegacyObjectRecord
	{
		Durin::uint64 Id = 0;
		Durin::uint64 OuterId = 0;
		std::string ClassName;
		std::string ObjectName;
		std::vector<FLegacyFieldRecord> Fields;
	};

	struct FLegacyPackage
	{
		Durin::uint32 Magic = 0;
		Durin::uint32 Version = 0;
		std::string AssetClassName;
		std::vector<std::string> Dependencies;
		std::vector<FLegacyObjectRecord> Objects;
	};

	class FLegacyPackageReader
	{
	public:
		explicit FLegacyPackageReader(std::span<const Durin::uint8> InBytes) : Bytes(InBytes) {}

		template<typename T>
		auto Read(T& Value) -> bool
		{
			if (Offset + sizeof(T) > Bytes.size()) return false;
			std::memcpy(&Value, Bytes.data() + Offset, sizeof(T));
			Offset += sizeof(T);
			return true;
		}

		auto ReadString(std::string& Value) -> bool
		{
			Durin::uint64 Size = 0;
			if (!Read(Size) || Size > Bytes.size() - Offset) return false;
			Value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), static_cast<size_t>(Size));
			Offset += static_cast<size_t>(Size);
			return true;
		}

		auto ReadBytes(std::vector<Durin::uint8>& Value) -> bool
		{
			Durin::uint64 Size = 0;
			if (!Read(Size) || Size > Bytes.size() - Offset) return false;
			Value.assign(Bytes.begin() + Offset, Bytes.begin() + Offset + static_cast<size_t>(Size));
			Offset += static_cast<size_t>(Size);
			return true;
		}

		auto AtEnd() const -> bool { return Offset == Bytes.size(); }

	private:
		std::span<const Durin::uint8> Bytes;
		size_t Offset = 0;
	};

	class FLegacyPackageWriter
	{
	public:
		template<typename T>
		auto Write(const T& Value) -> void
		{
			const auto* Data = reinterpret_cast<const Durin::uint8*>(&Value);
			Bytes.insert(Bytes.end(), Data, Data + sizeof(T));
		}

		auto WriteString(std::string_view Value) -> void
		{
			Write(static_cast<Durin::uint64>(Value.size()));
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}

		auto WriteBytes(std::span<const Durin::uint8> Value) -> void
		{
			Write(static_cast<Durin::uint64>(Value.size()));
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}

		std::vector<Durin::uint8> Bytes;
	};

	auto ReadLegacyPackage(std::span<const Durin::uint8> Bytes, FLegacyPackage& OutPackage) -> bool
	{
		FLegacyPackageReader Reader(Bytes);
		Durin::uint64 Count = 0;
		if (!Reader.Read(OutPackage.Magic)
			|| !Reader.Read(OutPackage.Version)
			|| !Reader.ReadString(OutPackage.AssetClassName)
			|| !Reader.Read(Count)) return false;
		OutPackage.Dependencies.resize(static_cast<size_t>(Count));
		for (std::string& Dependency : OutPackage.Dependencies)
			if (!Reader.ReadString(Dependency)) return false;
		if (!Reader.Read(Count)) return false;
		OutPackage.Objects.resize(static_cast<size_t>(Count));
		for (FLegacyObjectRecord& Object : OutPackage.Objects)
		{
			if (!Reader.Read(Object.Id)
				|| !Reader.Read(Object.OuterId)
				|| !Reader.ReadString(Object.ClassName)
				|| !Reader.ReadString(Object.ObjectName)
				|| !Reader.Read(Count)) return false;
			Object.Fields.resize(static_cast<size_t>(Count));
			for (FLegacyFieldRecord& Field : Object.Fields)
			{
				if (!Reader.ReadString(Field.DeclaringClass)
					|| !Reader.ReadString(Field.Name)
					|| !Reader.Read(Field.Kind)
					|| !Reader.ReadString(Field.TypeSignature)
					|| !Reader.ReadBytes(Field.Payload)) return false;
			}
		}
		return Reader.AtEnd();
	}

	auto WriteLegacyPackage(const FLegacyPackage& Package) -> std::vector<Durin::uint8>
	{
		FLegacyPackageWriter Writer;
		Writer.Write(Package.Magic);
		Writer.Write(Package.Version);
		Writer.WriteString(Package.AssetClassName);
		Writer.Write(static_cast<Durin::uint64>(Package.Dependencies.size()));
		for (const std::string& Dependency : Package.Dependencies) Writer.WriteString(Dependency);
		Writer.Write(static_cast<Durin::uint64>(Package.Objects.size()));
		for (const FLegacyObjectRecord& Object : Package.Objects)
		{
			Writer.Write(Object.Id);
			Writer.Write(Object.OuterId);
			Writer.WriteString(Object.ClassName);
			Writer.WriteString(Object.ObjectName);
			Writer.Write(static_cast<Durin::uint64>(Object.Fields.size()));
			for (const FLegacyFieldRecord& Field : Object.Fields)
			{
				Writer.WriteString(Field.DeclaringClass);
				Writer.WriteString(Field.Name);
				Writer.Write(Field.Kind);
				Writer.WriteString(Field.TypeSignature);
				Writer.WriteBytes(Field.Payload);
			}
		}
		return std::move(Writer.Bytes);
	}

	auto MakeObjectReferencePayload(std::optional<std::string_view> AssetPath) -> std::vector<Durin::uint8>
	{
		FLegacyPackageWriter Writer;
		if (!AssetPath)
		{
			Writer.Write(Durin::uint8(0));
			return std::move(Writer.Bytes);
		}
		Writer.Write(Durin::uint8(2));
		Writer.WriteString(*AssetPath);
		return std::move(Writer.Bytes);
	}

	auto MakeObjectReferenceArrayPayload(
		std::span<const std::optional<std::string_view>> AssetPaths) -> std::vector<Durin::uint8>
	{
		FLegacyPackageWriter Writer;
		Writer.Write(static_cast<Durin::uint64>(AssetPaths.size()));
		for (const std::optional<std::string_view> AssetPath : AssetPaths)
		{
			const std::vector<Durin::uint8> Reference = MakeObjectReferencePayload(AssetPath);
			Writer.Bytes.insert(Writer.Bytes.end(), Reference.begin(), Reference.end());
		}
		return std::move(Writer.Bytes);
	}

	auto RewriteComponentWithLegacyMaterials(
		const std::filesystem::path& File,
		std::optional<std::string_view> Material,
		std::span<const std::optional<std::string_view>> Materials) -> bool
	{
		std::vector<Durin::uint8> Bytes;
		if (!Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string())) return false;
		FLegacyPackage Package;
		if (!ReadLegacyPackage(Bytes, Package)) return false;
		const auto ObjectIt = std::ranges::find(
			Package.Objects, "Durin::DStaticMeshComponent", &FLegacyObjectRecord::ClassName);
		if (ObjectIt == Package.Objects.end()) return false;
		std::erase_if(ObjectIt->Fields, [](const FLegacyFieldRecord& Field) {
			return Field.DeclaringClass == "Durin::DStaticMeshComponent"
				&& Field.Name == "MaterialOverrides";
		});
		ObjectIt->Fields.push_back({
			.DeclaringClass = "Durin::DStaticMeshComponent",
			.Name = "Material",
			.Kind = static_cast<Durin::uint8>(Durin::DurinCodeGen::EPropertyGenFlags::Object),
			.TypeSignature = "Object:Durin::DMaterialInterface:true",
			.Payload = MakeObjectReferencePayload(Material)});
		ObjectIt->Fields.push_back({
			.DeclaringClass = "Durin::DStaticMeshComponent",
			.Name = "Materials",
			.Kind = static_cast<Durin::uint8>(Durin::DurinCodeGen::EPropertyGenFlags::Array),
			.TypeSignature = "Array<Object:Durin::DMaterialInterface:true>",
			.Payload = MakeObjectReferenceArrayPayload(Materials)});
		Bytes = WriteLegacyPackage(Package);
		std::ofstream Stream(File, std::ios::binary | std::ios::trunc);
		if (!Stream.is_open()) return false;
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		return Stream.good();
	}

	auto RewriteUInt32Field(
		const std::filesystem::path& File,
		std::string_view ClassName,
		std::string_view FieldName,
		Durin::uint32 Value) -> bool
	{
		std::vector<Durin::uint8> Bytes;
		if (!Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string())) return false;
		FLegacyPackage Package;
		if (!ReadLegacyPackage(Bytes, Package)) return false;
		const auto ObjectIt =
			std::ranges::find(Package.Objects, ClassName, &FLegacyObjectRecord::ClassName);
		if (ObjectIt == Package.Objects.end()) return false;
		const auto FieldIt =
			std::ranges::find(ObjectIt->Fields, FieldName, &FLegacyFieldRecord::Name);
		if (FieldIt == ObjectIt->Fields.end() || FieldIt->Payload.size() != sizeof(Value))
			return false;
		std::memcpy(FieldIt->Payload.data(), &Value, sizeof(Value));
		Bytes = WriteLegacyPackage(Package);
		std::ofstream Stream(File, std::ios::binary | std::ios::trunc);
		if (!Stream.is_open()) return false;
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		return Stream.good();
	}

	struct FLegacyMaterialFixture
	{
		Durin::FAssetPath MeshPath;
		Durin::FAssetPath FirstMaterialPath;
		Durin::FAssetPath SecondMaterialPath;
		Durin::FAssetPath ThirdMaterialPath;
		Durin::FAssetPath ComponentPath;
		std::filesystem::path MeshFile;
		std::filesystem::path ComponentFile;
		std::array<Durin::FGuid, 2> SlotIds;
	};

	auto CreateLegacyMaterialFixture(std::string_view Name) -> FLegacyMaterialFixture
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "StaticMeshMaterialUpgrades";
		static std::unordered_set<std::filesystem::path> InitializedRoots;
		if (InitializedRoots.insert(Root).second)
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::PathUtilities::RegisterMountPoint(
				"/StaticMeshMaterialUpgrades/", Root.generic_string() + "/");
		}

		FLegacyMaterialFixture Fixture;
		const std::string BasePath = std::format("/StaticMeshMaterialUpgrades/{}", Name);
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(BasePath + "/Mesh", Fixture.MeshPath));
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(BasePath + "/First", Fixture.FirstMaterialPath));
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(BasePath + "/Second", Fixture.SecondMaterialPath));
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(BasePath + "/Third", Fixture.ThirdMaterialPath));
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(BasePath + "/Component", Fixture.ComponentPath));

		const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(Source.generic_string(), Fixture.MeshPath.ToString());
		EXPECT_TRUE(Import) << Import.Message;
		if (!Import || Import.Asset == nullptr) return Fixture;
		Fixture.SlotIds = {
			Import.Asset->GetMaterialSlot(0)->SlotId,
			Import.Asset->GetMaterialSlot(1)->SlotId};

		Durin::DMaterial* First = nullptr;
		Durin::DMaterial* Second = nullptr;
		Durin::DMaterial* Third = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Fixture.FirstMaterialPath, First));
		EXPECT_TRUE(Durin::Asset::CreateAsset(Fixture.SecondMaterialPath, Second));
		EXPECT_TRUE(Durin::Asset::CreateAsset(Fixture.ThirdMaterialPath, Third));
		EXPECT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
		EXPECT_TRUE(Durin::Asset::SavePackage(Second->GetPackage()));
		EXPECT_TRUE(Durin::Asset::SavePackage(Third->GetPackage()));

		Durin::DStaticMeshComponent* Component = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Fixture.ComponentPath, Component));
		if (Component == nullptr) return Fixture;
		Component->SetStaticMesh(Import.Asset);
		Component->SetMaterial(0, First);
		Component->SetMaterial(1, Second);
		EXPECT_TRUE(Durin::Asset::SavePackage(Component->GetPackage()));
		Fixture.MeshFile = Root / std::filesystem::path(Name) / "Mesh.dasset";
		Fixture.ComponentFile = Root / std::filesystem::path(Name) / "Component.dasset";
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Fixture.ComponentPath));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Fixture.ThirdMaterialPath));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Fixture.SecondMaterialPath));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Fixture.FirstMaterialPath));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Fixture.MeshPath));
		return Fixture;
	}

	auto ExpectSingleUpgradeIssue(
		const Durin::Asset::FAssetLoadReport& Report,
		Durin::Asset::EAssetCompatibilityClassification Classification,
		Durin::uint64 MigratedCount) -> const Durin::Asset::FAssetCompatibilityIssue&
	{
		EXPECT_EQ(Report.GetAffectedObjectCount(), 1u);
		EXPECT_EQ(Report.GetLegacyFieldCount(), 2u);
		EXPECT_EQ(Report.GetMigratedDataCount(), MigratedCount);
		EXPECT_EQ(Report.GetRiskItemCount(), 0u);
		EXPECT_FALSE(Report.HasRiskItems());
		EXPECT_EQ(Report.CompatibilityIssues.size(), 1u);
		if (Report.CompatibilityIssues.empty())
		{
			static const Durin::Asset::FAssetCompatibilityIssue Empty;
			return Empty;
		}
		const Durin::Asset::FAssetCompatibilityIssue& Issue = Report.CompatibilityIssues.front();
		EXPECT_EQ(Issue.Classification, Classification);
		EXPECT_EQ(Issue.HandlerId, "Engine.StaticMeshComponent.LegacyMaterials");
		EXPECT_EQ(Issue.MigratedDataCount, MigratedCount);
		return Issue;
	}

	auto MakeCurrentAssetData(
		const Durin::FAssetPath& Path,
		const std::filesystem::path& File) -> Durin::Asset::FAssetData
	{
		const Durin::Asset::FAssetData* Registered =
			Durin::Asset::GetAssetRegistry().FindAsset(Path);
		EXPECT_NE(Registered, nullptr);
		if (!Registered) return {};
		Durin::Asset::FAssetData Data = *Registered;
		std::error_code ErrorCode;
		Data.FileSize = std::filesystem::file_size(File, ErrorCode);
		EXPECT_FALSE(ErrorCode);
		Data.LastWriteTime = std::filesystem::last_write_time(File, ErrorCode);
		EXPECT_FALSE(ErrorCode);
		Data.LastWriteTimeTicks =
			Durin::DerivedDataCache::FileTimeToStableTicks(Data.LastWriteTime);
		return Data;
	}

}

TEST(FStaticMeshMaterialUpgradeTests, SemanticSlotVersionAuditsAndExecutesWithoutLoadingDuringAudit)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("SemanticSlotVersion");
	ASSERT_TRUE(RewriteUInt32Field(
		Fixture.MeshFile,
		"Durin::DStaticMesh",
		"MaterialSlotsVersion",
		0));

	const Durin::Asset::FAssetData Data =
		MakeCurrentAssetData(Fixture.MeshPath, Fixture.MeshFile);
	Durin::Asset::FAssetPackageAuditReport Audit;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Audit));
	ASSERT_EQ(Audit.State, Durin::Asset::EAssetPackageAuditState::SafeUpgrade);
	ASSERT_EQ(Audit.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(
		Audit.CompatibilityIssues.front().HandlerId,
		"Engine.StaticMesh.MaterialSlotsV1");
	EXPECT_TRUE(Audit.CompatibilityIssues.front().LegacyFields.empty());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Fixture.MeshPath), nullptr);

	Durin::Asset::FAssetPackageUpgradeResult Upgrade;
	const Durin::Asset::FAssetResult ExecutionResult =
		Durin::Asset::ExecutePackageUpgrade(Audit, Upgrade);
	ASSERT_FALSE(Upgrade.LoadReport.Mutations.empty()) << Upgrade.Diagnostic;
	EXPECT_FALSE(Upgrade.LoadReport.HasNonUpgradeMutations());
	ASSERT_TRUE(ExecutionResult)
		<< Upgrade.Diagnostic
		<< " audit-object=" << Audit.CompatibilityIssues.front().ObjectPath
		<< " mutation-object=" << Upgrade.LoadReport.Mutations.front().ObjectPath
		<< " mutation-handler=" << Upgrade.LoadReport.Mutations.front().HandlerId;
	EXPECT_TRUE(Upgrade.bSaved);
	EXPECT_EQ(Upgrade.State, Durin::Asset::EAssetPackageAuditState::UpToDate);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Fixture.MeshPath), nullptr);
	ASSERT_EQ(Upgrade.LoadReport.Mutations.size(), 1u);
	EXPECT_EQ(
		Upgrade.LoadReport.Mutations.front().HandlerId,
		"Engine.StaticMesh.MaterialSlotsV1");
	EXPECT_EQ(
		Upgrade.LoadReport.Mutations.front().Kind,
		Durin::Asset::EAssetLoadMutationKind::Upgrade);
}

TEST(FStaticMeshMaterialUpgradeTests, ObjectFreeAuditClassifiesEmptyLegacyFieldsAsSafe)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("AuditEmptyCleanup");
	const std::array<std::optional<std::string_view>, 0> Materials{};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(
		Fixture.ComponentFile, std::nullopt, Materials));

	const Durin::Asset::FAssetData Data =
		MakeCurrentAssetData(Fixture.ComponentPath, Fixture.ComponentFile);
	Durin::Asset::FAssetPackageAuditReport Report;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Report));
	EXPECT_EQ(Report.State, Durin::Asset::EAssetPackageAuditState::SafeUpgrade);
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(
		Report.CompatibilityIssues.front().Classification,
		Durin::Asset::EAssetCompatibilityClassification::SafeCleanup);
	EXPECT_EQ(
		Report.CompatibilityIssues.front().HandlerId,
		"Engine.StaticMeshComponent.LegacyMaterials");
	EXPECT_EQ(Report.CompatibilityIssues.front().MigratedDataCount, 0u);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Fixture.ComponentPath), nullptr);
}

TEST(FStaticMeshMaterialUpgradeTests, ObjectFreeAuditCountsLegacyAssignmentsWithoutLoading)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("AuditAssignments");
	const std::array<std::optional<std::string_view>, 3> Materials{
		Fixture.FirstMaterialPath.GetView(),
		std::nullopt,
		Fixture.ThirdMaterialPath.GetView()};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(
		Fixture.ComponentFile, Fixture.SecondMaterialPath.GetView(), Materials));

	const Durin::Asset::FAssetData Data =
		MakeCurrentAssetData(Fixture.ComponentPath, Fixture.ComponentFile);
	Durin::Asset::FAssetPackageAuditReport Report;
	ASSERT_TRUE(Durin::Asset::AuditAssetPackage(Data, Report));
	EXPECT_EQ(Report.State, Durin::Asset::EAssetPackageAuditState::SafeUpgrade);
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(
		Report.CompatibilityIssues.front().Classification,
		Durin::Asset::EAssetCompatibilityClassification::Migrated);
	EXPECT_EQ(Report.CompatibilityIssues.front().MigratedDataCount, 2u);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Fixture.ComponentPath), nullptr);
}

TEST(FStaticMeshMaterialUpgradeTests, EmptyLegacyFieldsAreOneSafeCleanup)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("EmptyCleanup");
	const std::array<std::optional<std::string_view>, 0> Materials{};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(Fixture.ComponentFile, std::nullopt, Materials));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.ComponentPath, Loaded, &Report));
	ASSERT_NE(Loaded, nullptr);
	const auto& Issue = ExpectSingleUpgradeIssue(
		Report, Durin::Asset::EAssetCompatibilityClassification::SafeCleanup, 0);
	EXPECT_EQ(Issue.LegacyFields.size(), 2u);
	EXPECT_TRUE(Loaded->GetMaterialOverrides().empty());
	EXPECT_TRUE(Loaded->GetPackage()->IsDirty());
	EXPECT_NE(Issue.MigrationSummary.find("no material assignments"), std::string::npos);
	ASSERT_EQ(Report.Mutations.size(), 1u);
	EXPECT_EQ(Report.Mutations.front().PackagePath, Fixture.ComponentPath);
	EXPECT_EQ(Report.Mutations.front().HandlerId, "Engine.StaticMeshComponent.LegacyMaterials");
	EXPECT_EQ(
		Report.Mutations.front().Kind,
		Durin::Asset::EAssetLoadMutationKind::Upgrade);
	EXPECT_FALSE(Report.HasNonUpgradeMutations());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.ComponentPath));
}

TEST(FStaticMeshMaterialUpgradeTests, SlotZeroFallbackAppliesOnlyWhenArrayLacksIndexZero)
{
	const FLegacyMaterialFixture FallbackFixture = CreateLegacyMaterialFixture("SlotZeroFallback");
	const std::array<std::optional<std::string_view>, 0> EmptyMaterials{};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(
		FallbackFixture.ComponentFile, FallbackFixture.FirstMaterialPath.GetView(), EmptyMaterials));
	Durin::DStaticMeshComponent* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(FallbackFixture.ComponentPath, Loaded, &Report));
	ExpectSingleUpgradeIssue(Report, Durin::Asset::EAssetCompatibilityClassification::Migrated, 1);
	EXPECT_EQ(Loaded->GetMaterialOverride(FallbackFixture.SlotIds[0])->GetPackage()->GetPackagePath(),
		FallbackFixture.FirstMaterialPath.ToString());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FallbackFixture.ComponentPath));

	const FLegacyMaterialFixture NullArrayFixture = CreateLegacyMaterialFixture("NullArrayZero");
	const std::array<std::optional<std::string_view>, 1> NullMaterials{std::nullopt};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(
		NullArrayFixture.ComponentFile, NullArrayFixture.FirstMaterialPath.GetView(), NullMaterials));
	Loaded = nullptr;
	Report = {};
	ASSERT_TRUE(Durin::Asset::LoadAsset(NullArrayFixture.ComponentPath, Loaded, &Report));
	ExpectSingleUpgradeIssue(Report, Durin::Asset::EAssetCompatibilityClassification::SafeCleanup, 0);
	EXPECT_TRUE(Loaded->GetMaterialOverrides().empty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(NullArrayFixture.ComponentPath));
}

TEST(FStaticMeshMaterialUpgradeTests, MultiSlotAndExcessEntriesPreserveEveryAssignment)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("MultiSlotOrphan");
	const std::array<std::optional<std::string_view>, 3> Materials{
		Fixture.FirstMaterialPath.GetView(),
		Fixture.SecondMaterialPath.GetView(),
		Fixture.ThirdMaterialPath.GetView()};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(
		Fixture.ComponentFile, Fixture.ThirdMaterialPath.GetView(), Materials));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.ComponentPath, Loaded, &Report));
	const auto& Issue = ExpectSingleUpgradeIssue(
		Report, Durin::Asset::EAssetCompatibilityClassification::Migrated, 3);
	ASSERT_EQ(Loaded->GetMaterialOverrides().size(), 3u);
	EXPECT_EQ(Loaded->GetMaterialOverride(Fixture.SlotIds[0])->GetPackage()->GetPackagePath(),
		Fixture.FirstMaterialPath.ToString());
	EXPECT_EQ(Loaded->GetMaterialOverride(Fixture.SlotIds[1])->GetPackage()->GetPackagePath(),
		Fixture.SecondMaterialPath.ToString());
	const auto Orphan = std::ranges::find_if(Loaded->GetMaterialOverrides(), [&](const Durin::FStaticMeshMaterialOverride& Override) {
		return Override.SlotId != Fixture.SlotIds[0] && Override.SlotId != Fixture.SlotIds[1];
	});
	ASSERT_NE(Orphan, Loaded->GetMaterialOverrides().end());
	EXPECT_TRUE(Loaded->IsMaterialOverrideOrphan(Orphan->SlotId));
	EXPECT_EQ(Orphan->Material->GetPackage()->GetPackagePath(), Fixture.ThirdMaterialPath.ToString());
	const Durin::FGuid OrphanSlotId = Orphan->SlotId;
	EXPECT_NE(Issue.MigrationSummary.find("legacy index 0"), std::string::npos);
	EXPECT_NE(Issue.MigrationSummary.find("legacy index 1"), std::string::npos);
	EXPECT_NE(Issue.MigrationSummary.find("legacy index 2"), std::string::npos);
	EXPECT_NE(Issue.MigrationSummary.find("orphan"), std::string::npos);

	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.ComponentPath));
	Loaded = nullptr;
	Report = {};
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.ComponentPath, Loaded, &Report));
	EXPECT_FALSE(Report.HasCompatibilityIssues());
	ASSERT_EQ(Loaded->GetMaterialOverrides().size(), 3u);
	EXPECT_EQ(Loaded->GetMaterialOverride(Fixture.SlotIds[0])->GetPackage()->GetPackagePath(),
		Fixture.FirstMaterialPath.ToString());
	EXPECT_EQ(Loaded->GetMaterialOverride(Fixture.SlotIds[1])->GetPackage()->GetPackagePath(),
		Fixture.SecondMaterialPath.ToString());
	const auto ReloadedOrphan = std::ranges::find_if(
		Loaded->GetMaterialOverrides(), [&](const Durin::FStaticMeshMaterialOverride& Override) {
			return Override.SlotId != Fixture.SlotIds[0] && Override.SlotId != Fixture.SlotIds[1];
	});
	ASSERT_NE(ReloadedOrphan, Loaded->GetMaterialOverrides().end());
	EXPECT_EQ(ReloadedOrphan->SlotId, OrphanSlotId);
	EXPECT_EQ(ReloadedOrphan->Material->GetPackage()->GetPackagePath(), Fixture.ThirdMaterialPath.ToString());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.ComponentPath));
}

TEST(FStaticMeshMaterialUpgradeTests, NullEntriesAreSkipped)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("NullEntries");
	const std::array<std::optional<std::string_view>, 3> Materials{
		std::nullopt, Fixture.SecondMaterialPath.GetView(), std::nullopt};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(Fixture.ComponentFile, std::nullopt, Materials));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.ComponentPath, Loaded, &Report));
	ExpectSingleUpgradeIssue(Report, Durin::Asset::EAssetCompatibilityClassification::Migrated, 1);
	ASSERT_EQ(Loaded->GetMaterialOverrides().size(), 1u);
	EXPECT_EQ(Loaded->GetMaterialOverride(Fixture.SlotIds[1])->GetPackage()->GetPackagePath(),
		Fixture.SecondMaterialPath.ToString());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.ComponentPath));
}

TEST(FStaticMeshMaterialUpgradeTests, IncompatibleReferencedObjectsRejectTheLoad)
{
	const FLegacyMaterialFixture Fixture = CreateLegacyMaterialFixture("WrongType");
	const std::array<std::optional<std::string_view>, 1> Materials{Fixture.MeshPath.GetView()};
	ASSERT_TRUE(RewriteComponentWithLegacyMaterials(Fixture.ComponentFile, std::nullopt, Materials));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Fixture.ComponentPath, Loaded, &Report);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::TypeMismatch);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_FALSE(Durin::Asset::FindLoadedPackage(Fixture.ComponentPath));
}
