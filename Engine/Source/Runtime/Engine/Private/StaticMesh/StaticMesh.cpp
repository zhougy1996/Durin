#include "StaticMesh/StaticMesh.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMeshResources.h"

#include "RHICommandList.h"

namespace Durin
{
	namespace
	{
		inline constexpr uint32 StaticMeshMaterialSlotsVersion = 1;
		inline constexpr std::string_view LegacySlotGuidDomain = "Durin.StaticMeshMaterialSlot.v1";
		constexpr float VectorTolerance = 1.0e-10f;

		auto MakeGuidFromHash(const FXxHash128& Hash) -> FGuid
		{
			FGuid Result(
				static_cast<uint32>(Hash.HashLow),
				static_cast<uint32>(Hash.HashLow >> 32),
				static_cast<uint32>(Hash.HashHigh),
				static_cast<uint32>(Hash.HashHigh >> 32));
			// A valid identity is required even for the theoretical all-zero hash result.
			if (!Result.IsValid()) Result = FGuid(0, 0, 0, 1);
			return Result;
		}

		auto MakeLegacySlotGuid(
			std::string_view PackagePath,
			std::string_view SourceName,
			uint32 SourceMaterialIndex) -> FGuid
		{
			FXxHash128Builder Builder;
			Builder.Update(LegacySlotGuidDomain);
			Builder.Update(PackagePath);
			Builder.Update(SourceName);
			const std::array<uint8, 4> LittleEndianIndex{
				static_cast<uint8>(SourceMaterialIndex),
				static_cast<uint8>(SourceMaterialIndex >> 8),
				static_cast<uint8>(SourceMaterialIndex >> 16),
				static_cast<uint8>(SourceMaterialIndex >> 24)};
			Builder.Update(std::span<const uint8>(LittleEndianIndex));
			return MakeGuidFromHash(Builder.Finalize());
		}

		auto SlotDefinitionsEqual(
			std::span<const FStaticMeshMaterialSlotDefinition> A,
			std::span<const FStaticMeshMaterialSlotDefinition> B) -> bool
		{
			if (A.size() != B.size()) return false;
			for (size_t Index = 0; Index < A.size(); ++Index)
			{
				if (A[Index].SlotId != B[Index].SlotId
					|| A[Index].Name != B[Index].Name
					|| A[Index].SourceName != B[Index].SourceName
					|| A[Index].SourceMaterialIndex != B[Index].SourceMaterialIndex
					|| A[Index].DefaultMaterial != B[Index].DefaultMaterial) return false;
			}
			return true;
		}

