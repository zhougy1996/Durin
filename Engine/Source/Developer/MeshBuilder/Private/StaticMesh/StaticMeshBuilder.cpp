#include "StaticMesh/StaticMeshBuilder.h"

#include "Logging/LogMacros.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		// Mutable scratch retained only while building the candidate.
		struct FBuildLOD : FStaticMeshVertexData
		{
			std::vector<FStaticMeshSection> Sections;
			FBox LocalBounds;
			uint8 NumTexCoords = 0;
			bool bHasColorVertexData = false;
		};

		constexpr float VectorTolerance = 1.0e-10f;

		struct FBuildCancelled {};

		// Unwinds only the synchronous build stack; cancellation does not cross the module ABI.
		struct FBuildControl
		{
			const FAssetBuildTaskContext& Execution;
			uint32 WorkSinceCheckpoint = 0;

			auto Check() const -> void
			{
				if (Execution.IsCancelled()) throw FBuildCancelled{};
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

		auto BuildNormals(std::span<const FVector3f> Positions, std::span<const uint32> Indices, FBuildControl& Control) -> std::vector<FVector3f>
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
			std::span<const FVector3f> Positions,
			std::span<const FVector3f> Normals,
			std::span<const FVector2f> UV0,
			std::span<const uint32> Indices, FBuildControl& Control) -> std::vector<FVector4f>
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

		auto HasValidNormals(std::span<const FVector3f> Normals, size_t NumVertices, FBuildControl& Control) -> bool
		{
			return Normals.size() == NumVertices && std::ranges::all_of(Normals, [&Control](const FVector3f& Normal) {
				Control.Tick();
				return Math::IsFinite(Normal) && Math::LengthSquared(Normal) > VectorTolerance;
			});
		}

		auto HasValidTangents(std::span<const FVector4f> Tangents, size_t NumVertices, FBuildControl& Control) -> bool
		{
			return Tangents.size() == NumVertices && std::ranges::all_of(Tangents, [&Control](const FVector4f& Tangent) {
				Control.Tick();
				const FVector3f Direction(Tangent);
				return Math::IsFinite(Tangent) && Math::LengthSquared(Direction) > VectorTolerance && std::abs(Tangent.w) > 0.5f;
			});
		}

		auto ValidateImportedMesh(const FMeshDescriptionSection& Mesh, std::span<const FVector3f> Positions, FBuildControl& Control) -> bool
		{
			if (Positions.size() > std::numeric_limits<uint32>::max())
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: mesh '{}' exceeds the uint32 vertex limit (actual {}, expected {}).",
					Mesh.Name, Positions.size(), std::numeric_limits<uint32>::max());
				return false;
			}
			if (Mesh.Indices.size() % 3 != 0)
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: mesh '{}' index count {} is not a triangle list.", Mesh.Name, Mesh.Indices.size());
				return false;
			}
			for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
			{
				Control.Tick();
				if (Math::IsFinite(Positions[Vertex])) continue;
				Control.Check();
				const auto& Position = Positions[Vertex];
				DURIN_ERROR("StaticMesh render build: mesh '{}' has non-finite position {} ({}, {}, {}).",
					Mesh.Name, Vertex, Position.x, Position.y, Position.z);
				return false;
			}
			for (size_t Offset = 0; Offset < Mesh.Indices.size(); ++Offset)
			{
				Control.Tick();
				if (Mesh.Indices[Offset] < Positions.size()) continue;
				Control.Check();
				DURIN_ERROR("StaticMesh render build: mesh '{}' contains an out-of-range index (index {}, actual {}, expected {}).",
					Mesh.Name, Offset, Mesh.Indices[Offset], Positions.size());
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

		auto BuildLODStreams(const FStaticMeshBuildParameters& Parameters,
			FBuildLOD& LOD, FBuildControl& Control) -> bool
		{
			const auto& ImportedData = *Parameters.Geometry;
			const auto MaterialSlots = Parameters.MaterialSlots;
			FAssetBuildMemoryEstimate Memory{Control.Execution.MaximumWorkingSetBytes};
			bool bFits = Memory.Add(1, 1024 * 1024)
				&& Memory.Add(ImportedData.Sections.size(), 1024)
				&& Memory.Add(std::max(MaterialSlots.size(), ImportedData.PolygonGroups.size()), 32768);
			for (const auto& Mesh : ImportedData.Sections)
			{
				Control.Tick();
				bFits = bFits && Memory.Add(std::max(Mesh.Positions.size(), Mesh.GetVertexInstanceCount()), 512) && Memory.Add(Mesh.Indices.size(), 192);
			}
			if (!bFits)
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: working-set reservation exceeded (reserved {} bytes, accepted {} bytes, rejected {} x {} bytes).",
					Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth);
				return false;
			}
			std::unordered_map<uint32, uint32> SourceToStableSlot;
			for (const auto& Imported : ImportedData.PolygonGroups)
			{
				Control.Tick();
				const auto Slot = std::ranges::find(MaterialSlots, Imported.SourceMaterialIndex,
					&FStaticMeshBuildMaterialSlot::SourceMaterialIndex);
				if (Slot == MaterialSlots.end())
				{
					Control.Check();
					DURIN_ERROR("StaticMesh render build: polygon group references missing source material {}.", Imported.SourceMaterialIndex);
					return false;
				}
				if (!SourceToStableSlot.emplace(Imported.SourceMaterialIndex, static_cast<uint32>(Slot - MaterialSlots.begin())).second)
				{
					Control.Check();
					DURIN_ERROR("StaticMesh render build: duplicate polygon-group source material {}.", Imported.SourceMaterialIndex);
					return false;
				}
			}

			auto& Positions = LOD.Positions;
			auto& Normals = LOD.Normals;
			auto& Tangents = LOD.Tangents;
			auto& TexCoords = LOD.TexCoords;
			auto& Colors = LOD.Colors;
			auto& Indices = LOD.Indices;
			std::unordered_map<std::string, uint32> SectionNameCounts;
			for (const FMeshDescriptionSection& SourceMesh : ImportedData.Sections)
			{
				Control.Tick();
				// Split source vertices at instance boundaries before generating render streams.
				std::vector<FVector3f> ExpandedPositions;
				std::span<const FVector3f> MeshPositions = SourceMesh.Positions;
				if (!SourceMesh.VertexInstanceVertices.empty())
				{
					ExpandedPositions.reserve(SourceMesh.GetVertexInstanceCount());
					for (uint32 Vertex : SourceMesh.VertexInstanceVertices)
					{
						Control.Tick();
						if (Vertex >= SourceMesh.Positions.size())
						{
							Control.Check();
							DURIN_ERROR("StaticMesh render build: mesh '{}' instance mapping has out-of-range vertex {} (vertex count {}).",
								SourceMesh.Name, Vertex, SourceMesh.Positions.size());
							return false;
						}
						ExpandedPositions.push_back(SourceMesh.Positions[Vertex]);
					}
					MeshPositions = ExpandedPositions;
				}
				if (MeshPositions.empty() || SourceMesh.Indices.empty()) continue;
				if (!ValidateImportedMesh(SourceMesh, MeshPositions, Control)) return false;
				if (Positions.size() > std::numeric_limits<uint32>::max() - MeshPositions.size()
					|| Indices.size() > std::numeric_limits<uint32>::max() - SourceMesh.Indices.size())
				{
					Control.Check();
					DURIN_ERROR("StaticMesh render build: mesh '{}' exceeds uint32 render limits ({} vertices, {} indices, limit {}).",
						SourceMesh.Name, Positions.size() + MeshPositions.size(), Indices.size() + SourceMesh.Indices.size(),
						std::numeric_limits<uint32>::max());
					return false;
				}

				const uint32 BaseVertexIndex = static_cast<uint32>(Positions.size());
				const uint32 FirstIndex = static_cast<uint32>(Indices.size());
				Positions.insert(
					Positions.end(),
					MeshPositions.begin(),
					MeshPositions.end());

				std::vector<FVector3f> MeshNormals;
				if (HasValidNormals(SourceMesh.Normals, MeshPositions.size(), Control))
				{
					MeshNormals = SourceMesh.Normals;
					for (FVector3f& Normal : MeshNormals)
					{
						Control.Tick();
						Normal = SafeNormalize(Normal, FVector3f(0.0f, 0.0f, 1.0f));
					}
				}
				else if (!SourceMesh.VertexInstanceVertices.empty())
				{
					// UV splits alone must not turn a shared geometric vertex into a hard normal seam.
					std::vector<uint32> VertexIndices;
					VertexIndices.reserve(SourceMesh.Indices.size());
					for (uint32 Instance : SourceMesh.Indices)
					{
						Control.Tick();
						VertexIndices.push_back(SourceMesh.GetVertexIndex(Instance));
					}
					const auto VertexNormals = BuildNormals(SourceMesh.Positions, VertexIndices, Control);
					MeshNormals.reserve(SourceMesh.GetVertexInstanceCount());
					for (uint32 Vertex : SourceMesh.VertexInstanceVertices)
					{
						Control.Tick();
						MeshNormals.push_back(VertexNormals[Vertex]);
					}
				}
				else MeshNormals = BuildNormals(MeshPositions, SourceMesh.Indices, Control);
				Normals.insert(
					Normals.end(), MeshNormals.begin(), MeshNormals.end());

				for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
				{
					Control.Tick();
					const auto& ImportedTexCoords = SourceMesh.UVChannels[Channel];
					const bool bValidChannel = ImportedTexCoords.size() == MeshPositions.size()
						&& std::ranges::all_of(ImportedTexCoords, [&Control](const FVector2f& UV) { Control.Tick(); return Math::IsFinite(UV); });
					if (bValidChannel)
					{
						TexCoords[Channel].insert(TexCoords[Channel].end(), ImportedTexCoords.begin(), ImportedTexCoords.end());
						LOD.NumTexCoords = static_cast<uint8>(std::max<uint32>(LOD.NumTexCoords, Channel + 1));
					}
					else
					{
						TexCoords[Channel].insert(TexCoords[Channel].end(), MeshPositions.size(), FVector2f(0.0f));
					}
				}

				std::vector<FVector4f> MeshTangents;
				if (HasValidTangents(SourceMesh.Tangents, MeshPositions.size(), Control))
				{
					MeshTangents.reserve(SourceMesh.Tangents.size());
					for (size_t VertexIndex = 0; VertexIndex < SourceMesh.Tangents.size(); ++VertexIndex)
					{
						Control.Tick();
						const FVector3f& Normal = MeshNormals[VertexIndex];
						const FVector3f SourceTangent(SourceMesh.Tangents[VertexIndex]);
						const FVector3f Tangent = SafeNormalize(SourceTangent - Normal * Math::Dot(Normal, SourceTangent), MakeStableTangent(Normal));
						MeshTangents.emplace_back(Tangent, SourceMesh.Tangents[VertexIndex].w < 0.0f ? -1.0f : 1.0f);
					}
				}
				else
				{
					const auto UV0 = std::span<const FVector2f>(TexCoords[0]).subspan(BaseVertexIndex, MeshPositions.size());
					MeshTangents = BuildTangents(MeshPositions, MeshNormals, UV0, SourceMesh.Indices, Control);
				}
				Tangents.insert(
					Tangents.end(),
					MeshTangents.begin(),
					MeshTangents.end());

				const bool bValidColors = SourceMesh.Colors.size() == MeshPositions.size()
					&& std::ranges::all_of(SourceMesh.Colors, [&Control](const FVector4f& Color) { Control.Tick(); return Math::IsFinite(Color); });
				if (bValidColors)
				{
					Colors.insert(
						Colors.end(),
						SourceMesh.Colors.begin(),
						SourceMesh.Colors.end());
					LOD.bHasColorVertexData = true;
				}
				else
				{
					Colors.insert(
						Colors.end(),
						MeshPositions.size(),
						FVector4f(1.0f));
				}

				Indices.reserve(
					Indices.size() + SourceMesh.Indices.size());
				uint32 MinimumIndex = std::numeric_limits<uint32>::max();
				uint32 MaximumIndex = 0;
				for (uint32 Index : SourceMesh.Indices)
				{
					Control.Tick();
					MinimumIndex = std::min(MinimumIndex, Index);
					MaximumIndex = std::max(MaximumIndex, Index);
					Indices.push_back(BaseVertexIndex + Index);
				}

				FStaticMeshSection Section;
				Section.Name = MakeUniqueSectionName(SourceMesh.Name, static_cast<uint32>(LOD.Sections.size()), SectionNameCounts);
				Section.FirstIndex = FirstIndex;
				Section.IndexCount = static_cast<uint32>(SourceMesh.Indices.size());
				Section.MinVertexIndex = BaseVertexIndex + MinimumIndex;
				Section.MaxVertexIndex = BaseVertexIndex + MaximumIndex;
				const auto Slot = SourceToStableSlot.find(SourceMesh.SourceMaterialIndex);
				if (Slot == SourceToStableSlot.end())
				{
					Control.Check();
					DURIN_ERROR("StaticMesh render build: mesh '{}' section '{}' references missing source material {}.",
						SourceMesh.Name, Section.Name, SourceMesh.SourceMaterialIndex);
					return false;
				}
				Section.MaterialSlotIndex = Slot->second;
				LOD.Sections.emplace_back(std::move(Section));
			}

			if (Positions.empty() || Indices.empty() || LOD.Sections.empty())
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: source has no renderable geometry ({} vertices, {} indices, {} sections).",
					Positions.size(), Indices.size(), LOD.Sections.size());
				return false;
			}

			FBox SourceBounds;
			for (const auto& Section : ImportedData.Sections)
				for (const auto& Position : Section.Positions)
				{
					Control.Tick();
					SourceBounds.AddPoint(FVector3(Position));
				}
			const auto Normalization = GetStaticMeshPositionNormalization(SourceBounds, Parameters.NormalizedSize);
			if (!Normalization)
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: invalid bounds (valid {}, min [{}, {}, {}], max [{}, {}, {}], normalized size {}).",
					SourceBounds.bIsValid, SourceBounds.Min.x, SourceBounds.Min.y, SourceBounds.Min.z,
					SourceBounds.Max.x, SourceBounds.Max.y, SourceBounds.Max.z, Parameters.NormalizedSize);
				return false;
			}
			for (FVector3f& Position : Positions)
			{
				Control.Tick();
				Position = (Position - Normalization->Center) * Normalization->Scale;
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
			return true;
		}

	}

	auto FStaticMeshBuilder::Build(FStaticMeshRenderData& OutRenderData,
		const FStaticMeshBuildParameters& Parameters) -> bool
	{
		check(OutRenderData.GetNumInitializedResources() == 0 && OutRenderData.LODVertexFactories.empty());
		FBuildControl Control{Parameters.Control};
		try
		{
			Control.Check();
			if (!Parameters.Geometry)
			{
				Control.Check();
				DURIN_ERROR("StaticMesh render build: requires decoded geometry.");
				return false;
			}
			FBuildLOD Source;
			if (!BuildLODStreams(Parameters, Source, Control)) return false;

			FStaticMeshRenderData Product;
			Product.LocalBounds = Source.LocalBounds;
			for (const auto& Slot : Parameters.MaterialSlots)
				Product.MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex});
			auto& LOD = Product.LODResources.emplace_back();
			auto& Buffers = LOD.VertexBuffers;
			Buffers.PositionVertexBuffer.Init(std::move(Source.Positions));
			Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(std::move(Source.Normals), std::move(Source.Tangents));
			const auto Count = Buffers.PositionVertexBuffer.GetNumVertices();
			Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(Source.TexCoords), Count, Source.NumTexCoords);
			Buffers.ColorVertexBuffer.Init(std::move(Source.Colors), Count);
			LOD.IndexBuffer.Init(std::move(Source.Indices));
			LOD.Sections = std::move(Source.Sections);
			LOD.LocalBounds = Source.LocalBounds;
			LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
			LOD.NumTexCoords = Source.NumTexCoords;
			LOD.bHasColorVertexData = Source.bHasColorVertexData;
			Buffers.Finalize(LOD.NumTexCoords, LOD.bHasColorVertexData);
			Control.Check();
			OutRenderData = std::move(Product);
			return true;
		}
		catch (const FBuildCancelled&)
		{
			return false;
		}
	}
}
