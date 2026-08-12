#include "StaticMesh/StaticMeshBuildOperations.h"

#include "DerivedDataObjectStore.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuildDerivedData.h"
#include "StaticMesh/StaticMeshDerivedData.h"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace Durin::AssetBuild
{
	namespace
	{
		inline constexpr uint64 StaticMeshDerivedDataBudgetBytes =
			8ull * 1024ull * 1024ull * 1024ull;
		inline constexpr uint32 StaticMeshDerivedDataCleanupDeleteLimit = 16;
		constexpr float VectorTolerance = 1.0e-10f;

		auto IsCanonicalHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32
				&& std::ranges::all_of(Hash, [](char Character) {
					return Character >= '0' && Character <= '9'
						|| Character >= 'a' && Character <= 'f';
				});
		}

		auto GetObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore(
				"StaticMesh/Objects", MaximumStaticMeshPayloadBytes);
		}

		auto GetCollisionObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore(
				"StaticMeshCollision/Objects",
				MaximumStaticMeshCollisionPayloadBytes);
		}

		auto BuildCollisionGeometryHash(
			std::span<const FVector3f> Positions,
			std::span<const uint32> Indices) -> FXxHash128
		{
			std::vector<uint8> Bytes;
			Bytes.reserve(16 + Positions.size() * 12 + Indices.size() * 4);
			auto AppendU32 = [&](uint32 Value) {
				for (uint32 Byte = 0; Byte < 4; ++Byte)
					Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
			};
			auto AppendU64 = [&](uint64 Value) {
				for (uint32 Byte = 0; Byte < 8; ++Byte)
					Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
			};
			AppendU64(Positions.size());
			for (const FVector3f& Position : Positions)
				for (uint32 Axis = 0; Axis < 3; ++Axis)
					AppendU32(std::bit_cast<uint32>(Position[Axis]));
			AppendU64(Indices.size());
			for (uint32 Index : Indices) AppendU32(Index);
			return FXxHash128::HashBuffer(Bytes);
		}

		auto StoreProduct(
			std::string_view Key,
			const FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;
			if (!MakeStaticMeshPayloadData(RenderData, Payload, OutError)) return false;
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			if (!GetObjectStore().Write(Key, Bytes, &OutError)) return false;
			const Asset::FDerivedDataObjectCleanupResult Cleanup =
				GetObjectStore().CleanupToBudget(
					StaticMeshDerivedDataBudgetBytes,
					StaticMeshDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty())
				DURIN_WARN("Static-mesh DDC cleanup: {}", Cleanup.Message);
			return true;
		}

		auto RestoreRuntimeMetadata(
			std::span<const FStaticMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				OutError = "Cached StaticMesh material slot count does not match asset metadata.";
				return false;
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				RenderData.MaterialSlots[SlotIndex].Name =
					MaterialSlots[SlotIndex].Name.ToString();
				RenderData.MaterialSlots[SlotIndex].SourceMaterialIndex =
					MaterialSlots[SlotIndex].SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
				for (size_t SectionIndex = 0;
					SectionIndex < RenderData.LODResources[LODIndex].Sections.size();
					++SectionIndex)
					RenderData.LODResources[LODIndex].Sections[SectionIndex].Name =
						std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			OutError.clear();
			return true;
		}
		auto SlotDefinitionsEqual(
			std::span<const FStaticMeshMaterialSlotDefinition> A,
			std::span<const FStaticMeshMaterialSlotDefinition> B) -> bool
		{
			if (A.size() != B.size()) return false;
			for (size_t Index = 0; Index < A.size(); ++Index)
			{
				if (A[Index].Name != B[Index].Name
					|| A[Index].SourceName != B[Index].SourceName
					|| A[Index].SourceMaterialIndex != B[Index].SourceMaterialIndex
					|| A[Index].DefaultMaterial != B[Index].DefaultMaterial) return false;
			}
			return true;
		}

		auto SafeNormalize(const FVector3f& Value, const FVector3f& Fallback) -> FVector3f
		{
			return Math::NormalizeOr(Value, Fallback, VectorTolerance);
		}

		auto MakeStableTangent(const FVector3f& Normal) -> FVector3f
		{
			const FVector3f Axis = std::abs(Normal.z) < 0.999f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(0.0f, 1.0f, 0.0f);
			return SafeNormalize(Math::Cross(Axis, Normal), FVector3f(1.0f, 0.0f, 0.0f));
		}

		auto BuildNormals(const std::vector<FVector3f>& Positions, const std::vector<uint32>& Indices) -> std::vector<FVector3f>
		{
			std::vector<FVector3f> Normals(Positions.size(), FVector3f(0.0f));
			for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
			{
				const uint32 I0 = Indices[Index];
				const uint32 I1 = Indices[Index + 1];
				const uint32 I2 = Indices[Index + 2];
				const FVector3f FaceNormal = Math::Cross(Positions[I1] - Positions[I0], Positions[I2] - Positions[I0]);
				if (!Math::IsFinite(FaceNormal) || Math::LengthSquared(FaceNormal) <= VectorTolerance) continue;
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
					if (!Math::IsFinite(Tangent) || !Math::IsFinite(Bitangent)) continue;
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
				const FVector3f Orthogonalized = TangentAccum[VertexIndex] - Normal * Math::Dot(Normal, TangentAccum[VertexIndex]);
				const FVector3f Tangent = SafeNormalize(Orthogonalized, MakeStableTangent(Normal));
				const float Sign = Math::Dot(Math::Cross(Normal, Tangent), BitangentAccum[VertexIndex]) < 0.0f ? -1.0f : 1.0f;
				Tangents[VertexIndex] = FVector4f(Tangent, Sign);
			}
			return Tangents;
		}

		auto HasValidNormals(const std::vector<FVector3f>& Normals, size_t NumVertices) -> bool
		{
			return Normals.size() == NumVertices && std::ranges::all_of(Normals, [](const FVector3f& Normal) {
				return Math::IsFinite(Normal) && Math::LengthSquared(Normal) > VectorTolerance;
			});
		}

		auto HasValidTangents(const std::vector<FVector4f>& Tangents, size_t NumVertices) -> bool
		{
			return Tangents.size() == NumVertices && std::ranges::all_of(Tangents, [](const FVector4f& Tangent) {
				const FVector3f Direction(Tangent);
				return Math::IsFinite(Tangent) && Math::LengthSquared(Direction) > VectorTolerance && std::abs(Tangent.w) > 0.5f;
			});
		}

		auto ValidateImportedMesh(const FStaticMeshImportedMesh& Mesh, std::string& OutError) -> bool
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
			if (!std::ranges::all_of(Mesh.Positions, [](const FVector3f& Position) { return Math::IsFinite(Position); }))
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

		auto MakeUniqueSectionName(std::string Name, uint32 Index, std::unordered_map<std::string, uint32>& NameCounts) -> std::string
		{
			if (Name.empty()) Name = std::format("Section_{}", Index);
			uint32& Count = NameCounts[Name];
			const std::string Result = Count == 0 ? Name : std::format("{}_{}", Name, Count);
			++Count;
			return Result;
		}

		auto BuildRenderDataCandidate(
		std::span<const FStaticMeshMaterialSlotDefinition> PreviousMaterialSlots,
		float NormalizedSize,
		std::string_view OwnerPath,
		const FStaticMeshImportedData& ImportedData,
		std::string_view SourceLabel,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
		bool& bOutSlotMetadataChanged,
		std::string& OutError) -> bool
	{
		const std::vector<FStaticMeshMaterialSlotDefinition> PreviousSlots(
			PreviousMaterialSlots.begin(), PreviousMaterialSlots.end());
		std::vector<FStaticMeshMaterialSlotDefinition> ReconciledSlots = PreviousSlots;
		std::vector<bool> OldConsumed(PreviousSlots.size(), false);
		std::vector<bool> NewMatched(ImportedData.MaterialSlots.size(), false);
		std::vector<uint32> ImportedToStableSlot(
			ImportedData.MaterialSlots.size(), std::numeric_limits<uint32>::max());
		std::unordered_map<std::string, uint32> OldNameCounts;
		std::unordered_map<std::string, uint32> NewNameCounts;
		std::unordered_map<uint32, uint32> OldSourceIndexCounts;
		std::unordered_map<uint32, uint32> NewSourceIndexCounts;
		for (const FStaticMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldNameCounts[Slot.SourceName];
		for (const FStaticMeshImportedMaterialSlot& Slot : ImportedData.MaterialSlots) ++NewNameCounts[Slot.SourceName];
		for (const FStaticMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldSourceIndexCounts[Slot.SourceMaterialIndex];
		for (const FStaticMeshImportedMaterialSlot& Slot : ImportedData.MaterialSlots) ++NewSourceIndexCounts[Slot.SourceMaterialIndex];

		auto PreserveSlot = [&](size_t ImportedIndex, size_t OldIndex) {
			const FStaticMeshImportedMaterialSlot& Imported = ImportedData.MaterialSlots[ImportedIndex];
			ReconciledSlots[OldIndex].SourceName = Imported.SourceName;
			ReconciledSlots[OldIndex].SourceMaterialIndex = Imported.SourceMaterialIndex;
			OldConsumed[OldIndex] = true;
			NewMatched[ImportedIndex] = true;
			ImportedToStableSlot[ImportedIndex] = static_cast<uint32>(OldIndex);
		};

		for (size_t NewIndex = 0; NewIndex < ImportedData.MaterialSlots.size(); ++NewIndex)
		{
			const std::string& SourceName = ImportedData.MaterialSlots[NewIndex].SourceName;
			if (SourceName.empty()) continue;
			if (OldNameCounts[SourceName] != 1 || NewNameCounts[SourceName] != 1) continue;
			const auto It = std::ranges::find(PreviousSlots, SourceName, &FStaticMeshMaterialSlotDefinition::SourceName);
			if (It != PreviousSlots.end()) PreserveSlot(NewIndex, static_cast<size_t>(It - PreviousSlots.begin()));
		}

		for (size_t NewIndex = 0; NewIndex < ImportedData.MaterialSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedData.MaterialSlots[NewIndex];
			if (OldSourceIndexCounts[Imported.SourceMaterialIndex] != 1
				|| NewSourceIndexCounts[Imported.SourceMaterialIndex] != 1) continue;
			for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
			{
				const FStaticMeshMaterialSlotDefinition& Previous = PreviousSlots[OldIndex];
				if (OldConsumed[OldIndex]
					|| Previous.SourceMaterialIndex != Imported.SourceMaterialIndex) continue;
				PreserveSlot(NewIndex, OldIndex);
				break;
			}
		}

		auto MakeUniqueSlotName = [&](const FStaticMeshImportedMaterialSlot& Imported) {
			std::string BaseName = Imported.Name.empty() ? Imported.SourceName : Imported.Name;
			if (BaseName.empty() || FName(BaseName).IsNone()) BaseName = "Material";
			FName Candidate(BaseName);
			uint32 Suffix = 1;
			while (std::ranges::find(ReconciledSlots, Candidate, &FStaticMeshMaterialSlotDefinition::Name)
				!= ReconciledSlots.end())
			{
				Candidate = FName(std::format("{}_{}", BaseName, Suffix++));
			}
			return Candidate;
		};

		for (size_t NewIndex = 0; NewIndex < ImportedData.MaterialSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedData.MaterialSlots[NewIndex];
			FStaticMeshMaterialSlotDefinition& Definition = ReconciledSlots.emplace_back();
			Definition.Name = MakeUniqueSlotName(Imported);
			Definition.SourceName = Imported.SourceName;
			Definition.SourceMaterialIndex = Imported.SourceMaterialIndex;
			ImportedToStableSlot[NewIndex] = static_cast<uint32>(ReconciledSlots.size() - 1);
			if (NewNameCounts[Imported.SourceName] > 1)
			{
				DURIN_WARN("Static mesh '{}' has ambiguous duplicate source material name '{}'; appended a stable slot.",
					OwnerPath, Imported.SourceName);
			}
		}

		const bool bSlotMetadataChanged =
			!SlotDefinitionsEqual(PreviousMaterialSlots, ReconciledSlots);

		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.reserve(ReconciledSlots.size());
		for (const FStaticMeshMaterialSlotDefinition& Slot : ReconciledSlots)
		{
			RenderData->MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex});
		}
		std::unordered_map<uint32, uint32> ImportedSourceToIndex;
		for (uint32 ImportedIndex = 0; ImportedIndex < ImportedData.MaterialSlots.size(); ++ImportedIndex)
		{
			const uint32 SourceIndex = ImportedData.MaterialSlots[ImportedIndex].SourceMaterialIndex;
			if (!ImportedSourceToIndex.emplace(SourceIndex, ImportedIndex).second)
			{
				OutError = std::format(
					"Static mesh '{}' has duplicate imported source material index {}.", SourceLabel, SourceIndex);
				return false;
			}
		}

		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
		auto& Positions =
			LOD.VertexBuffers.PositionVertexBuffer.GetMutablePositions();
		auto& Normals =
			LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer
				.GetMutableNormals();
		auto& Tangents =
			LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer
				.GetMutableTangents();
		auto& TexCoords =
			LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer
				.GetMutableTexCoords();
		auto& Colors =
			LOD.VertexBuffers.ColorVertexBuffer.GetMutableColors();
		auto& Indices = LOD.IndexBuffer.GetMutableIndices();
		std::unordered_map<std::string, uint32> SectionNameCounts;
		for (const FStaticMeshImportedMesh& ImportedMesh : ImportedData.Meshes)
		{
			if (!ValidateImportedMesh(ImportedMesh, OutError))
			{
				if (!OutError.empty()) return false;
				continue;
			}
			if (Positions.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Positions.size()
				|| Indices.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Indices.size())
			{
				OutError = std::format("Static mesh '{}' exceeds uint32 render-data limits.", SourceLabel);
				return false;
			}

			const uint32 BaseVertexIndex = static_cast<uint32>(Positions.size());
			const uint32 FirstIndex = static_cast<uint32>(Indices.size());
			Positions.insert(
				Positions.end(),
				ImportedMesh.Positions.begin(),
				ImportedMesh.Positions.end());

			std::vector<FVector3f> MeshNormals = HasValidNormals(ImportedMesh.Normals, ImportedMesh.Positions.size())
				? ImportedMesh.Normals
				: BuildNormals(ImportedMesh.Positions, ImportedMesh.Indices);
			for (FVector3f& Normal : MeshNormals) Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
			Normals.insert(
				Normals.end(), MeshNormals.begin(), MeshNormals.end());

			std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> MeshTexCoords;
			for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			{
				const auto& ImportedTexCoords = ImportedMesh.UVChannels[Channel];
				const bool bValidChannel = ImportedTexCoords.size() == ImportedMesh.Positions.size()
					&& std::ranges::all_of(ImportedTexCoords, [](const FVector2f& UV) { return Math::IsFinite(UV); });
				if (bValidChannel)
				{
					MeshTexCoords[Channel] = ImportedTexCoords;
					LOD.NumTexCoords = static_cast<uint8>(std::max<uint32>(LOD.NumTexCoords, Channel + 1));
				}
				else
				{
					MeshTexCoords[Channel].assign(ImportedMesh.Positions.size(), FVector2f(0.0f));
				}
				TexCoords[Channel].insert(
					TexCoords[Channel].end(),
					MeshTexCoords[Channel].begin(),
					MeshTexCoords[Channel].end());
			}

			std::vector<FVector4f> MeshTangents;
			if (HasValidTangents(ImportedMesh.Tangents, ImportedMesh.Positions.size()))
			{
				MeshTangents.reserve(ImportedMesh.Tangents.size());
				for (size_t VertexIndex = 0; VertexIndex < ImportedMesh.Tangents.size(); ++VertexIndex)
				{
					const FVector3f& Normal = MeshNormals[VertexIndex];
					const FVector3f SourceTangent(ImportedMesh.Tangents[VertexIndex]);
					const FVector3f Tangent = SafeNormalize(SourceTangent - Normal * Math::Dot(Normal, SourceTangent), MakeStableTangent(Normal));
					MeshTangents.emplace_back(Tangent, ImportedMesh.Tangents[VertexIndex].w < 0.0f ? -1.0f : 1.0f);
				}
			}
			else
			{
				MeshTangents = BuildTangents(ImportedMesh.Positions, MeshNormals, MeshTexCoords[0], ImportedMesh.Indices);
			}
			Tangents.insert(
				Tangents.end(),
				MeshTangents.begin(),
				MeshTangents.end());

			const bool bValidColors = ImportedMesh.Colors.size() == ImportedMesh.Positions.size()
				&& std::ranges::all_of(ImportedMesh.Colors, [](const FVector4f& Color) { return Math::IsFinite(Color); });
			if (bValidColors)
			{
				Colors.insert(
					Colors.end(),
					ImportedMesh.Colors.begin(),
					ImportedMesh.Colors.end());
				LOD.bHasColorVertexData = true;
			}
			else
			{
				Colors.insert(
					Colors.end(),
					ImportedMesh.Positions.size(),
					FVector4f(1.0f));
			}

			Indices.reserve(
				Indices.size() + ImportedMesh.Indices.size());
			for (uint32 Index : ImportedMesh.Indices)
			{
				Indices.push_back(BaseVertexIndex + Index);
			}

			FStaticMeshSection Section;
			Section.Name = MakeUniqueSectionName(ImportedMesh.Name, static_cast<uint32>(LOD.Sections.size()), SectionNameCounts);
			Section.FirstIndex = FirstIndex;
			Section.IndexCount = static_cast<uint32>(ImportedMesh.Indices.size());
			Section.MinVertexIndex = BaseVertexIndex + *std::ranges::min_element(ImportedMesh.Indices);
			Section.MaxVertexIndex = BaseVertexIndex + *std::ranges::max_element(ImportedMesh.Indices);
			const auto ImportedSlot = ImportedSourceToIndex.find(ImportedMesh.SourceMaterialIndex);
			if (ImportedSlot == ImportedSourceToIndex.end())
			{
				OutError = std::format("Static mesh section '{}' references missing source material index {}.",
					Section.Name, ImportedMesh.SourceMaterialIndex);
				return false;
			}
			Section.MaterialSlotIndex = ImportedToStableSlot[ImportedSlot->second];
			LOD.Sections.emplace_back(std::move(Section));
		}

		if (Positions.empty() || Indices.empty() || LOD.Sections.empty())
		{
			OutError = std::format("Static mesh source has no renderable geometry: {}", SourceLabel);
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
			OutError = std::format("Static mesh source has invalid bounds: {}", SourceLabel);
			return false;
		}

		const float Scale = NormalizedSize / MaxDimension;
		for (FVector3f& Position : Positions)
		{
			Position = (Position - BoundsCenter) * Scale;
		}
		LOD.VertexBuffers.Finalize(
			LOD.NumTexCoords, LOD.bHasColorVertexData);
		RenderData->RecalculateBounds();

		OutRenderData = std::move(RenderData);
		OutMaterialSlots = std::move(ReconciledSlots);
		bOutSlotMetadataChanged = bSlotMetadataChanged;
		OutError.clear();
		return true;
	}

	}

	auto FStaticMeshBuildOperations::BuildAndPublishImported(
		DStaticMesh& Mesh,
		const FStaticMeshImportedData& ImportedData,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceLabel,
		std::string& OutError) -> bool
	{
		FStaticMeshBuildProduct Product;
		return BuildImportedProduct(
				Mesh, ImportedData, std::move(SourceImportData), SourceLabel,
				Product, OutError)
			&& PublishImportedProduct(Mesh, std::move(Product), OutError);
	}

	auto FStaticMeshBuildOperations::BuildImportedProduct(
		DStaticMesh& ReconciliationOwner,
		const FStaticMeshImportedData& ImportedData,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceLabel,
		FStaticMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!SourceImportData.HasSource()
			|| !IsCanonicalHash(SourceImportData.SourceContentHash)
			|| SourceImportData.ImporterId.empty()
			|| SourceImportData.ImporterVersion == 0
			|| !SourceImportData.ImportSettings.IsValid(&OutError))
		{
			OutProduct.FailureStage = EStaticMeshAuthoringFailureStage::Request;
			if (OutError.empty())
				OutError = "Imported StaticMesh build requires complete source provenance.";
			return false;
		}

		FStaticMeshBuildProduct& Product = OutProduct;
		if (!BuildRenderDataCandidate(
			ReconciliationOwner.MaterialSlots,
			ReconciliationOwner.NormalizedSize,
			ReconciliationOwner.GetObjectPath(),
			ImportedData,
			SourceLabel,
			Product.RenderData,
			Product.MaterialSlots,
			Product.bSlotMetadataChanged,
			OutError))
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::RenderConversion;
			return false;
		}

		const FStaticMeshBuildKeyInput KeyInput{
			.SourceContentHash = FXxHash128::FromString(
				SourceImportData.SourceContentHash),
			.ImporterId = SourceImportData.ImporterId,
			.ImporterVersion = SourceImportData.ImporterVersion,
			.ImportSettings = SourceImportData.ImportSettings,
			.TargetPlatform = EStaticMeshTargetPlatform::Win64};
		Product.DerivedDataKey = BuildStaticMeshDerivedDataKey(KeyInput, OutError);
		if (Product.DerivedDataKey.empty())
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::Key;
			return false;
		}
		if (!StoreProduct(Product.DerivedDataKey, *Product.RenderData, OutError))
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::DerivedDataWrite;
			return false;
		}

		Product.SourceImportData = std::move(SourceImportData);
		Product.FailureStage = EStaticMeshAuthoringFailureStage::None;
		OutError.clear();
		return true;
	}

	auto FStaticMeshBuildOperations::PublishImportedProduct(
		DStaticMesh& Mesh,
		FStaticMeshBuildProduct Product,
		std::string& OutError) -> bool
	{
		return Mesh.PublishImportedProduct(std::move(Product), OutError);
	}

	auto FStaticMeshBuildOperations::LoadDerivedDataProduct(
		DStaticMesh& Mesh,
		FStaticMeshSourceImportData SourceImportData,
		bool bSourceAvailable,
		FStaticMeshBuildProduct& OutProduct,
		EStaticMeshDerivedDataStatus& OutStatus,
		std::string& OutMessage,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		const FStaticMeshBuildKeyInput KeyInput{
			.SourceContentHash = FXxHash128::FromString(
				SourceImportData.SourceContentHash),
			.ImporterId = SourceImportData.ImporterId,
			.ImporterVersion = SourceImportData.ImporterVersion,
			.ImportSettings = SourceImportData.ImportSettings,
			.TargetPlatform = EStaticMeshTargetPlatform::Win64};
		const std::string Key = BuildStaticMeshDerivedDataKey(KeyInput, OutError);
		if (Key.empty())
		{
			OutStatus = EStaticMeshDerivedDataStatus::Incompatible;
			OutMessage = OutError;
			return false;
		}
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = GetObjectStore().Read(Key, Bytes);
		if (!Read)
		{
			OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
				? EStaticMeshDerivedDataStatus::Missing
				: EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = Read.Message;
			OutError = Read.Message;
			return false;
		}
		FStaticMeshPayloadData Payload;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
		if (Ar.HasError())
		{
			OutStatus = Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
				? EStaticMeshDerivedDataStatus::Incompatible
				: EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = Ar.GetFailure()->Message;
			OutError = OutMessage;
			return false;
		}
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		if (!MakeStaticMeshRenderData(Payload, RenderData, OutError)
			|| !RestoreRuntimeMetadata(Mesh.GetMaterialSlots(), *RenderData, OutError))
		{
			OutStatus = EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = OutError;
			return false;
		}
		OutProduct.RenderData = std::move(RenderData);
		OutProduct.MaterialSlots.assign(
			Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end());
		OutProduct.SourceImportData = std::move(SourceImportData);
		OutProduct.DerivedDataKey = Key;
		OutProduct.DerivedDataStatus = bSourceAvailable
			? EStaticMeshDerivedDataStatus::Hit
			: EStaticMeshDerivedDataStatus::SourceUnavailableCached;
		OutProduct.DiagnosticMessage = bSourceAvailable
			? std::format("StaticMesh DDC hit for key {}.", Key)
			: std::format(
				"StaticMesh source is unavailable; cached key {} loaded. Reimport and cache regeneration are unavailable.",
				Key);
		OutProduct.bSourceImporterInvoked = false;
		OutProduct.bMarkPackageDirty = false;
		OutProduct.FailureStage = EStaticMeshAuthoringFailureStage::None;
		OutStatus = OutProduct.DerivedDataStatus;
		OutMessage = OutProduct.DiagnosticMessage;
		OutError.clear();
		return true;
	}

	auto FStaticMeshBuildOperations::BuildCollisionProduct(
		const FStaticMeshRenderData& RenderData,
		const FStaticMeshSourceImportData& SourceImportData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionAuthoringProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			OutError.clear();
			return true;
		}
		if (RenderData.LODResources.empty())
		{
			OutError = "StaticMesh has no LOD 0 collision source.";
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData.LODResources.front();
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0)
		{
			OutError = "StaticMesh LOD 0 collision source is empty or malformed.";
			return false;
		}
		const FXxHash128 GeometryHash = BuildCollisionGeometryHash(Positions, Indices);
		const bool bHasSourceIdentity = IsCanonicalHash(SourceImportData.SourceContentHash);
		const FXxHash128 SourceHash = bHasSourceIdentity
			? FXxHash128::FromString(SourceImportData.SourceContentHash)
			: GeometryHash;
		FStaticMeshImportSettings Settings = SourceImportData.ImportSettings;
		if (!Settings.IsValid()) Settings = FStaticMeshImportSettings::MakeDurin();
		const FStaticMeshCollisionBuildKeyInput KeyInput{
			.SourceContentHash = SourceHash,
			.GeometryHash = GeometryHash,
			.ImporterId = SourceImportData.ImporterId.empty()
				? "CanonicalLOD0" : SourceImportData.ImporterId,
			.ImporterVersion = SourceImportData.ImporterVersion == 0
				? 1u : SourceImportData.ImporterVersion,
			.ImportSettings = Settings,
			.SourceMode = Mode,
			.QueryPolicy = Policy,
			.WeldToleranceBits = 0,
			.TargetPlatform = EStaticMeshTargetPlatform::Win64};
		OutProduct.DerivedDataKey =
			BuildStaticMeshCollisionDerivedDataKey(KeyInput, OutError);
		if (OutProduct.DerivedDataKey.empty()) return false;

		std::vector<uint8> CachedBytes;
		const Asset::FDerivedDataObjectReadResult Read =
			GetCollisionObjectStore().Read(OutProduct.DerivedDataKey, CachedBytes);
		if (Read)
		{
			FStaticMeshCollisionPayloadData Payload;
			FCanonicalMemoryReader Ar(CachedBytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			FCollisionGeometryRef Geometry;
			Payload.QueryPolicy = Policy;
			if (!Ar.HasError() && Payload.SourceMode == Mode
				&& MakeStaticMeshCollisionGeometry(Payload, Geometry, OutError))
			{
				if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
					OutProduct.Simple = Geometry;
				else OutProduct.Complex = Geometry;
				OutProduct.Status = EBodySetupCollisionBuildStatus::CacheHit;
				OutProduct.PayloadBytes = CachedBytes.size();
				OutProduct.Diagnostic = std::format(
					"StaticMesh collision DDC hit for key {}.",
					OutProduct.DerivedDataKey);
				OutError.clear();
				return true;
			}
		}

		std::vector<FVector3> BuildPositions;
		BuildPositions.reserve(Positions.size());
		for (const FVector3f& Position : Positions)
			BuildPositions.emplace_back(Position);
		FCollisionGeometryBuildDiagnostics Facts;
		FCollisionGeometryRef Geometry =
			Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			? FCollisionGeometryRef::BuildConvexHull(BuildPositions, &Facts)
			: FCollisionGeometryRef::BuildTriangleMesh(BuildPositions, Indices, &Facts);
		if (!Geometry)
		{
			OutError = std::format(
				"StaticMesh collision build failed with status {}.",
				static_cast<uint32>(Facts.Status));
			return false;
		}
		FStaticMeshCollisionPayloadData Payload;
		if (!MakeStaticMeshCollisionPayloadData(Geometry, Policy, Payload, OutError))
			return false;
		std::vector<uint8> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			return false;
		}
		if (!GetCollisionObjectStore().Write(
			OutProduct.DerivedDataKey, Bytes, &OutError)) return false;
		if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
			OutProduct.Simple = Geometry;
		else OutProduct.Complex = Geometry;
		OutProduct.Status = EBodySetupCollisionBuildStatus::Rebuilt;
		OutProduct.PayloadBytes = Bytes.size();
		OutProduct.Diagnostic = std::format(
			"Rebuilt StaticMesh collision key {} ({} triangles, {} bytes).",
			OutProduct.DerivedDataKey, Facts.RetainedTriangles,
			Geometry.GetRetainedBytes());
		OutError.clear();
		return true;
	}
}