		auto ImportAxisVector(EStaticMeshImportAxis Axis, FVector3f& OutVector, uint32& OutComponent) -> bool
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: OutVector = FVector3f(1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::NegativeX: OutVector = FVector3f(-1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::PositiveY: OutVector = FVector3f(0.0f, 1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::NegativeY: OutVector = FVector3f(0.0f, -1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::PositiveZ: OutVector = FVector3f(0.0f, 0.0f, 1.0f); OutComponent = 2; return true;
			case EStaticMeshImportAxis::NegativeZ: OutVector = FVector3f(0.0f, 0.0f, -1.0f); OutComponent = 2; return true;
			}
			return false;
		}

		auto MakeImportOptions(const FStaticMeshImportSettings& Settings) -> Asset::FMeshImportOptions
		{
			FVector3f Forward;
			FVector3f Right;
			FVector3f Up;
			uint32 UnusedComponent = 0;
			ImportAxisVector(Settings.ForwardAxis, Forward, UnusedComponent);
			ImportAxisVector(Settings.RightAxis, Right, UnusedComponent);
			ImportAxisVector(Settings.UpAxis, Up, UnusedComponent);

			Asset::FMeshImportOptions Options;
			Options.SourceToEngine = glm::mat4(1.0f);
			for (uint32 SourceComponent = 0; SourceComponent < 3; ++SourceComponent)
			{
				Options.SourceToEngine[SourceComponent][0] = Forward[SourceComponent];
				Options.SourceToEngine[SourceComponent][1] = Right[SourceComponent];
				Options.SourceToEngine[SourceComponent][2] = Up[SourceComponent];
			}
			return Options;
		}

		auto IsFinite(const FVector2f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		auto IsFinite(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		auto IsFinite(const FVector4f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		auto SafeNormalize(const FVector3f& Value, const FVector3f& Fallback) -> FVector3f
		{
			const float LengthSquared = glm::dot(Value, Value);
			return IsFinite(Value) && std::isfinite(LengthSquared) && LengthSquared > VectorTolerance
				? Value / std::sqrt(LengthSquared)
				: Fallback;
		}

		auto MakeStableTangent(const FVector3f& Normal) -> FVector3f
		{
			const FVector3f Axis = std::abs(Normal.z) < 0.999f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(0.0f, 1.0f, 0.0f);
			return SafeNormalize(glm::cross(Axis, Normal), FVector3f(1.0f, 0.0f, 0.0f));
		}

		auto BuildNormals(const std::vector<FVector3f>& Positions, const std::vector<uint32>& Indices) -> std::vector<FVector3f>
		{
			std::vector<FVector3f> Normals(Positions.size(), FVector3f(0.0f));
			for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
			{
				const uint32 I0 = Indices[Index];
				const uint32 I1 = Indices[Index + 1];
				const uint32 I2 = Indices[Index + 2];
				const FVector3f FaceNormal = glm::cross(Positions[I1] - Positions[I0], Positions[I2] - Positions[I0]);
				if (!IsFinite(FaceNormal) || glm::dot(FaceNormal, FaceNormal) <= VectorTolerance) continue;
				Normals[I0] += FaceNormal;
				Normals[I1] += FaceNormal;
				Normals[I2] += FaceNormal;
			}
			for (FVector3f& Normal : Normals) Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
			return Normals;
		}

		auto BuildTangents(
			const std::vector<FVector3f>& Positions,
			const std::vector<FVector3f>& Normals,
			const std::vector<FVector2f>& UV0,
			const std::vector<uint32>& Indices) -> std::vector<FVector4f>
		{
			std::vector<FVector3f> TangentAccum(Positions.size(), FVector3f(0.0f));
			std::vector<FVector3f> BitangentAccum(Positions.size(), FVector3f(0.0f));
			const bool bHasUV0 = UV0.size() == Positions.size();
			if (bHasUV0)
			{
				for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
				{
					const uint32 I0 = Indices[Index];
					const uint32 I1 = Indices[Index + 1];
					const uint32 I2 = Indices[Index + 2];
					const FVector3f Edge1 = Positions[I1] - Positions[I0];
					const FVector3f Edge2 = Positions[I2] - Positions[I0];
					const FVector2f DeltaUV1 = UV0[I1] - UV0[I0];
					const FVector2f DeltaUV2 = UV0[I2] - UV0[I0];
					const float Determinant = DeltaUV1.x * DeltaUV2.y - DeltaUV1.y * DeltaUV2.x;
					if (!std::isfinite(Determinant) || std::abs(Determinant) <= VectorTolerance) continue;
					const float InverseDeterminant = 1.0f / Determinant;
					const FVector3f Tangent = (Edge1 * DeltaUV2.y - Edge2 * DeltaUV1.y) * InverseDeterminant;
					const FVector3f Bitangent = (Edge2 * DeltaUV1.x - Edge1 * DeltaUV2.x) * InverseDeterminant;
					if (!IsFinite(Tangent) || !IsFinite(Bitangent)) continue;
					for (uint32 VertexIndex : {I0, I1, I2})
					{
						TangentAccum[VertexIndex] += Tangent;
						BitangentAccum[VertexIndex] += Bitangent;
					}
				}
			}

			std::vector<FVector4f> Tangents(Positions.size());
			for (size_t VertexIndex = 0; VertexIndex < Positions.size(); ++VertexIndex)
			{
				const FVector3f& Normal = Normals[VertexIndex];
				const FVector3f Orthogonalized = TangentAccum[VertexIndex] - Normal * glm::dot(Normal, TangentAccum[VertexIndex]);
				const FVector3f Tangent = SafeNormalize(Orthogonalized, MakeStableTangent(Normal));
				const float Sign = glm::dot(glm::cross(Normal, Tangent), BitangentAccum[VertexIndex]) < 0.0f ? -1.0f : 1.0f;
				Tangents[VertexIndex] = FVector4f(Tangent, Sign);
			}
			return Tangents;
		}

		auto HasValidNormals(const std::vector<FVector3f>& Normals, size_t NumVertices) -> bool
		{
			return Normals.size() == NumVertices && std::ranges::all_of(Normals, [](const FVector3f& Normal) {
				return IsFinite(Normal) && glm::dot(Normal, Normal) > VectorTolerance;
			});
		}

		auto HasValidTangents(const std::vector<FVector4f>& Tangents, size_t NumVertices) -> bool
		{
			return Tangents.size() == NumVertices && std::ranges::all_of(Tangents, [](const FVector4f& Tangent) {
				const FVector3f Direction(Tangent);
				return IsFinite(Tangent) && glm::dot(Direction, Direction) > VectorTolerance && std::abs(Tangent.w) > 0.5f;
			});
		}

		auto ValidateImportedMesh(const Asset::FImportedMeshData& Mesh, std::string& OutError) -> bool
		{
			if (Mesh.Positions.empty() || Mesh.Indices.empty()) return false;
			if (Mesh.Positions.size() > std::numeric_limits<uint32>::max())
			{
				OutError = std::format("Mesh '{}' exceeds the uint32 vertex limit.", Mesh.Name);
				return false;
			}
			if (Mesh.Indices.size() % 3 != 0)
			{
				OutError = std::format("Mesh '{}' index count is not a triangle list.", Mesh.Name);
				return false;
			}
			if (!std::ranges::all_of(Mesh.Positions, [](const FVector3f& Position) { return IsFinite(Position); }))
			{
				OutError = std::format("Mesh '{}' contains a non-finite position.", Mesh.Name);
				return false;
			}
			for (uint32 Index : Mesh.Indices)
			{
				if (Index >= Mesh.Positions.size())
				{
					OutError = std::format("Mesh '{}' contains an out-of-range index {}.", Mesh.Name, Index);
					return false;
				}
			}
			return true;
		}

		auto FindMaterialSlotIndex(const FStaticMeshRenderData& RenderData, uint32 SourceMaterialIndex, uint32& OutSlotIndex) -> bool
		{
			const auto It = std::ranges::find(RenderData.MaterialSlots, SourceMaterialIndex, &FStaticMeshMaterialSlot::SourceMaterialIndex);
			if (It == RenderData.MaterialSlots.end()) return false;
			OutSlotIndex = static_cast<uint32>(std::distance(RenderData.MaterialSlots.begin(), It));
			return true;
		}

		auto MakeUniqueSectionName(std::string Name, uint32 Index, std::unordered_map<std::string, uint32>& NameCounts) -> std::string
		{
			if (Name.empty()) Name = std::format("Section_{}", Index);
			uint32& Count = NameCounts[Name];
			const std::string Result = Count == 0 ? Name : std::format("{}_{}", Name, Count);
			++Count;
			return Result;
		}

		auto ResolveMountedFile(std::string_view VirtualPath) -> std::filesystem::path
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (VirtualPath.starts_with(Mount.VirtualRoot))
				{
					return (std::filesystem::path(Mount.PhysicalPath) / std::string(VirtualPath.substr(Mount.VirtualRoot.size()))).lexically_normal();
				}
			}
			return std::filesystem::path(VirtualPath).lexically_normal();
		}

		auto ResolveStaticMeshSource(const DStaticMesh& Mesh) -> std::filesystem::path
		{
			const std::filesystem::path StoredPath(Mesh.GetSourceFile());
			const std::filesystem::path PackageFile = ResolveMountedFile(Mesh.GetPackage()->GetPackagePath());
			if (!StoredPath.is_absolute() && !Mesh.GetSourceFile().starts_with('/'))
			{
				return (PackageFile.parent_path() / StoredPath).lexically_normal();
			}

			const std::filesystem::path LegacyPath = ResolveMountedFile(Mesh.GetSourceFile());
			if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
			return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
		}
	}

	auto FStaticMeshImportSettings::IsValid(std::string* OutError) const -> bool
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
		{
			if (OutError) *OutError = "The import coordinate system contains an unknown axis.";
			return false;
		}
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
		{
			if (OutError) *OutError = "Forward, Right, and Up must use X, Y, and Z exactly once.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto FStaticMeshImportSettings::MakeDurin() -> FStaticMeshImportSettings
	{
		return {};
	}

	auto FStaticMeshImportSettings::MakeYUpNegativeZForward() -> FStaticMeshImportSettings
	{
		return {
			.ForwardAxis = EStaticMeshImportAxis::NegativeZ,
			.RightAxis = EStaticMeshImportAxis::PositiveX,
			.UpAxis = EStaticMeshImportAxis::PositiveY
		};
	}

	DStaticMesh::DStaticMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		static const bool RegisteredMoveContributor = [] {
			Asset::RegisterAssetMoveContributor(DStaticMesh::StaticClass(), [](DObject* Object, const FAssetPath& OldPath, const FAssetPath& NewPath, Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult {
				auto* Mesh = Cast<DStaticMesh>(Object);
				if (!Mesh || Mesh->SourceFile.empty()) return {};
				const std::string Original = Mesh->SourceFile;
				const std::filesystem::path OldPackage = ResolveMountedFile(OldPath.ToString());
				const std::filesystem::path NewPackage = ResolveMountedFile(NewPath.ToString());
				const std::filesystem::path SourceName(Original);
				const std::filesystem::path OldSource = SourceName.is_absolute() ? SourceName : OldPackage.parent_path() / SourceName;
				const std::string NewFileName = OldPath.GetAssetName() == NewPath.GetAssetName()
					? SourceName.filename().generic_string()
					: std::string(NewPath.GetAssetName()) + SourceName.extension().generic_string();
				const std::filesystem::path NewSource = NewPackage.parent_path() / NewFileName;
				if (OldSource.lexically_normal() != NewSource.lexically_normal()) Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
				if (NewFileName != Original)
				{
					Out.Apply = [Mesh, NewFileName] { Mesh->SourceFile = NewFileName; };
					Out.Rollback = [Mesh, Original] { Mesh->SourceFile = Original; };
				}
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DStaticMesh::StaticClass(), [](DObject* Object, Asset::FAssetDeleteContribution& Out) -> Asset::FAssetResult {
				auto* Mesh = Cast<DStaticMesh>(Object);
				if (Mesh && !Mesh->SourceFile.empty()) Out.Files.push_back(ResolveStaticMeshSource(*Mesh));
				return {};
			});
			return true;
		}();
		(void)RegisteredMoveContributor;
	}

	DStaticMesh::~DStaticMesh() = default;

	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetRenderData() -> FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DStaticMesh::FindMaterialSlot(const FGuid& SlotId) const -> const FStaticMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, SlotId, &FStaticMeshMaterialSlotDefinition::SlotId);
		return It == MaterialSlots.end() ? nullptr : &*It;
	}

	auto DStaticMesh::FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name, &FStaticMeshMaterialSlotDefinition::Name);
		return It == MaterialSlots.end() ? nullptr : &*It;
	}

