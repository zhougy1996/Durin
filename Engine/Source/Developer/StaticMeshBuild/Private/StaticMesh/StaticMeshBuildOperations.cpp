#include "StaticMesh/StaticMeshBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr float VectorTolerance = 1.0e-10f;

		struct FRecipeCancelled {};

		// Unwinds only the synchronous recipe stack; no exception crosses the provider ABI.
		struct FRecipeControl
		{
			const FStaticMeshBuildExecutionControl& Execution;
			uint32 WorkSinceCheckpoint = 0;

			auto Check() const -> void
			{
				if (Execution.IsCancelled()) throw FRecipeCancelled{};
			}

			auto Tick() -> void
			{
				if (++WorkSinceCheckpoint < 256) return;
				WorkSinceCheckpoint = 0;
				Check();
			}
		};

		auto SlotDefinitionsEqual(
			std::span<const FStaticMeshRecipeMaterialSlot> A,
			std::span<const FStaticMeshRecipeMaterialSlot> B) -> bool
		{
			if (A.size() != B.size()) return false;
			for (size_t Index = 0; Index < A.size(); ++Index)
			{
				if (A[Index].Name != B[Index].Name
					|| A[Index].SourceName != B[Index].SourceName
					|| A[Index].SourceMaterialIndex != B[Index].SourceMaterialIndex) return false;
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

		auto BuildNormals(const std::vector<FVector3f>& Positions, const std::vector<uint32>& Indices, FRecipeControl& Control) -> std::vector<FVector3f>
		{
			std::vector<FVector3f> Normals(Positions.size(), FVector3f(0.0f));
			for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
			{
				Control.Tick();
				const uint32 I0 = Indices[Index];
				const uint32 I1 = Indices[Index + 1];
				const uint32 I2 = Indices[Index + 2];
				const FVector3f FaceNormal = Math::Cross(Positions[I1] - Positions[I0], Positions[I2] - Positions[I0]);
				if (!Math::IsFinite(FaceNormal) || Math::LengthSquared(FaceNormal) <= VectorTolerance) continue;
				Normals[I0] += FaceNormal;
				Normals[I1] += FaceNormal;
				Normals[I2] += FaceNormal;
			}
			for (FVector3f& Normal : Normals)
			{
				Control.Tick();
				Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
			}
			return Normals;
		}

		auto BuildTangents(
			const std::vector<FVector3f>& Positions,
			const std::vector<FVector3f>& Normals,
			const std::vector<FVector2f>& UV0,
			const std::vector<uint32>& Indices, FRecipeControl& Control) -> std::vector<FVector4f>
		{
			std::vector<FVector3f> TangentAccum(Positions.size(), FVector3f(0.0f));
			std::vector<FVector3f> BitangentAccum(Positions.size(), FVector3f(0.0f));
			const bool bHasUV0 = UV0.size() == Positions.size();
			if (bHasUV0)
			{
				for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
				{
					Control.Tick();
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
						Control.Tick();
						TangentAccum[VertexIndex] += Tangent;
						BitangentAccum[VertexIndex] += Bitangent;
					}
				}
			}

			std::vector<FVector4f> Tangents(Positions.size());
			for (size_t VertexIndex = 0; VertexIndex < Positions.size(); ++VertexIndex)
			{
				Control.Tick();
				const FVector3f& Normal = Normals[VertexIndex];
				const FVector3f Orthogonalized = TangentAccum[VertexIndex] - Normal * Math::Dot(Normal, TangentAccum[VertexIndex]);
				const FVector3f Tangent = SafeNormalize(Orthogonalized, MakeStableTangent(Normal));
				const float Sign = Math::Dot(Math::Cross(Normal, Tangent), BitangentAccum[VertexIndex]) < 0.0f ? -1.0f : 1.0f;
				Tangents[VertexIndex] = FVector4f(Tangent, Sign);
			}
			return Tangents;
		}

		auto HasValidNormals(const std::vector<FVector3f>& Normals, size_t NumVertices, FRecipeControl& Control) -> bool
		{
			return Normals.size() == NumVertices && std::ranges::all_of(Normals, [&Control](const FVector3f& Normal) {
				Control.Tick();
				return Math::IsFinite(Normal) && Math::LengthSquared(Normal) > VectorTolerance;
			});
		}

		auto HasValidTangents(const std::vector<FVector4f>& Tangents, size_t NumVertices, FRecipeControl& Control) -> bool
		{
			return Tangents.size() == NumVertices && std::ranges::all_of(Tangents, [&Control](const FVector4f& Tangent) {
				Control.Tick();
				const FVector3f Direction(Tangent);
				return Math::IsFinite(Tangent) && Math::LengthSquared(Direction) > VectorTolerance && std::abs(Tangent.w) > 0.5f;
			});
		}

		auto ValidateImportedMesh(const FStaticMeshImportedMesh& Mesh, std::string& OutError, FRecipeControl& Control) -> bool
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
			if (!std::ranges::all_of(Mesh.Positions, [&Control](const FVector3f& Position) { Control.Tick(); return Math::IsFinite(Position); }))
			{
				OutError = std::format("Mesh '{}' contains a non-finite position.", Mesh.Name);
				return false;
			}
			for (uint32 Index : Mesh.Indices)
			{
				Control.Tick();
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
		std::span<const FStaticMeshRecipeMaterialSlot> PreviousMaterialSlots,
		float NormalizedSize,
		const FStaticMeshDecodedGeometry& ImportedData,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::vector<FStaticMeshRecipeMaterialSlot>& OutMaterialSlots,
		bool& bOutSlotMetadataChanged,
		std::string& OutError, FRecipeControl& Control) -> bool
	{
		FStaticMeshBuildMemoryEstimate Memory{Control.Execution.MaximumWorkingSetBytes};
		bool bFits = Memory.Add(1, 1024 * 1024)
			&& Memory.Add(ImportedData.Meshes.size(), 1024)
			&& Memory.Add(std::max(PreviousMaterialSlots.size(), ImportedData.MaterialSlots.size()), 32768);
		for (const auto& Mesh : ImportedData.Meshes)
		{
			Control.Tick();
			bFits = bFits && Memory.Add(Mesh.Positions.size(), 512) && Memory.Add(Mesh.Indices.size(), 192);
		}
		if (!bFits)
		{
			OutError = "StaticMesh predicted render working set exceeds its reservation.";
			return false;
		}
		const std::vector<FStaticMeshRecipeMaterialSlot> PreviousSlots(
			PreviousMaterialSlots.begin(), PreviousMaterialSlots.end());
		std::vector<FStaticMeshRecipeMaterialSlot> ReconciledSlots = PreviousSlots;
		std::vector<bool> OldConsumed(PreviousSlots.size(), false);
		std::vector<bool> NewMatched(ImportedData.MaterialSlots.size(), false);
		std::vector<uint32> ImportedToStableSlot(
			ImportedData.MaterialSlots.size(), std::numeric_limits<uint32>::max());
		std::unordered_map<std::string, uint32> OldNameCounts;
		std::unordered_map<std::string, uint32> NewNameCounts;
		std::unordered_map<uint32, uint32> OldSourceIndexCounts;
		std::unordered_map<uint32, uint32> NewSourceIndexCounts;
		for (const FStaticMeshRecipeMaterialSlot& Slot : PreviousSlots) ++OldNameCounts[Slot.SourceName];
		for (const FStaticMeshImportedMaterialSlot& Slot : ImportedData.MaterialSlots) ++NewNameCounts[Slot.SourceName];
		for (const FStaticMeshRecipeMaterialSlot& Slot : PreviousSlots) ++OldSourceIndexCounts[Slot.SourceMaterialIndex];
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
			Control.Tick();
			const std::string& SourceName = ImportedData.MaterialSlots[NewIndex].SourceName;
			if (SourceName.empty()) continue;
			if (OldNameCounts[SourceName] != 1 || NewNameCounts[SourceName] != 1) continue;
			const auto It = std::ranges::find_if(PreviousSlots, [&](const auto& Slot) {
				Control.Tick();
				return Slot.SourceName == SourceName;
			});
			if (It != PreviousSlots.end()) PreserveSlot(NewIndex, static_cast<size_t>(It - PreviousSlots.begin()));
		}

		for (size_t NewIndex = 0; NewIndex < ImportedData.MaterialSlots.size(); ++NewIndex)
		{
			Control.Tick();
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedData.MaterialSlots[NewIndex];
			if (OldSourceIndexCounts[Imported.SourceMaterialIndex] != 1
				|| NewSourceIndexCounts[Imported.SourceMaterialIndex] != 1) continue;
			for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
			{
				Control.Tick();
				const FStaticMeshRecipeMaterialSlot& Previous = PreviousSlots[OldIndex];
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
			while (std::ranges::find_if(ReconciledSlots, [&](const auto& Slot) {
				Control.Tick();
				return Slot.Name == Candidate;
			})
				!= ReconciledSlots.end())
			{
				Candidate = FName(std::format("{}_{}", BaseName, Suffix++));
			}
			return Candidate;
		};

		for (size_t NewIndex = 0; NewIndex < ImportedData.MaterialSlots.size(); ++NewIndex)
		{
			Control.Tick();
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedData.MaterialSlots[NewIndex];
			FStaticMeshRecipeMaterialSlot& Definition = ReconciledSlots.emplace_back();
			Definition.Name = MakeUniqueSlotName(Imported);
			Definition.SourceName = Imported.SourceName;
			Definition.SourceMaterialIndex = Imported.SourceMaterialIndex;
			ImportedToStableSlot[NewIndex] = static_cast<uint32>(ReconciledSlots.size() - 1);
			if (NewNameCounts[Imported.SourceName] > 1)
			{
				DURIN_WARN("Static mesh has ambiguous duplicate source material name '{}'; appended a stable slot.",
					Imported.SourceName);
			}
		}

		// Preserved editor slots that no longer map to an imported material must
		// not alias a current source index. Scene publication validates lookups by
		// source index, and an old unmatched slot can otherwise become ambiguous
		// after a reorder followed by removal.
		std::unordered_set<uint32> AssignedSourceIndices;
		for (const FStaticMeshImportedMaterialSlot& Imported : ImportedData.MaterialSlots)
			AssignedSourceIndices.insert(Imported.SourceMaterialIndex);
		uint32 RetiredSourceIndex = 0;
		for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
		{
			Control.Tick();
			if (OldConsumed[OldIndex]) continue;
			while (AssignedSourceIndices.contains(RetiredSourceIndex))
				++RetiredSourceIndex;
			ReconciledSlots[OldIndex].SourceMaterialIndex = RetiredSourceIndex;
			AssignedSourceIndices.insert(RetiredSourceIndex++);
		}

		const bool bSlotMetadataChanged =
			!SlotDefinitionsEqual(PreviousMaterialSlots, ReconciledSlots);

		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.reserve(ReconciledSlots.size());
		for (const FStaticMeshRecipeMaterialSlot& Slot : ReconciledSlots)
		{
			Control.Tick();
			RenderData->MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex});
		}
		std::unordered_map<uint32, uint32> ImportedSourceToIndex;
		for (uint32 ImportedIndex = 0; ImportedIndex < ImportedData.MaterialSlots.size(); ++ImportedIndex)
		{
			Control.Tick();
			const uint32 SourceIndex = ImportedData.MaterialSlots[ImportedIndex].SourceMaterialIndex;
			if (!ImportedSourceToIndex.emplace(SourceIndex, ImportedIndex).second)
			{
				OutError = std::format(
					"Static mesh has duplicate imported source material index {}.", SourceIndex);
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
			Control.Tick();
			if (!ValidateImportedMesh(ImportedMesh, OutError, Control))
			{
				if (!OutError.empty()) return false;
				continue;
			}
			if (Positions.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Positions.size()
				|| Indices.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Indices.size())
			{
				OutError = "Static mesh exceeds uint32 render-data limits.";
				return false;
			}

			const uint32 BaseVertexIndex = static_cast<uint32>(Positions.size());
			const uint32 FirstIndex = static_cast<uint32>(Indices.size());
			Positions.insert(
				Positions.end(),
				ImportedMesh.Positions.begin(),
				ImportedMesh.Positions.end());

			std::vector<FVector3f> MeshNormals = HasValidNormals(ImportedMesh.Normals, ImportedMesh.Positions.size(), Control)
				? ImportedMesh.Normals
				: BuildNormals(ImportedMesh.Positions, ImportedMesh.Indices, Control);
			for (FVector3f& Normal : MeshNormals)
			{
				Control.Tick();
				Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
			}
			Normals.insert(
				Normals.end(), MeshNormals.begin(), MeshNormals.end());

			std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> MeshTexCoords;
			for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			{
				Control.Tick();
				const auto& ImportedTexCoords = ImportedMesh.UVChannels[Channel];
				const bool bValidChannel = ImportedTexCoords.size() == ImportedMesh.Positions.size()
					&& std::ranges::all_of(ImportedTexCoords, [&Control](const FVector2f& UV) { Control.Tick(); return Math::IsFinite(UV); });
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
			if (HasValidTangents(ImportedMesh.Tangents, ImportedMesh.Positions.size(), Control))
			{
				MeshTangents.reserve(ImportedMesh.Tangents.size());
				for (size_t VertexIndex = 0; VertexIndex < ImportedMesh.Tangents.size(); ++VertexIndex)
				{
					Control.Tick();
					const FVector3f& Normal = MeshNormals[VertexIndex];
					const FVector3f SourceTangent(ImportedMesh.Tangents[VertexIndex]);
					const FVector3f Tangent = SafeNormalize(SourceTangent - Normal * Math::Dot(Normal, SourceTangent), MakeStableTangent(Normal));
					MeshTangents.emplace_back(Tangent, ImportedMesh.Tangents[VertexIndex].w < 0.0f ? -1.0f : 1.0f);
				}
			}
			else
			{
				MeshTangents = BuildTangents(ImportedMesh.Positions, MeshNormals, MeshTexCoords[0], ImportedMesh.Indices, Control);
			}
			Tangents.insert(
				Tangents.end(),
				MeshTangents.begin(),
				MeshTangents.end());

			const bool bValidColors = ImportedMesh.Colors.size() == ImportedMesh.Positions.size()
				&& std::ranges::all_of(ImportedMesh.Colors, [&Control](const FVector4f& Color) { Control.Tick(); return Math::IsFinite(Color); });
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
			uint32 MinimumIndex = std::numeric_limits<uint32>::max();
			uint32 MaximumIndex = 0;
			for (uint32 Index : ImportedMesh.Indices)
			{
				Control.Tick();
				MinimumIndex = std::min(MinimumIndex, Index);
				MaximumIndex = std::max(MaximumIndex, Index);
				Indices.push_back(BaseVertexIndex + Index);
			}

			FStaticMeshSection Section;
			Section.Name = MakeUniqueSectionName(ImportedMesh.Name, static_cast<uint32>(LOD.Sections.size()), SectionNameCounts);
			Section.FirstIndex = FirstIndex;
			Section.IndexCount = static_cast<uint32>(ImportedMesh.Indices.size());
			Section.MinVertexIndex = BaseVertexIndex + MinimumIndex;
			Section.MaxVertexIndex = BaseVertexIndex + MaximumIndex;
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
			OutError = "Static mesh source has no renderable geometry.";
			return false;
		}

		if (!RenderData->RecalculateBounds([&] { return Control.Execution.IsCancelled(); }))
			throw FRecipeCancelled{};
		const FVector3f BoundsMin(RenderData->LocalBounds.Min);
		const FVector3f BoundsMax(RenderData->LocalBounds.Max);

		const FVector3f BoundsCenter = (BoundsMin + BoundsMax) * 0.5f;
		const FVector3f BoundsExtent = BoundsMax - BoundsMin;
		const float MaxDimension = std::max(BoundsExtent.x, std::max(BoundsExtent.y, BoundsExtent.z));
		if (MaxDimension <= 0.0f)
		{
			OutError = "Static mesh source has invalid bounds.";
			return false;
		}

		const float Scale = NormalizedSize / MaxDimension;
		for (FVector3f& Position : Positions)
		{
			Control.Tick();
			Position = (Position - BoundsCenter) * Scale;
		}
		LOD.VertexBuffers.Finalize(
			LOD.NumTexCoords, LOD.bHasColorVertexData);
		if (!RenderData->RecalculateBounds([&] { return Control.Execution.IsCancelled(); }))
			throw FRecipeCancelled{};

		OutRenderData = std::move(RenderData);
		OutMaterialSlots = std::move(ReconciledSlots);
		bOutSlotMetadataChanged = bSlotMetadataChanged;
		OutError.clear();
		return true;
	}

	}

	static auto BuildRenderRecipeInternal(
		const FStaticMeshRecipeBuildRequest& Request,
		FStaticMeshRecipeBuildProduct& OutProduct,
		std::string& OutError, FRecipeControl& Control) -> bool
	{
		OutProduct = {};
		Control.Check();
		if (!Request.Geometry)
		{
			OutError = "StaticMesh recipe requires decoded geometry.";
			return false;
		}
		return BuildRenderDataCandidate(
			Request.PreviousMaterialSlots,
			Request.NormalizedSize,
			*Request.Geometry,
			OutProduct.RenderData,
			OutProduct.MaterialSlots,
			OutProduct.bSlotMetadataChanged,
			OutError, Control);
	}

	static auto BuildCollisionRecipeInternal(
		const FStaticMeshCollisionRecipeRequest& Request,
		FStaticMeshCollisionRecipeProduct& OutProduct,
		std::string& OutError, FRecipeControl& Control) -> bool
	{
		OutProduct = {};
		Control.Check();
		if (Request.Mode == EBodySetupCollisionSourceMode::None)
		{
			OutError.clear();
			return true;
		}
		if (Request.Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			&& Request.Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			OutError = "StaticMesh collision source mode is invalid.";
			return false;
		}
		if (Request.Positions.empty() || Request.Indices.empty()
			|| Request.Indices.size() % 3 != 0)
		{
			OutError = "StaticMesh collision recipe input is empty or malformed.";
			return false;
		}
		FStaticMeshBuildMemoryEstimate Memory{Control.Execution.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Request.Positions.size(), 512)
			|| !Memory.Add(Request.Indices.size(), 192))
		{
			OutError = "StaticMesh predicted collision working set exceeds its reservation.";
			return false;
		}
		FCollisionGeometryBuildDiagnostics Diagnostics;
		std::vector<FVector3> CollisionPositions;
		CollisionPositions.reserve(Request.Positions.size());
		for (const FVector3f& Position : Request.Positions)
		{
			Control.Tick();
			CollisionPositions.emplace_back(Position);
		}
		Control.Check();
		const std::function<bool()> ShouldCancel = [&] { return Control.Execution.IsCancelled(); };
		OutProduct.Geometry =
			Request.Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			? FCollisionGeometryRef::BuildConvexHull(CollisionPositions, &Diagnostics, ShouldCancel)
			: FCollisionGeometryRef::BuildTriangleMesh(
				CollisionPositions, Request.Indices, &Diagnostics, ShouldCancel);
		if (Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled) throw FRecipeCancelled{};
		if (!OutProduct.Geometry)
		{
			OutError = std::format(
				"StaticMesh collision build failed with status {}.",
				static_cast<uint32>(Diagnostics.Status));
			return false;
		}
		OutError.clear();
		return true;
	}


	auto FStaticMeshBuildOperations::BuildRenderRecipe(const FStaticMeshRecipeBuildRequest& Request,
		FStaticMeshRecipeBuildProduct& OutProduct, std::string& OutError,
		const FStaticMeshBuildExecutionControl& Execution) -> FStaticMeshBuildOutcome
	{
		OutProduct = {};
		OutError.clear();
		FRecipeControl Control{Execution};
		try
		{
			const bool bSucceeded = BuildRenderRecipeInternal(Request, OutProduct, OutError, Control);
			Control.Check();
			if (!bSucceeded) OutProduct = {};
			OutError.resize(std::min(OutError.size(), MaximumStaticMeshBuildDiagnosticBytes));
			return {bSucceeded ? EStaticMeshBuildStatus::Succeeded : EStaticMeshBuildStatus::Failed, OutError};
		}
		catch (const FRecipeCancelled&)
		{
			OutProduct = {};
			OutError = "StaticMesh render recipe was cancelled.";
			return {EStaticMeshBuildStatus::Cancelled, OutError};
		}
	}

	auto FStaticMeshBuildOperations::BuildCollisionRecipe(const FStaticMeshCollisionRecipeRequest& Request,
		FStaticMeshCollisionRecipeProduct& OutProduct, std::string& OutError,
		const FStaticMeshBuildExecutionControl& Execution) -> FStaticMeshBuildOutcome
	{
		OutProduct = {};
		OutError.clear();
		FRecipeControl Control{Execution};
		try
		{
			const bool bSucceeded = BuildCollisionRecipeInternal(Request, OutProduct, OutError, Control);
			Control.Check();
			if (!bSucceeded) OutProduct = {};
			OutError.resize(std::min(OutError.size(), MaximumStaticMeshBuildDiagnosticBytes));
			return {bSucceeded ? EStaticMeshBuildStatus::Succeeded : EStaticMeshBuildStatus::Failed, OutError};
		}
		catch (const FRecipeCancelled&)
		{
			OutProduct = {};
			OutError = "StaticMesh collision recipe was cancelled.";
			return {EStaticMeshBuildStatus::Cancelled, OutError};
		}
	}
}
