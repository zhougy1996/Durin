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

		auto ValidateImportedMesh(const FStaticMeshImportedMesh& Mesh, FStaticMeshRecipeError& OutError, FRecipeControl& Control) -> bool
		{
			if (Mesh.Positions.empty() || Mesh.Indices.empty()) return false;
			if (Mesh.Positions.size() > std::numeric_limits<uint32>::max())
			{
				OutError = {.Code = EStaticMeshRecipeError::VertexLimit, .MeshName = Mesh.Name, .Actual = Mesh.Positions.size(), .Expected = std::numeric_limits<uint32>::max()};
				return false;
			}
			if (Mesh.Indices.size() % 3 != 0)
			{
				OutError = {.Code = EStaticMeshRecipeError::TriangleList, .MeshName = Mesh.Name, .IndexCount = Mesh.Indices.size()};
				return false;
			}
			for (size_t Vertex = 0; Vertex < Mesh.Positions.size(); ++Vertex)
			{
				Control.Tick();
				if (Math::IsFinite(Mesh.Positions[Vertex])) continue;
				OutError = {.Code = EStaticMeshRecipeError::NonFinitePosition, .MeshName = Mesh.Name,
					.Index = Vertex, .Position = Mesh.Positions[Vertex]};
				return false;
			}
			for (size_t Offset = 0; Offset < Mesh.Indices.size(); ++Offset)
			{
				Control.Tick();
				if (Mesh.Indices[Offset] < Mesh.Positions.size()) continue;
				OutError = {.Code = EStaticMeshRecipeError::IndexRange, .MeshName = Mesh.Name,
					.Index = Offset, .Actual = Mesh.Indices[Offset], .Expected = Mesh.Positions.size()};
				return false;
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
		std::span<const FStaticMeshRecipeMaterialSlot> MaterialSlots,
		float NormalizedSize,
		const FStaticMeshDecodedGeometry& ImportedData,
		std::vector<FStaticMeshBuildLOD>& OutLODs,
		FBox& OutBounds,
		FStaticMeshRecipeError& OutError, FRecipeControl& Control) -> bool
	{
		FStaticMeshBuildMemoryEstimate Memory{Control.Execution.MaximumWorkingSetBytes};
		bool bFits = Memory.Add(1, 1024 * 1024)
			&& Memory.Add(ImportedData.Meshes.size(), 1024)
			&& Memory.Add(std::max(MaterialSlots.size(), ImportedData.MaterialSlots.size()), 32768);
		for (const auto& Mesh : ImportedData.Meshes)
		{
			Control.Tick();
			bFits = bFits && Memory.Add(Mesh.Positions.size(), 512) && Memory.Add(Mesh.Indices.size(), 192);
		}
		if (!bFits)
		{
			OutError = {.Code = EStaticMeshRecipeError::WorkingSet, .Actual = Memory.Bytes, .Expected = Memory.Limit};
			return false;
		}
		std::vector<uint32> ImportedToStableSlot;
		for (const auto& Imported : ImportedData.MaterialSlots)
		{
			Control.Tick();
			const auto Slot = std::ranges::find(MaterialSlots, Imported.SourceMaterialIndex,
				&FStaticMeshRecipeMaterialSlot::SourceMaterialIndex);
			if (Slot == MaterialSlots.end())
			{
				OutError = {.Code = EStaticMeshRecipeError::MissingMaterial, .Actual = Imported.SourceMaterialIndex};
				return false;
			}
			ImportedToStableSlot.push_back(static_cast<uint32>(Slot - MaterialSlots.begin()));
		}

		std::vector<FStaticMeshBuildLOD> LODs;
		std::unordered_map<uint32, uint32> ImportedSourceToIndex;
		for (uint32 ImportedIndex = 0; ImportedIndex < ImportedData.MaterialSlots.size(); ++ImportedIndex)
		{
			Control.Tick();
			const uint32 SourceIndex = ImportedData.MaterialSlots[ImportedIndex].SourceMaterialIndex;
			if (!ImportedSourceToIndex.emplace(SourceIndex, ImportedIndex).second)
			{
				OutError = {.Code = EStaticMeshRecipeError::DuplicateMaterial, .Index = ImportedIndex, .Actual = SourceIndex};
				return false;
			}
		}

		FStaticMeshBuildLOD& LOD = LODs.emplace_back();
		LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
		auto& Positions = LOD.Positions;
		auto& Normals = LOD.Normals;
		auto& Tangents = LOD.Tangents;
		auto& TexCoords = LOD.TexCoords;
		auto& Colors = LOD.Colors;
		auto& Indices = LOD.Indices;
		std::unordered_map<std::string, uint32> SectionNameCounts;
		for (const FStaticMeshImportedMesh& ImportedMesh : ImportedData.Meshes)
		{
			Control.Tick();
			if (!ValidateImportedMesh(ImportedMesh, OutError, Control))
			{
				if (OutError.Code != EStaticMeshRecipeError::None) return false;
				continue;
			}
			if (Positions.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Positions.size()
				|| Indices.size() > std::numeric_limits<uint32>::max() - ImportedMesh.Indices.size())
			{
				OutError = {.Code = EStaticMeshRecipeError::RenderLimits, .MeshName = ImportedMesh.Name,
					.Expected = std::numeric_limits<uint32>::max(), .VertexCount = Positions.size() + ImportedMesh.Positions.size(), .IndexCount = Indices.size() + ImportedMesh.Indices.size()};
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
				OutError = {.Code = EStaticMeshRecipeError::MissingMaterial, .MeshName = ImportedMesh.Name, .SectionName = Section.Name, .Actual = ImportedMesh.SourceMaterialIndex};
				return false;
			}
			Section.MaterialSlotIndex = ImportedToStableSlot[ImportedSlot->second];
			LOD.Sections.emplace_back(std::move(Section));
		}

		if (Positions.empty() || Indices.empty() || LOD.Sections.empty())
		{
			OutError = {.Code = EStaticMeshRecipeError::EmptyGeometry, .VertexCount = Positions.size(), .IndexCount = Indices.size()};
			return false;
		}

		FBox SourceBounds;
		for (const auto& Position : Positions)
		{
			Control.Tick();
			SourceBounds.AddPoint(FVector3(Position));
		}
		const FVector3f BoundsMin(SourceBounds.Min);
		const FVector3f BoundsMax(SourceBounds.Max);

		const FVector3f BoundsCenter = (BoundsMin + BoundsMax) * 0.5f;
		const FVector3f BoundsExtent = BoundsMax - BoundsMin;
		const float MaxDimension = std::max(BoundsExtent.x, std::max(BoundsExtent.y, BoundsExtent.z));
		if (MaxDimension <= 0.0f)
		{
			OutError = {.Code = EStaticMeshRecipeError::Bounds, .Bounds = SourceBounds};
			return false;
		}

		const float Scale = NormalizedSize / MaxDimension;
		for (FVector3f& Position : Positions)
		{
			Control.Tick();
			Position = (Position - BoundsCenter) * Scale;
		}
		LOD.LocalBounds.Reset();
		for (const auto& Position : Positions)
		{
			Control.Tick();
			LOD.LocalBounds.AddPoint(FVector3(Position));
		}
		for (auto& Section : LOD.Sections)
		{
			Section.LocalBounds.Reset();
			for (uint32 Offset = 0; Offset < Section.IndexCount; ++Offset)
			{
				Control.Tick();
				Section.LocalBounds.AddPoint(FVector3(Positions[Indices[Section.FirstIndex + Offset]]));
			}
		}
		Control.Check();
		OutBounds = LOD.LocalBounds;
		OutLODs = std::move(LODs);
		OutError = {};
		return true;
	}

	}

	static auto BuildRenderRecipeInternal(
		const FStaticMeshRecipeBuildRequest& Request,
		FStaticMeshRecipeBuildProduct& OutProduct,
		FStaticMeshRecipeError& OutError, FRecipeControl& Control) -> bool
	{
		OutProduct = {};
		Control.Check();
		if (!Request.Geometry)
		{
			OutError = {.Code = EStaticMeshRecipeError::MissingGeometry};
			return false;
		}
		return BuildRenderDataCandidate(
			Request.MaterialSlots,
			Request.NormalizedSize,
			*Request.Geometry,
			OutProduct.LODs,
			OutProduct.LocalBounds,
			OutError, Control);
	}

	static auto BuildCollisionRecipeInternal(
		const FStaticMeshCollisionRecipeRequest& Request,
		FStaticMeshCollisionRecipeProduct& OutProduct,
		FStaticMeshRecipeError& OutError, FRecipeControl& Control) -> bool
	{
		OutProduct = {};
		Control.Check();
		if (Request.Mode == EBodySetupCollisionSourceMode::None)
		{
			OutError = {};
			return true;
		}
		if (Request.Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			&& Request.Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			OutError = {.Code = EStaticMeshRecipeError::CollisionMode, .Mode = Request.Mode};
			return false;
		}
		if (Request.Positions.empty() || Request.Indices.empty()
			|| Request.Indices.size() % 3 != 0)
		{
			OutError = {.Code = EStaticMeshRecipeError::CollisionInput, .VertexCount = Request.Positions.size(), .IndexCount = Request.Indices.size(), .Mode = Request.Mode};
			return false;
		}
		FStaticMeshBuildMemoryEstimate Memory{Control.Execution.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Request.Positions.size(), 512)
			|| !Memory.Add(Request.Indices.size(), 192))
		{
			OutError = {.Code = EStaticMeshRecipeError::WorkingSet, .Actual = Memory.Bytes, .Expected = Memory.Limit, .VertexCount = Request.Positions.size(), .IndexCount = Request.Indices.size(), .Mode = Request.Mode};
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
		if (Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled)
		{
			OutError = {.Code = EStaticMeshRecipeError::Cancelled, .Mode = Request.Mode, .CollisionCause = Diagnostics};
			return false;
		}
		if (!OutProduct.Geometry)
		{
			OutError = {.Code = EStaticMeshRecipeError::CollisionBuild, .Mode = Request.Mode, .CollisionCause = Diagnostics};
			return false;
		}
		OutError = {};
		return true;
	}

	auto FStaticMeshBuildOperations::BuildRenderRecipe(const FStaticMeshRecipeBuildRequest& Request,
		const FStaticMeshBuildExecutionControl& Execution) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError>
	{
		FStaticMeshRecipeBuildProduct Product;
		FStaticMeshRecipeError Error;
		FRecipeControl Control{Execution};
		try
		{
			const bool bSucceeded = BuildRenderRecipeInternal(Request, Product, Error, Control);
			Control.Check();
			Error.Kind = EStaticMeshRecipeKind::Render;
			if (bSucceeded) return Product;
			return std::unexpected(std::move(Error));
		}
		catch (const FRecipeCancelled&)
		{
			return std::unexpected(FStaticMeshRecipeError{.Code = EStaticMeshRecipeError::Cancelled, .Kind = EStaticMeshRecipeKind::Render});
		}
	}

	auto FStaticMeshBuildOperations::BuildCollisionRecipe(const FStaticMeshCollisionRecipeRequest& Request,
		const FStaticMeshBuildExecutionControl& Execution) -> std::expected<FStaticMeshCollisionRecipeProduct, FStaticMeshRecipeError>
	{
		FStaticMeshCollisionRecipeProduct Product;
		FStaticMeshRecipeError Error;
		FRecipeControl Control{Execution};
		try
		{
			const bool bSucceeded = BuildCollisionRecipeInternal(Request, Product, Error, Control);
			Control.Check();
			Error.Kind = EStaticMeshRecipeKind::Collision;
			if (bSucceeded) return Product;
			return std::unexpected(std::move(Error));
		}
		catch (const FRecipeCancelled&)
		{
			Error.Code = EStaticMeshRecipeError::Cancelled;
			Error.Kind = EStaticMeshRecipeKind::Collision;
			Error.Mode = Request.Mode;
			return std::unexpected(std::move(Error));
		}
	}

}