	auto DStaticMesh::SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void
	{
		if (InRenderData != nullptr) InRenderData->RecalculateBounds();
		RenderData = std::move(InRenderData);
	}

	auto DStaticMesh::CreateDebugTriangle(DObject* Outer) -> DStaticMesh*
	{
		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, "DebugStaticMesh");
		Mesh->MaterialSlotsVersion = StaticMeshMaterialSlotsVersion;
		Mesh->MaterialSlots.push_back({.SlotId = FGuid::NewGuid(), .Name = FName("Default"), .SourceMaterialIndex = 0});
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.push_back({"Default", 0, Mesh->MaterialSlots[0].SlotId});
		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		LOD.Positions = {
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		};
		LOD.Indices = {0, 1, 2};
		LOD.Normals = {
			FVector3f(0.0f, 0.0f, 1.0f),
			FVector3f(0.0f, 0.0f, 1.0f),
			FVector3f(0.0f, 0.0f, 1.0f)
		};
		LOD.Tangents.assign(LOD.Positions.size(), FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
		for (auto& TexCoords : LOD.TexCoords) TexCoords.assign(LOD.Positions.size(), FVector2f(0.0f));
		LOD.Colors.assign(LOD.Positions.size(), FVector4f(1.0f));
		LOD.Sections.push_back({"Default", 0, 3, 0, 2, 0, {}});
		Mesh->SetRenderData(std::move(RenderData));
		return Mesh;
	}

