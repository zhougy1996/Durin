#include "StaticMesh/StaticMesh.h"

#include "Components/StaticMeshComponent.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

#include "RHICommandList.h"

namespace Durin
{
	namespace
	{
		FStaticMeshUpdateCounters GLastStaticMeshUpdateCounters;

		auto CheckStaticMeshUpdateThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		inline constexpr uint32 StaticMeshMaterialSlotsVersion = 1;
		inline constexpr uint32 StaticMeshAssimpImporterVersion = 1;
		inline constexpr std::string_view StaticMeshImporterId = "Assimp";
		inline constexpr std::string_view StaticMeshSourceRoot = "SourceAssets/Models";
		inline constexpr std::string_view LegacySlotGuidDomain = "Durin.StaticMeshMaterialSlot.v1";
		inline constexpr uint64 StaticMeshDerivedDataBudgetBytes = 8ull * 1024ull * 1024ull * 1024ull;
		inline constexpr uint32 StaticMeshDerivedDataCleanupDeleteLimit = 16;
		constexpr float VectorTolerance = 1.0e-10f;

		auto GetStaticMeshObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore(
				"StaticMesh/Objects", MaximumStaticMeshPayloadBytes);
		}

		auto IsCanonicalStaticMeshHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}

		auto MakeStaticMeshKey(
			std::string_view SourceHash,
			std::string_view ImporterId,
			uint32 ImporterVersion,
			const FStaticMeshImportSettings& ImportSettings,
			std::string& OutKey,
			std::string& OutError) -> bool
		{
			if (!IsCanonicalStaticMeshHash(SourceHash))
			{
				OutError = "Static mesh source content hash is missing or invalid.";
				return false;
			}
			if (ImporterId.empty())
			{
				OutError = "Static mesh importer identity is missing.";
				return false;
			}
			if (!ImportSettings.IsValid(&OutError)) return false;
			OutKey = BuildStaticMeshDerivedDataKey({
				.SourceContentHash = FXxHash128::FromString(SourceHash),
				.ImporterId = std::string(ImporterId),
				.ImporterVersion = ImporterVersion,
				.ImportSettings = ImportSettings,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64});
			return true;
		}

		auto RestoreStaticMeshRuntimeMetadata(
			const std::vector<FStaticMeshMaterialSlotDefinition>& MaterialSlots,
			FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				OutError = "Cached static-mesh material slot count does not match asset metadata.";
				return false;
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				const FStaticMeshMaterialSlotDefinition& Definition = MaterialSlots[SlotIndex];
				FStaticMeshMaterialSlot& Slot = RenderData.MaterialSlots[SlotIndex];
				// Editable asset metadata is authoritative for stable slot identity. The
				// cached payload contributes only the compatible slot ordering.
				Slot.SlotId = Definition.SlotId;
				Slot.Name = Definition.Name.ToString();
				Slot.SourceMaterialIndex = Definition.SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
			{
				auto& Sections = RenderData.LODResources[LODIndex].Sections;
				for (size_t SectionIndex = 0; SectionIndex < Sections.size(); ++SectionIndex)
					Sections[SectionIndex].Name = std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			}
			return true;
		}

		auto ValidateStaticMeshMaterialSlotMapping(
			const FStaticMeshPayloadData& Payload,
			const std::vector<FStaticMeshMaterialSlotDefinition>& MaterialSlots,
			std::string& OutError) -> bool
		{
			if (Payload.MaterialSlotIds.size() != MaterialSlots.size())
			{
				OutError = "Static-mesh payload material slot count does not match package metadata.";
				return false;
			}
			for (size_t Index = 0; Index < MaterialSlots.size(); ++Index)
			{
				if (Payload.MaterialSlotIds[Index] != MaterialSlots[Index].SlotId)
				{
					OutError = std::format(
						"Static-mesh payload material slot {} does not match package metadata.", Index);
					return false;
				}
			}
			return true;
		}

		auto LoadStaticMeshDerivedData(
			std::string_view Key,
			const std::vector<FStaticMeshMaterialSlotDefinition>& MaterialSlots,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			EStaticMeshDerivedDataStatus& OutStatus,
			std::string& OutMessage) -> bool
		{
			std::vector<uint8> Bytes;
			const Asset::FDerivedDataObjectReadResult Read = GetStaticMeshObjectStore().Read(Key, Bytes);
			if (!Read)
			{
				OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
					? EStaticMeshDerivedDataStatus::Missing
					: EStaticMeshDerivedDataStatus::Corrupt;
				OutMessage = Read.Message;
				return false;
			}

			FStaticMeshPayloadData Payload;
			std::string Error;
			if (!DecodeStaticMeshPayload(Bytes, EStaticMeshTargetPlatform::Win64, Payload, Error)
				|| !MakeStaticMeshRenderData(Payload, OutRenderData, Error)
				|| !RestoreStaticMeshRuntimeMetadata(MaterialSlots, *OutRenderData, Error))
			{
				OutStatus = Error.find("unsupported") != std::string::npos
					? EStaticMeshDerivedDataStatus::Incompatible
					: EStaticMeshDerivedDataStatus::Corrupt;
				OutMessage = std::move(Error);
				return false;
			}
			OutStatus = EStaticMeshDerivedDataStatus::Hit;
			OutMessage.clear();
			return true;
		}

		auto StoreStaticMeshDerivedData(
			std::string_view Key,
			const FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;
			std::vector<uint8> Bytes;
			if (!MakeStaticMeshPayloadData(RenderData, Payload, OutError)
				|| !EncodeStaticMeshPayload(Payload, EStaticMeshTargetPlatform::Win64, Bytes, OutError)
				|| !GetStaticMeshObjectStore().Write(Key, Bytes, &OutError)) return false;

			const Asset::FDerivedDataObjectCleanupResult Cleanup = GetStaticMeshObjectStore().CleanupToBudget(
				StaticMeshDerivedDataBudgetBytes, StaticMeshDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty())
			{
				DURIN_WARN("Static-mesh DDC cleanup: {}", Cleanup.Message);
			}
			return true;
		}

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

		auto FindOwningMount(std::string_view AssetPath) -> const PathUtilities::FMountPoint*
		{
			const auto& Mounts = PathUtilities::GetRegisteredMountPoints();
			const auto It = std::ranges::find_if(Mounts, [AssetPath](const PathUtilities::FMountPoint& Mount) {
				return AssetPath.starts_with(Mount.VirtualRoot);
			});
			return It != Mounts.end() ? &*It : nullptr;
		}

		auto GetMountOwnerRoot(const PathUtilities::FMountPoint& Mount) -> std::filesystem::path
		{
			std::filesystem::path ContentRoot = std::filesystem::path(Mount.PhysicalPath).lexically_normal();
			if (ContentRoot.filename().empty()) ContentRoot = ContentRoot.parent_path();
			std::string DirectoryName = ContentRoot.filename().generic_string();
			std::ranges::transform(DirectoryName, DirectoryName.begin(), [](char Value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
			});
			return DirectoryName == "content" ? ContentRoot.parent_path() : ContentRoot;
		}

		auto IsPortableStaticMeshSourcePath(std::string_view SourcePath, std::string* OutError = nullptr) -> bool
		{
			const std::filesystem::path Path(SourcePath);
			const std::filesystem::path Normalized = Path.lexically_normal();
			const bool bContainsParent = std::ranges::any_of(Path, [](const std::filesystem::path& Part) {
				return Part == "..";
			});
			const bool bValid = !SourcePath.empty()
				&& !Path.is_absolute()
				&& !SourcePath.starts_with('/')
				&& !bContainsParent
				&& SourcePath == Normalized.generic_string()
				&& Normalized.generic_string().starts_with(std::string(StaticMeshSourceRoot) + "/");
			if (!bValid && OutError)
			{
				*OutError = std::format(
					"Static mesh source path '{}' must be normalized beneath {}/.",
					SourcePath, StaticMeshSourceRoot);
			}
			return bValid;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Extension,
			std::filesystem::path& OutPhysicalPath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format("Static mesh asset {} is not beneath a registered content mount.", AssetPath.ToString());
				return false;
			}
			std::filesystem::path RelativeAssetPath(
				std::string(AssetPath.ToString().substr(Mount->VirtualRoot.size())));
			RelativeAssetPath.replace_extension(Extension);
			const std::filesystem::path StoredPath = std::filesystem::path(StaticMeshSourceRoot) / RelativeAssetPath;
			OutStoredPath = StoredPath.lexically_normal().generic_string();
			if (!IsPortableStaticMeshSourcePath(OutStoredPath, &OutError)) return false;
			OutPhysicalPath = (GetMountOwnerRoot(*Mount) / StoredPath).lexically_normal();
			return true;
		}

		auto ResolvePortableStaticMeshSource(
			const DStaticMesh& Mesh,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const FStaticMeshSourceImportData& Source = Mesh.GetSourceImportData();
			if (!IsPortableStaticMeshSourcePath(Source.SourcePath, &OutError)) return false;
			if (!Mesh.GetPackage())
			{
				OutError = "Static mesh source cannot be resolved without an owning package.";
				return false;
			}
			const PathUtilities::FMountPoint* Mount = FindOwningMount(Mesh.GetPackage()->GetPackagePath());
			if (!Mount)
			{
				OutError = std::format("Static mesh package {} is not beneath a registered content mount.",
					Mesh.GetPackage()->GetPackagePath());
				return false;
			}
			OutPath = (GetMountOwnerRoot(*Mount) / Source.SourcePath).lexically_normal();
			return true;
		}

		auto ResolveLegacyStaticMeshSource(const DStaticMesh& Mesh) -> std::filesystem::path
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

		auto HashStaticMeshSource(const std::filesystem::path& Path, std::string& OutHash, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutError = std::format("Failed to read static mesh source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes).ToString();
			return true;
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
			Asset::RegisterAssetMoveContributor(DStaticMesh::StaticClass(), [](
				DObject*, const FAssetPath&, const FAssetPath&, Asset::FAssetMoveContribution&) -> Asset::FAssetResult {
				// Source provenance is independent of package placement.
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DStaticMesh::StaticClass(), [](
				DObject*, Asset::FAssetDeleteContribution&) -> Asset::FAssetResult {
				// Portable SourceAssets may be shared and require a separate, explicit source operation.
				return {};
			});
			return true;
		}();
		(void)RegisteredMoveContributor;
	}

	DStaticMesh::~DStaticMesh() = default;

	auto GetLastStaticMeshUpdateCounters() -> FStaticMeshUpdateCounters
	{
		CheckStaticMeshUpdateThread();
		return GLastStaticMeshUpdateCounters;
	}

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
		NotifyLoadedComponents();
	}

	auto DStaticMesh::NotifyLoadedComponents() -> void
	{
		CheckStaticMeshUpdateThread();
		FStaticMeshUpdateCounters Counters;
		std::vector<FObjectHandle> ComponentHandles;
		const std::vector<DObject*> Objects = GDObjectArray.Snapshot();
		Counters.ObjectSnapshotCount = 1;
		Counters.ScannedObjectCount = static_cast<uint64>(Objects.size());
		for (DObject* Object : Objects)
		{
			auto* Component = Cast<DStaticMeshComponent>(Object);
			if (!IsValid(Component)) continue;
			++Counters.ScannedComponentCount;
			if (Component->GetStaticMesh() != this) continue;
			const FObjectHandle Handle = MakeObjectHandle(Component);
			if (IsObjectHandleNull(Handle)) continue;
			ComponentHandles.push_back(Handle);
			++Counters.MatchedComponentCount;
		}
		std::ranges::sort(ComponentHandles, [](FObjectHandle Left, FObjectHandle Right) {
			return Left.Index < Right.Index
				|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
		});
		for (FObjectHandle Handle : ComponentHandles)
		{
			auto* Component = Cast<DStaticMeshComponent>(ResolveObjectHandle(Handle));
			if (!IsValid(Component) || Component->GetStaticMesh() != this) continue;
			Component->HandleStaticMeshRenderDataChanged(this);
			++Counters.UpdatedComponentCount;
		}
		GLastStaticMeshUpdateCounters = Counters;
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
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		std::vector<FStaticMeshMaterialSlotDefinition> CandidateMaterialSlots;
		bool bSlotMetadataChanged = false;
		if (!BuildRenderDataCandidate(
			FilePath, CandidateRenderData, CandidateMaterialSlots, bSlotMetadataChanged, OutError)) return false;
		PublishRenderData(
			std::move(CandidateRenderData), std::move(CandidateMaterialSlots), bSlotMetadataChanged);
		return true;
	}

	auto DStaticMesh::PublishRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FStaticMeshMaterialSlotDefinition> InMaterialSlots,
		bool bSlotMetadataChanged) -> void
	{
		MaterialSlots = std::move(InMaterialSlots);
		MaterialSlotsVersion = StaticMeshMaterialSlotsVersion;
		SetRenderData(std::move(InRenderData));
		if (bSlotMetadataChanged) MarkPackageDirty();
	}

	auto DStaticMesh::BuildRenderDataCandidate(
		std::string_view FilePath,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
		bool& bOutSlotMetadataChanged,
		std::string& OutError) -> bool
	{
		Asset::FImportedSceneData ImportedScene;
		const FStaticMeshImportSettings& EffectiveImportSettings = GetImportSettings();
		std::string ImportSettingsError;
		if (!EffectiveImportSettings.IsValid(&ImportSettingsError))
		{
			OutError = std::move(ImportSettingsError);
			return false;
		}
		if (!Asset::ImportFromFile(FilePath, ImportedScene, MakeImportOptions(EffectiveImportSettings)))
		{
			OutError = std::format("Failed to import static mesh source file: {}", FilePath);
			return false;
		}

		const bool bVersionZeroMigration = MaterialSlotsVersion == 0 && !GetSourceFile().empty();
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

		OutRenderData = std::move(RenderData);
		OutMaterialSlots = std::move(ReconciledSlots);
		bOutSlotMetadataChanged = bSlotMetadataChanged;
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		DerivedDataDiagnostic = {};
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedRenderData(OutError);

		const FStaticMeshSourceDiagnostic Diagnostic = InspectSource();
		if (Diagnostic.Status == EStaticMeshSourceStatus::NoSource)
		{
			OutError.clear();
			return true;
		}

		const bool bLegacySource = Diagnostic.Status == EStaticMeshSourceStatus::LegacyAvailable
			|| Diagnostic.Status == EStaticMeshSourceStatus::LegacyMissing;
		const FStaticMeshImportSettings& EffectiveSettings = GetImportSettings();
		const std::string_view ImporterId = bLegacySource
			? StaticMeshImporterId
			: std::string_view(SourceImportData.ImporterId);
		const uint32 ImporterVersion = bLegacySource
			? StaticMeshAssimpImporterVersion
			: SourceImportData.ImporterVersion;

		std::string CurrentSourceHash;
		if (Diagnostic.IsAvailable())
		{
			if (!HashStaticMeshSource(Diagnostic.ResolvedPath, CurrentSourceHash, OutError))
			{
				DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
		}
		else if (!bLegacySource && IsCanonicalStaticMeshHash(SourceImportData.SourceContentHash))
		{
			CurrentSourceHash = SourceImportData.SourceContentHash;
		}
		else
		{
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = Diagnostic.Message;
			OutError = Diagnostic.Message;
			return false;
		}

		if (!MakeStaticMeshKey(
			CurrentSourceHash, ImporterId, ImporterVersion, EffectiveSettings,
			DerivedDataDiagnostic.Key, OutError))
		{
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::Incompatible;
			DerivedDataDiagnostic.Message = OutError;
			return false;
		}

		std::unique_ptr<FStaticMeshRenderData> CachedRenderData;
		EStaticMeshDerivedDataStatus CacheStatus = EStaticMeshDerivedDataStatus::Missing;
		std::string CacheMessage;
		const bool bSourceMetadataStale = Diagnostic.IsAvailable()
			&& !bLegacySource
			&& SourceImportData.SourceContentHash != CurrentSourceHash;
		if (!bSourceMetadataStale && LoadStaticMeshDerivedData(
			DerivedDataDiagnostic.Key, MaterialSlots, CachedRenderData, CacheStatus, CacheMessage))
		{
			SetRenderData(std::move(CachedRenderData));
			DerivedDataDiagnostic.Status = Diagnostic.IsAvailable()
				? EStaticMeshDerivedDataStatus::Hit
				: EStaticMeshDerivedDataStatus::SourceUnavailableCached;
			DerivedDataDiagnostic.Message = Diagnostic.IsAvailable()
				? std::format("Static-mesh DDC hit for key {}.", DerivedDataDiagnostic.Key)
				: std::format(
					"Static-mesh source is unavailable, but cached key {} loaded successfully. Reimport and cache regeneration are unavailable.",
					DerivedDataDiagnostic.Key);
			if (!Diagnostic.IsAvailable()) DURIN_WARN("{}: {}", GetObjectPath(), DerivedDataDiagnostic.Message);
			if (!bLegacySource && SourceImportData.SourceContentHash != CurrentSourceHash)
			{
				SourceImportData.SourceContentHash = CurrentSourceHash;
				MarkPackageDirty();
			}
			OutError.clear();
			return true;
		}
		if (bSourceMetadataStale)
		{
			CacheStatus = EStaticMeshDerivedDataStatus::Missing;
			CacheMessage = "Source content changed; importer metadata reconciliation is required.";
		}

		DerivedDataDiagnostic.Status = CacheStatus;
		DerivedDataDiagnostic.Message = std::format(
			"Static-mesh DDC miss for key {}: {}", DerivedDataDiagnostic.Key, CacheMessage);
		if (!Diagnostic.IsAvailable())
		{
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = std::format(
				"{}. Cached payload was unavailable: {}", Diagnostic.Message, CacheMessage);
			OutError = DerivedDataDiagnostic.Message;
			return false;
		}

		DURIN_WARN("{} Rebuilding static mesh '{}' from source.", DerivedDataDiagnostic.Message, GetObjectPath());
		DerivedDataDiagnostic.bSourceImporterInvoked = true;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		std::vector<FStaticMeshMaterialSlotDefinition> CandidateMaterialSlots;
		bool bSlotMetadataChanged = false;
		if (!BuildRenderDataCandidate(
			Diagnostic.ResolvedPath,
			CandidateRenderData,
			CandidateMaterialSlots,
			bSlotMetadataChanged,
			OutError))
		{
			DerivedDataDiagnostic.Message = std::format(
				"Static-mesh rebuild failed for key {}: {}", DerivedDataDiagnostic.Key, OutError);
			return false;
		}
		if (!StoreStaticMeshDerivedData(DerivedDataDiagnostic.Key, *CandidateRenderData, OutError))
		{
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::WriteFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Static-mesh DDC write failed for key {}: {}", DerivedDataDiagnostic.Key, OutError);
			OutError = DerivedDataDiagnostic.Message;
			return false;
		}

		PublishRenderData(
			std::move(CandidateRenderData), std::move(CandidateMaterialSlots), bSlotMetadataChanged);
		DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::Rebuilt;
		DerivedDataDiagnostic.Message = std::format(
			"Rebuilt static mesh and stored DDC key {} after cache miss: {}",
			DerivedDataDiagnostic.Key, CacheMessage);
		if (!bLegacySource && SourceImportData.SourceContentHash != CurrentSourceHash)
		{
			SourceImportData.SourceContentHash = std::move(CurrentSourceHash);
			MarkPackageDirty();
		}
		if (bLegacySource)
		{
			DURIN_WARN("Static mesh '{}' uses legacy source metadata '{}'; repair or reimport it to migrate into SourceAssets.",
				GetObjectPath(), SourceFile);
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::LoadCookedRenderData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked static mesh '{}': {}", GetObjectPath(), Message);
			OutError = DerivedDataDiagnostic.Message;
			return false;
		};

		if (CookedPayload.PayloadId != StaticMeshPrimaryCookedPayloadId
			|| CookedPayload.LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != StaticMeshPayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod != static_cast<uint32>(Asset::ECookedPayloadCompression::None))
		{
			return FailCooked("required DMSH descriptor is missing or incompatible.");
		}

		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
		{
			return FailCooked(OutError.empty() ? "package companion path could not be resolved." : OutError);
		}

		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			Container,
			&OutError))
		{
			return FailCooked(OutError);
		}
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);

		FStaticMeshPayloadData Payload;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		if (!DecodeStaticMeshPayload(
			Bytes, EStaticMeshTargetPlatform::Win64, Payload, OutError)
			|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError)
			|| !MakeStaticMeshRenderData(Payload, CandidateRenderData, OutError)
			|| !RestoreStaticMeshRuntimeMetadata(MaterialSlots, *CandidateRenderData, OutError))
		{
			return FailCooked(OutError);
		}

		SetRenderData(std::move(CandidateRenderData));
		DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedLoaded;
		DerivedDataDiagnostic.Message = std::format(
			"Loaded cooked static-mesh payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DStaticMesh::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Static mesh '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!RenderData && !PostLoad(OutError)) return false;
		if (!RenderData)
		{
			OutError = std::format("Static mesh '{}' has no render data to cook.", GetObjectPath());
			return false;
		}

		FStaticMeshPayloadData Payload;
		std::vector<uint8> PayloadBytes;
		if (!MakeStaticMeshPayloadData(*RenderData, Payload, OutError)
			|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError)
			|| !EncodeStaticMeshPayload(
				Payload, EStaticMeshTargetPlatform::Win64, PayloadBytes, OutError))
		{
			OutError = std::format("Failed to cook static mesh '{}': {}", GetObjectPath(), OutError);
			return false;
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = StaticMeshPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = StaticMeshPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = StaticMeshPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};

		return Context.AddPackage(
			std::string(VirtualPackagePath),
			{std::move(BulkPayload)},
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != StaticMeshPrimaryCookedPayloadId)
				{
					if (Error) *Error = "Static-mesh cook did not produce its required descriptor.";
					return false;
				}

				const std::string SavedSourceFile = SourceFile;
				const FStaticMeshSourceImportData SavedSourceImportData = SourceImportData;
				const FStaticMeshImportSettings SavedImportSettings = ImportSettings;
				const std::vector<FStaticMeshMaterialSlotDefinition> SavedMaterialSlots = MaterialSlots;
				const Asset::FCookedPayloadDescriptor SavedCookedPayload = CookedPayload;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceFile.clear();
					SourceImportData = {};
					ImportSettings = {};
					for (FStaticMeshMaterialSlotDefinition& Slot : MaterialSlots)
					{
						Slot.SourceName.clear();
						Slot.SourceMaterialIndex = 0;
					}
				}

				Asset::FAssetPackageSerializationOptions SerializationOptions;
				if (!bRetainDiagnosticSourceMetadata)
				{
					SerializationOptions.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						if (Object != this) return true;
						const FName Name = Property->NamePrivate;
						return Name != FName("SourceFile")
							&& Name != FName("SourceImportData")
							&& Name != FName("ImportSettings");
					};
				}
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, SerializationOptions);
				SourceFile = SavedSourceFile;
				SourceImportData = SavedSourceImportData;
				ImportSettings = SavedImportSettings;
				MaterialSlots = SavedMaterialSlots;
				CookedPayload = SavedCookedPayload;
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			},
			&OutError);
	}

	auto DStaticMesh::InspectSource() const -> FStaticMeshSourceDiagnostic
	{
		if (SourceImportData.HasSource())
		{
			std::filesystem::path PhysicalPath;
			std::string Error;
			if (!ResolvePortableStaticMeshSource(*this, PhysicalPath, Error))
				return {EStaticMeshSourceStatus::Invalid, {}, std::move(Error)};
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				return {
					EStaticMeshSourceStatus::Missing,
					PhysicalPath.generic_string(),
					std::format("Static mesh source is missing: {}. Use source-path repair to select its replacement.",
						SourceImportData.SourcePath)};
			}
			return {EStaticMeshSourceStatus::Available, PhysicalPath.generic_string(), {}};
		}
		if (SourceFile.empty()) return {};
		const std::filesystem::path PhysicalPath = ResolveLegacyStaticMeshSource(*this);
		const bool bExists = std::filesystem::is_regular_file(PhysicalPath);
		return {
			bExists ? EStaticMeshSourceStatus::LegacyAvailable : EStaticMeshSourceStatus::LegacyMissing,
			PhysicalPath.generic_string(),
			bExists
				? "Static mesh uses legacy colocated source metadata and should be repaired or reimported."
				: std::format("Legacy static mesh source is missing: {}. Use source-path repair to select its replacement.",
					SourceFile)};
	}

	auto DStaticMesh::RepairSourcePath(std::string_view FilePath, std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged static meshes can retain source provenance.";
			return false;
		}
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
		{
			OutError = std::format("Static mesh replacement source does not exist: {}", Input.generic_string());
			return false;
		}
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(GetPackage()->GetPackagePath(), AssetPath, &OutError)) return false;
		std::filesystem::path Destination;
		std::string StoredPath;
		if (!MakeCanonicalSourceLocation(AssetPath, Input.extension().generic_string(), Destination, StoredPath, OutError))
			return false;
		std::string SourceHash;
		if (!HashStaticMeshSource(Input, SourceHash, OutError)) return false;

		std::error_code Error;
		std::filesystem::create_directories(Destination.parent_path(), Error);
		if (Error)
		{
			OutError = std::format("Failed to create static mesh source directory {}: {}",
				Destination.parent_path().generic_string(), Error.message());
			return false;
		}
		if (Input != Destination
			&& !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::overwrite_existing, Error))
		{
			OutError = std::format("Failed to copy replacement source to {}: {}",
				Destination.generic_string(), Error.message());
			return false;
		}

		const FStaticMeshSourceImportData PreviousSource = SourceImportData;
		const std::string PreviousLegacySource = SourceFile;
		const FStaticMeshImportSettings PreviousLegacySettings = ImportSettings;
		const FStaticMeshImportSettings EffectiveSettings = GetImportSettings();
		SourceImportData = {
			.SourcePath = std::move(StoredPath),
			.SourceContentHash = std::move(SourceHash),
			.ImporterId = std::string(StaticMeshImporterId),
			.ImporterVersion = StaticMeshAssimpImporterVersion,
			.ImportSettings = EffectiveSettings};
		SourceFile.clear();
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		std::vector<FStaticMeshMaterialSlotDefinition> CandidateMaterialSlots;
		bool bSlotMetadataChanged = false;
		std::string DerivedDataKey;
		const bool bBuilt = BuildRenderDataCandidate(
			Destination.generic_string(),
			CandidateRenderData,
			CandidateMaterialSlots,
			bSlotMetadataChanged,
			OutError);
		const bool bKeyBuilt = bBuilt && MakeStaticMeshKey(
			SourceImportData.SourceContentHash,
			SourceImportData.ImporterId,
			SourceImportData.ImporterVersion,
			SourceImportData.ImportSettings,
			DerivedDataKey,
			OutError);
		const bool bCached = bKeyBuilt && StoreStaticMeshDerivedData(
			DerivedDataKey, *CandidateRenderData, OutError);
		if (!bCached)
		{
			SourceImportData = PreviousSource;
			SourceFile = PreviousLegacySource;
			ImportSettings = PreviousLegacySettings;
			return false;
		}
		PublishRenderData(
			std::move(CandidateRenderData), std::move(CandidateMaterialSlots), bSlotMetadataChanged);
		DerivedDataDiagnostic = {
			.Status = EStaticMeshDerivedDataStatus::Rebuilt,
			.Key = std::move(DerivedDataKey),
			.Message = "Repaired source, rebuilt static mesh, and populated the DDC.",
			.bSourceImporterInvoked = true};
		MarkPackageDirty();
		return true;
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
		std::filesystem::path Destination;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(ParsedAssetPath, Extension, Destination, StoredSourcePath, PathError))
			return {false, std::move(PathError), nullptr};
		std::string SourceHash;
		if (!HashStaticMeshSource(Input, SourceHash, PathError)) return {false, std::move(PathError), nullptr};
		const bool bSourceAlreadyExists = std::filesystem::is_regular_file(Destination);
		if (bSourceAlreadyExists)
		{
			std::string ExistingHash;
			if (!HashStaticMeshSource(Destination, ExistingHash, PathError))
				return {false, std::move(PathError), nullptr};
			if (ExistingHash != SourceHash)
			{
				return {
					false,
					std::format(
						"A different source model already exists at {}. Repair or remove that source explicitly.",
						Destination.generic_string()),
					nullptr};
			}
		}

		DStaticMesh* Mesh = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Mesh);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};
		Mesh->SourceImportData = {
			.SourcePath = StoredSourcePath,
			.SourceContentHash = std::move(SourceHash),
			.ImporterId = std::string(StaticMeshImporterId),
			.ImporterVersion = StaticMeshAssimpImporterVersion,
			.ImportSettings = InImportSettings};
		std::string BuildError;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		std::vector<FStaticMeshMaterialSlotDefinition> CandidateMaterialSlots;
		bool bSlotMetadataChanged = false;
		std::string DerivedDataKey;
		if (!Mesh->BuildRenderDataCandidate(
			Input.generic_string(),
			CandidateRenderData,
			CandidateMaterialSlots,
			bSlotMetadataChanged,
			BuildError)
			|| !MakeStaticMeshKey(
				Mesh->SourceImportData.SourceContentHash,
				Mesh->SourceImportData.ImporterId,
				Mesh->SourceImportData.ImporterVersion,
				Mesh->SourceImportData.ImportSettings,
				DerivedDataKey,
				BuildError)
			|| !StoreStaticMeshDerivedData(DerivedDataKey, *CandidateRenderData, BuildError))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}
		Mesh->PublishRenderData(
			std::move(CandidateRenderData), std::move(CandidateMaterialSlots), bSlotMetadataChanged);
		Mesh->DerivedDataDiagnostic = {
			.Status = EStaticMeshDerivedDataStatus::Rebuilt,
			.Key = std::move(DerivedDataKey),
			.Message = "Imported static mesh and populated the DDC.",
			.bSourceImporterInvoked = true};

		std::error_code Ec;
		std::filesystem::create_directories(Destination.parent_path(), Ec);
		if (Ec || (!bSourceAlreadyExists
			&& !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, Ec)))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy source file to {}: {}", Destination.generic_string(), Ec.message()), nullptr};
		}
		Asset::FAssetResult SaveResult = Asset::SavePackage(Mesh->GetPackage());
		if (!SaveResult)
		{
			if (!bSourceAlreadyExists) std::filesystem::remove(Destination, Ec);
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