	auto DStaticMesh::CreateTransientFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		std::string& OutError,
		const FStaticMeshImportSettings& InImportSettings) -> DStaticMesh*
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
		{
			OutError = std::format("Static mesh source file does not exist: {}", Input.generic_string());
			return nullptr;
		}
		if (!InImportSettings.IsValid(&OutError)) return nullptr;

		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, ObjectName);
		Mesh->ImportSettings = InImportSettings;
		if (Mesh->BuildRenderData(Input.generic_string(), OutError)) return Mesh;
		MarkAsGarbage(Mesh);
		return nullptr;
	}

	auto DStaticMesh::BuildRenderData(std::string_view FilePath, std::string& OutError) -> bool
	{
		Asset::FImportedSceneData ImportedScene;
		std::string ImportSettingsError;
		if (!ImportSettings.IsValid(&ImportSettingsError))
		{
			OutError = std::move(ImportSettingsError);
			return false;
		}
		if (!Asset::ImportFromFile(FilePath, ImportedScene, MakeImportOptions(ImportSettings)))
		{
			OutError = std::format("Failed to import static mesh source file: {}", FilePath);
			return false;
		}

		const bool bVersionZeroMigration = MaterialSlotsVersion == 0 && !SourceFile.empty();
		const std::vector<FStaticMeshMaterialSlotDefinition> PreviousSlots = MaterialSlots;
		std::vector<FStaticMeshMaterialSlotDefinition> ReconciledSlots(ImportedScene.MaterialSlots.size());
		std::vector<bool> OldConsumed(PreviousSlots.size(), false);
		std::vector<bool> NewMatched(ReconciledSlots.size(), false);
		std::unordered_map<std::string, uint32> OldNameCounts;
		std::unordered_map<std::string, uint32> NewNameCounts;
		for (const FStaticMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldNameCounts[Slot.SourceName];
		for (const Asset::FImportedMaterialSlot& Slot : ImportedScene.MaterialSlots) ++NewNameCounts[Slot.SourceName];

		auto PreserveSlot = [&](size_t NewIndex, size_t OldIndex) {
			const Asset::FImportedMaterialSlot& Imported = ImportedScene.MaterialSlots[NewIndex];
			ReconciledSlots[NewIndex] = PreviousSlots[OldIndex];
			ReconciledSlots[NewIndex].Name = FName(Imported.Name);
			ReconciledSlots[NewIndex].SourceName = Imported.SourceName;
			ReconciledSlots[NewIndex].SourceMaterialIndex = Imported.SourceMaterialIndex;
			OldConsumed[OldIndex] = true;
			NewMatched[NewIndex] = true;
		};

		for (size_t NewIndex = 0; NewIndex < ImportedScene.MaterialSlots.size(); ++NewIndex)
		{
			const std::string& SourceName = ImportedScene.MaterialSlots[NewIndex].SourceName;
			if (OldNameCounts[SourceName] != 1 || NewNameCounts[SourceName] != 1) continue;
			const auto It = std::ranges::find(PreviousSlots, SourceName, &FStaticMeshMaterialSlotDefinition::SourceName);
			if (It != PreviousSlots.end()) PreserveSlot(NewIndex, static_cast<size_t>(It - PreviousSlots.begin()));
		}

		for (size_t NewIndex = 0; NewIndex < ImportedScene.MaterialSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const Asset::FImportedMaterialSlot& Imported = ImportedScene.MaterialSlots[NewIndex];
			if (NewNameCounts[Imported.SourceName] != 1) continue;
			for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
			{
				const FStaticMeshMaterialSlotDefinition& Previous = PreviousSlots[OldIndex];
				if (OldConsumed[OldIndex] || OldNameCounts[Previous.SourceName] != 1
					|| Previous.SourceMaterialIndex != Imported.SourceMaterialIndex) continue;
				PreserveSlot(NewIndex, OldIndex);
				break;
			}
		}

		const std::string PackagePath = GetPackage() ? GetPackage()->GetPackagePath() : std::string{};
		for (size_t NewIndex = 0; NewIndex < ImportedScene.MaterialSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const Asset::FImportedMaterialSlot& Imported = ImportedScene.MaterialSlots[NewIndex];
			FStaticMeshMaterialSlotDefinition& Definition = ReconciledSlots[NewIndex];
			Definition.SlotId = bVersionZeroMigration
				? MakeLegacySlotGuid(PackagePath, Imported.SourceName, Imported.SourceMaterialIndex)
				: FGuid::NewGuid();
			Definition.Name = FName(Imported.Name);
			Definition.SourceName = Imported.SourceName;
			Definition.SourceMaterialIndex = Imported.SourceMaterialIndex;
			if (NewNameCounts[Imported.SourceName] > 1)
			{
				DURIN_WARN("Static mesh '{}' has ambiguous duplicate source material name '{}'; allocated a new slot identity.",
					GetObjectPath(), Imported.SourceName);
			}
		}
		std::unordered_set<FGuid> UsedSlotIds;
		for (FStaticMeshMaterialSlotDefinition& Definition : ReconciledSlots)
		{
			if (Definition.SlotId.IsValid() && UsedSlotIds.insert(Definition.SlotId).second) continue;
			do Definition.SlotId = FGuid::NewGuid();
			while (!UsedSlotIds.insert(Definition.SlotId).second);
			DURIN_WARN("Static mesh '{}' replaced an invalid or duplicate material slot identity.", GetObjectPath());
		}

		const bool bSlotMetadataChanged = MaterialSlotsVersion != StaticMeshMaterialSlotsVersion
			|| !SlotDefinitionsEqual(MaterialSlots, ReconciledSlots);

		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.reserve(ImportedScene.MaterialSlots.size());
		for (const FStaticMeshMaterialSlotDefinition& Slot : ReconciledSlots)
		{
			RenderData->MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex, Slot.SlotId});
		}

		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		std::unordered_map<std::string, uint32> SectionNameCounts;
		for (const Asset::FImportedMeshData& ImportedMesh : ImportedScene.Meshes)
		{
			if (!ValidateImportedMesh(ImportedMesh, OutError))
			{
				if (!OutError.empty()) return false;
				continue;
			}
			if (LOD.Positions.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Positions.size()
				|| LOD.Indices.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Indices.size())
			{
				OutError = std::format("Static mesh '{}' exceeds uint32 render-data limits.", FilePath);
				return false;
			}

			const uint32 BaseVertexIndex = static_cast<uint32>(LOD.Positions.size());
			const uint32 FirstIndex = static_cast<uint32>(LOD.Indices.size());
			LOD.Positions.insert(LOD.Positions.end(), ImportedMesh.Positions.begin(), ImportedMesh.Positions.end());

			std::vector<FVector3f> MeshNormals = HasValidNormals(ImportedMesh.Normals, ImportedMesh.Positions.size())
				? ImportedMesh.Normals
				: BuildNormals(ImportedMesh.Positions, ImportedMesh.Indices);
			for (FVector3f& Normal : MeshNormals) Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
			LOD.Normals.insert(LOD.Normals.end(), MeshNormals.begin(), MeshNormals.end());

			std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> MeshTexCoords;
			for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			{
				const auto& ImportedTexCoords = ImportedMesh.UVChannels[Channel];
				const bool bValidChannel = ImportedTexCoords.size() == ImportedMesh.Positions.size()
					&& std::ranges::all_of(ImportedTexCoords, [](const FVector2f& UV) { return IsFinite(UV); });
				if (bValidChannel)
				{
					MeshTexCoords[Channel] = ImportedTexCoords;
					LOD.NumTexCoords = static_cast<uint8>(std::max<uint32>(LOD.NumTexCoords, Channel + 1));
				}
				else
				{
					MeshTexCoords[Channel].assign(ImportedMesh.Positions.size(), FVector2f(0.0f));
				}
				LOD.TexCoords[Channel].insert(LOD.TexCoords[Channel].end(), MeshTexCoords[Channel].begin(), MeshTexCoords[Channel].end());
			}

			std::vector<FVector4f> MeshTangents;
			if (HasValidTangents(ImportedMesh.Tangents, ImportedMesh.Positions.size()))
			{
				MeshTangents.reserve(ImportedMesh.Tangents.size());
				for (size_t VertexIndex = 0; VertexIndex < ImportedMesh.Tangents.size(); ++VertexIndex)
				{
					const FVector3f& Normal = MeshNormals[VertexIndex];
					const FVector3f SourceTangent(ImportedMesh.Tangents[VertexIndex]);
					const FVector3f Tangent = SafeNormalize(SourceTangent - Normal * glm::dot(Normal, SourceTangent), MakeStableTangent(Normal));
					MeshTangents.emplace_back(Tangent, ImportedMesh.Tangents[VertexIndex].w < 0.0f ? -1.0f : 1.0f);
				}
			}
			else
			{
				MeshTangents = BuildTangents(ImportedMesh.Positions, MeshNormals, MeshTexCoords[0], ImportedMesh.Indices);
			}
			LOD.Tangents.insert(LOD.Tangents.end(), MeshTangents.begin(), MeshTangents.end());

			const bool bValidColors = ImportedMesh.Colors.size() == ImportedMesh.Positions.size()
				&& std::ranges::all_of(ImportedMesh.Colors, [](const FVector4f& Color) { return IsFinite(Color); });
			if (bValidColors)
			{
				LOD.Colors.insert(LOD.Colors.end(), ImportedMesh.Colors.begin(), ImportedMesh.Colors.end());
				LOD.bHasVertexColors = true;
			}
			else
			{
				LOD.Colors.insert(LOD.Colors.end(), ImportedMesh.Positions.size(), FVector4f(1.0f));
			}

			LOD.Indices.reserve(LOD.Indices.size() + ImportedMesh.Indices.size());
			for (uint32 Index : ImportedMesh.Indices)
			{
				LOD.Indices.push_back(BaseVertexIndex + Index);
			}

			FStaticMeshSection Section;
			Section.Name = MakeUniqueSectionName(ImportedMesh.Name, static_cast<uint32>(LOD.Sections.size()), SectionNameCounts);
			Section.FirstIndex = FirstIndex;
			Section.IndexCount = static_cast<uint32>(ImportedMesh.Indices.size());
			Section.MinVertexIndex = BaseVertexIndex + *std::ranges::min_element(ImportedMesh.Indices);
			Section.MaxVertexIndex = BaseVertexIndex + *std::ranges::max_element(ImportedMesh.Indices);
			if (!FindMaterialSlotIndex(*RenderData, ImportedMesh.MaterialIndex, Section.MaterialSlotIndex))
			{
				OutError = std::format("Static mesh section '{}' references missing source material index {}.",
					Section.Name, ImportedMesh.MaterialIndex);
				return false;
			}
			LOD.Sections.emplace_back(std::move(Section));
		}

		if (LOD.Positions.empty() || LOD.Indices.empty() || LOD.Sections.empty())
		{
			OutError = std::format("Static mesh source has no renderable geometry: {}", FilePath);
			return false;
		}

		RenderData->RecalculateBounds();
		const FVector3f BoundsMin(RenderData->LocalBounds.Min);
		const FVector3f BoundsMax(RenderData->LocalBounds.Max);

		const FVector3f BoundsCenter = (BoundsMin + BoundsMax) * 0.5f;
		const FVector3f BoundsExtent = BoundsMax - BoundsMin;
		const float MaxDimension = std::max(BoundsExtent.x, std::max(BoundsExtent.y, BoundsExtent.z));
		if (MaxDimension <= 0.0f)
		{
			OutError = std::format("Static mesh source has invalid bounds: {}", FilePath);
			return false;
		}

		const float Scale = NormalizedSize / MaxDimension;
		for (FVector3f& Position : LOD.Positions)
		{
			Position = (Position - BoundsCenter) * Scale;
		}
		RenderData->RecalculateBounds();

		MaterialSlots = std::move(ReconciledSlots);
		MaterialSlotsVersion = StaticMeshMaterialSlotsVersion;
		SetRenderData(std::move(RenderData));
		if (bSlotMetadataChanged) MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		if (SourceFile.empty())
		{
			OutError = "Static mesh asset has no source file.";
			return false;
		}
		const std::string PhysicalPath = ResolveStaticMeshSource(*this).generic_string();
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			OutError = std::format("Static mesh source file does not exist: {}", SourceFile);
			return false;
		}
		return BuildRenderData(PhysicalPath, OutError);
	}

	auto DStaticMesh::ImportAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& InImportSettings) -> FStaticMeshImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)) return {false, "Source file does not exist.", nullptr};
		std::string ImportSettingsError;
		if (!InImportSettings.IsValid(&ImportSettingsError)) return {false, std::move(ImportSettingsError), nullptr};

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::string Extension = Input.extension().generic_string();
		const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + Extension;
		const std::filesystem::path Destination = std::filesystem::path(ResolveMountedFile(ParsedAssetPath.ToString())).replace_extension(Extension);
		if (std::filesystem::exists(Destination)) return {false, std::format("Imported source already exists: {}", Destination.generic_string()), nullptr};

		DStaticMesh* Mesh = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Mesh);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};
		Mesh->ImportSettings = InImportSettings;
		std::string BuildError;
		if (!Mesh->BuildRenderData(Input.generic_string(), BuildError))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}

		std::error_code Ec;
		std::filesystem::create_directories(Destination.parent_path(), Ec);
		if (Ec || !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, Ec))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy source file to {}: {}", Destination.generic_string(), Ec.message()), nullptr};
		}
		Mesh->SourceFile = SourceFileName;
		Asset::FAssetResult SaveResult = Asset::SavePackage(Mesh->GetPackage());
		if (!SaveResult)
		{
			std::filesystem::remove(Destination, Ec);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		return {true, {}, Mesh};
	}

	auto PackStaticMeshVertex(
		const FVector3f& Normal,
		const FVector4f& Tangent,
		const std::array<FVector2f, MaxStaticMeshUVChannels>& TexCoords,
		const FVector4f& Color) -> FStaticMeshPackedVertex
	{
		auto PackSnorm = [](float Value) -> int16 {
			return static_cast<int16>(std::lround(std::clamp(Value, -1.0f, 1.0f) * 32767.0f));
		};
		auto PackUnorm = [](float Value) -> uint8 {
			return static_cast<uint8>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
		};

		FStaticMeshPackedVertex Result;
		Result.Normal = {PackSnorm(Normal.x), PackSnorm(Normal.y), PackSnorm(Normal.z), 0};
		Result.Tangent = {PackSnorm(Tangent.x), PackSnorm(Tangent.y), PackSnorm(Tangent.z), PackSnorm(Tangent.w)};
		Result.TexCoords = TexCoords;
		Result.Color = {PackUnorm(Color.r), PackUnorm(Color.g), PackUnorm(Color.b), PackUnorm(Color.a)};
		return Result;
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> void
	{
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			const size_t NumVertices = LOD.Positions.size();
			const bool bValidStreams = NumVertices > 0
				&& LOD.Normals.size() == NumVertices
				&& LOD.Tangents.size() == NumVertices
				&& LOD.Colors.size() == NumVertices
				&& std::ranges::all_of(LOD.TexCoords, [NumVertices](const auto& Channel) { return Channel.size() == NumVertices; });
			const bool bValidIndices = !LOD.Indices.empty() && LOD.Indices.size() % 3 == 0
				&& std::ranges::all_of(LOD.Indices, [NumVertices](uint32 Index) { return Index < NumVertices; });
			const bool bValidSections = !LOD.Sections.empty() && std::ranges::all_of(LOD.Sections, [this, &LOD](const FStaticMeshSection& Section) {
				return Section.IndexCount > 0
					&& Section.IndexCount % 3 == 0
					&& static_cast<uint64>(Section.FirstIndex) + Section.IndexCount <= LOD.Indices.size()
					&& Section.MinVertexIndex <= Section.MaxVertexIndex
					&& Section.MaxVertexIndex < LOD.Positions.size()
					&& Section.MaterialSlotIndex < MaterialSlots.size();
			});
			if (!bValidStreams || !bValidIndices || !bValidSections) continue;

			if (LOD.PositionVertexBufferRHI == nullptr)
			{
				FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex(
					"StaticMeshPositionVertexBuffer",
					static_cast<uint32>(LOD.Positions.size() * sizeof(FVector3f))
				);
				VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
				VertexBufferDesc.InitialData.Data = LOD.Positions.data();
				VertexBufferDesc.InitialData.Size = static_cast<uint32>(LOD.Positions.size() * sizeof(FVector3f));
				LOD.PositionVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, VertexBufferDesc);
			}

			if (LOD.StaticMeshVertexBufferRHI == nullptr)
			{
				std::vector<FStaticMeshPackedVertex> PackedVertices(NumVertices);
				for (size_t VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
				{
					std::array<FVector2f, MaxStaticMeshUVChannels> VertexTexCoords;
					for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel) VertexTexCoords[Channel] = LOD.TexCoords[Channel][VertexIndex];
					PackedVertices[VertexIndex] = PackStaticMeshVertex(LOD.Normals[VertexIndex], LOD.Tangents[VertexIndex], VertexTexCoords, LOD.Colors[VertexIndex]);
				}
				FRHIBufferCreateDesc AttributeBufferDesc = FRHIBufferCreateDesc::CreateVertex(
					"StaticMeshAttributeVertexBuffer", static_cast<uint32>(PackedVertices.size() * sizeof(FStaticMeshPackedVertex)));
				AttributeBufferDesc.Usage |= EBufferUsageFlags::Static;
				AttributeBufferDesc.InitialData.Data = PackedVertices.data();
				AttributeBufferDesc.InitialData.Size = static_cast<uint32>(PackedVertices.size() * sizeof(FStaticMeshPackedVertex));
				LOD.StaticMeshVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, AttributeBufferDesc);
			}

			if (LOD.IndexBufferRHI == nullptr)
			{
				FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
					"StaticMeshIndexBuffer",
					static_cast<uint32>(LOD.Indices.size() * sizeof(uint32)),
					sizeof(uint32)
				);
				IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
				IndexBufferDesc.InitialData.Data = LOD.Indices.data();
				IndexBufferDesc.InitialData.Size = static_cast<uint32>(LOD.Indices.size() * sizeof(uint32));
				LOD.IndexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, IndexBufferDesc);
			}
		}
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			LOD.PositionVertexBufferRHI = nullptr;
			LOD.StaticMeshVertexBufferRHI = nullptr;
			LOD.IndexBufferRHI = nullptr;
		}
	}

	auto FStaticMeshRenderData::IsReadyForRendering(uint32 LODIndex) const -> bool
	{
		if (LODIndex >= LODResources.size()) return false;
		const FStaticMeshLODResources& LOD = LODResources[LODIndex];
		return LOD.PositionVertexBufferRHI != nullptr
			&& LOD.StaticMeshVertexBufferRHI != nullptr
			&& LOD.IndexBufferRHI != nullptr
			&& !LOD.Indices.empty()
			&& !LOD.Sections.empty();
	}

	auto FStaticMeshRenderData::RecalculateBounds() -> void
	{
		LocalBounds.Reset();
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			LOD.LocalBounds.Reset();
			for (const FVector3f& Position : LOD.Positions) LOD.LocalBounds.AddPoint(FVector3(Position));
			for (FStaticMeshSection& Section : LOD.Sections)
			{
				Section.LocalBounds.Reset();
				const uint64 EndIndex = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
				if (EndIndex > LOD.Indices.size()) continue;
				for (uint32 IndexOffset = 0; IndexOffset < Section.IndexCount; ++IndexOffset)
				{
					const uint32 VertexIndex = LOD.Indices[Section.FirstIndex + IndexOffset];
					if (VertexIndex < LOD.Positions.size()) Section.LocalBounds.AddPoint(FVector3(LOD.Positions[VertexIndex]));
				}
			}
			for (const FVector3f& Position : LOD.Positions) LocalBounds.AddPoint(FVector3(Position));
		}
	}
}
