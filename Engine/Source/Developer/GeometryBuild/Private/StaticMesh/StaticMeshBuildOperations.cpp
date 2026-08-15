#include "StaticMesh/StaticMeshBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "DObject/DObjectGlobals.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuildDerivedData.h"
#include "StaticMesh/StaticMeshDerivedData.h"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace Durin::Asset::Build
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

		const FBuildFunctionIdentity StaticMeshFunctionIdentity{
			"Durin.GeometryBuild.StaticMesh", 1};
		const FBuildFunctionIdentity StaticMeshCollisionFunctionIdentity{
			"Durin.GeometryBuild.StaticMeshCollision", 1};
		constexpr std::string_view StaticMeshInputName = "StaticMeshBuildInput";
		constexpr std::string_view StaticMeshValueName = "StaticMeshPayload";
		constexpr std::string_view CollisionInputName = "StaticMeshCollisionBuildInput";
		constexpr std::string_view CollisionValueName = "StaticMeshCollisionPayload";

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

		auto EncodeRenderData(
			const FStaticMeshRenderData& RenderData, FBuildValue& OutValue,
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
			OutValue = FBuildValue::FromOwned(std::string(StaticMeshValueName), std::move(Bytes));
			return true;
		}

		auto DecodeRenderData(const FBuildValue& Value,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message : "StaticMesh payload has trailing bytes.";
				return false;
			}
			return MakeStaticMeshRenderData(Payload, OutRenderData, OutError);
		}

		auto AppendU32(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte) Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto AppendU64(std::vector<uint8>& Bytes, uint64 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 8; ++Byte) Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto ReadU32(std::span<const uint8> Bytes, size_t& Offset, uint32& Value) -> bool
		{
			if (Offset + 4 > Bytes.size()) return false;
			Value = 0; for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= uint32(Bytes[Offset++]) << (Byte * 8);
			return true;
		}
		auto ReadU64(std::span<const uint8> Bytes, size_t& Offset, uint64& Value) -> bool
		{
			if (Offset + 8 > Bytes.size()) return false;
			Value = 0; for (uint32 Byte = 0; Byte < 8; ++Byte) Value |= uint64(Bytes[Offset++]) << (Byte * 8);
			return true;
		}

		auto EncodeCollisionInput(std::span<const FVector3f> Positions,
			std::span<const uint32> Indices, EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			AppendU32(Bytes, static_cast<uint32>(Mode));
			AppendU32(Bytes, static_cast<uint32>(Policy));
			AppendU64(Bytes, Positions.size());
			for (const FVector3f& Position : Positions)
				for (uint32 Axis = 0; Axis < 3; ++Axis) AppendU32(Bytes, std::bit_cast<uint32>(Position[Axis]));
			AppendU64(Bytes, Indices.size());
			for (uint32 Index : Indices) AppendU32(Bytes, Index);
			return Bytes;
		}

		auto DecodeCollisionInput(std::span<const uint8> Bytes,
			std::vector<FVector3>& Positions, std::vector<uint32>& Indices,
			EBodySetupCollisionSourceMode& Mode, EBodySetupCollisionQueryPolicy& Policy,
			std::string& OutError) -> bool
		{
			size_t Offset = 0; uint32 ModeValue = 0, PolicyValue = 0; uint64 Count = 0;
			if (!ReadU32(Bytes, Offset, ModeValue) || !ReadU32(Bytes, Offset, PolicyValue)
				|| !ReadU64(Bytes, Offset, Count) || Count > (Bytes.size() - Offset) / 12) goto Invalid;
			Mode = static_cast<EBodySetupCollisionSourceMode>(ModeValue);
			Policy = static_cast<EBodySetupCollisionQueryPolicy>(PolicyValue);
			Positions.reserve(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 X = 0, Y = 0, Z = 0;
				if (!ReadU32(Bytes, Offset, X) || !ReadU32(Bytes, Offset, Y) || !ReadU32(Bytes, Offset, Z)) goto Invalid;
				Positions.emplace_back(std::bit_cast<float>(X), std::bit_cast<float>(Y), std::bit_cast<float>(Z));
			}
			if (!ReadU64(Bytes, Offset, Count) || Count > (Bytes.size() - Offset) / 4) goto Invalid;
			Indices.reserve(Count);
			for (uint64 Index = 0; Index < Count; ++Index) { uint32 Value = 0; if (!ReadU32(Bytes, Offset, Value)) goto Invalid; Indices.push_back(Value); }
			if (Offset == Bytes.size() && !Positions.empty() && !Indices.empty() && Indices.size() % 3 == 0) return true;
		Invalid:
			OutError = "StaticMesh collision local build input is malformed.";
			return false;
		}

		auto DecodeCollisionValue(const FBuildValue& Value,
			EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
			FCollisionGeometryRef& OutGeometry, std::string& OutError) -> bool
		{
			FStaticMeshCollisionPayloadData Payload;
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (Ar.HasError() || !RequireArchiveEnd(Ar) || Payload.SourceMode != Mode)
			{ OutError = Ar.GetError().empty() ? "StaticMesh collision payload is incompatible." : Ar.GetError(); return false; }
			Payload.QueryPolicy = Policy;
			return MakeStaticMeshCollisionGeometry(Payload, OutGeometry, OutError);
		}

		class FStaticMeshBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override { return {
				.CacheRoot = "StaticMesh/Objects", .ExpectedValueName = std::string(StaticMeshValueName),
				.MaximumValueBytes = MaximumStaticMeshPayloadBytes,
				.CleanupBudgetBytes = StaticMeshDerivedDataBudgetBytes,
				.CleanupDeleteLimit = StaticMeshDerivedDataCleanupDeleteLimit}; }
			auto Validate(const FBuildDefinition&, const FBuildValue& Value, std::string& Error) const -> bool override
			{ std::unique_ptr<FStaticMeshRenderData> Data; return DecodeRenderData(Value, Data, Error); }
			auto Build(const FBuildContext& Context, FBuildValue& Value, std::string& Error) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(StaticMeshInputName);
				if (!Input) { Error = "StaticMesh build input is missing."; return false; }
				Value = FBuildValue::FromOwned(std::string(StaticMeshValueName),
					std::vector<uint8>(Input->GetBytes().begin(), Input->GetBytes().end()));
				return true;
			}
		};

		class FStaticMeshCollisionBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override { return {
				.CacheRoot = "StaticMeshCollision/Objects", .ExpectedValueName = std::string(CollisionValueName),
				.MaximumValueBytes = MaximumStaticMeshCollisionPayloadBytes}; }
			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value, std::string& Error) const -> bool override
			{
				const auto ModeFact = Definition.GetTargetFact("Mode");
				const auto PolicyFact = Definition.GetTargetFact("Policy");
				if (!ModeFact || !PolicyFact) { Error = "StaticMesh collision target facts are missing."; return false; }
				FCollisionGeometryRef Geometry;
				return DecodeCollisionValue(Value,
					static_cast<EBodySetupCollisionSourceMode>(std::stoul(std::string(*ModeFact))),
					static_cast<EBodySetupCollisionQueryPolicy>(std::stoul(std::string(*PolicyFact))), Geometry, Error);
			}
			auto Build(const FBuildContext& Context, FBuildValue& Value, std::string& Error) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(CollisionInputName);
				std::vector<FVector3> Positions; std::vector<uint32> Indices;
				EBodySetupCollisionSourceMode Mode; EBodySetupCollisionQueryPolicy Policy;
				if (!Input || !DecodeCollisionInput(Input->GetBytes(), Positions, Indices, Mode, Policy, Error)) return false;
				FCollisionGeometryBuildDiagnostics Facts;
				FCollisionGeometryRef Geometry = Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? FCollisionGeometryRef::BuildConvexHull(Positions, &Facts)
					: FCollisionGeometryRef::BuildTriangleMesh(Positions, Indices, &Facts);
				if (!Geometry) { Error = std::format("StaticMesh collision build failed with status {}.", static_cast<uint32>(Facts.Status)); return false; }
				FStaticMeshCollisionPayloadData Payload;
				if (!MakeStaticMeshCollisionPayloadData(Geometry, Policy, Payload, Error)) return false;
				std::vector<uint8> Bytes;
				FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
				Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
				if (Ar.HasError()) { Error = Ar.GetError(); return false; }
				Value = FBuildValue::FromOwned(std::string(CollisionValueName), std::move(Bytes));
				return true;
			}
		};
		std::mutex GStaticMeshFunctionMutex;
		FBuildFunctionRegistration GStaticMeshFunctionRegistration;
		FBuildFunctionRegistration GStaticMeshCollisionFunctionRegistration;

		auto EnsureStaticMeshBuildFunctions(std::string* OutError,
			FModuleOwnedCallbackGate Gate = {}) -> bool
		{
			std::lock_guard Lock(GStaticMeshFunctionMutex);
			if (GStaticMeshFunctionRegistration.IsValid()
				&& GStaticMeshCollisionFunctionRegistration.IsValid()) return true;
			GStaticMeshFunctionRegistration = RegisterBuildFunction(StaticMeshFunctionIdentity,
				std::make_shared<FStaticMeshBuildFunction>(), Gate, OutError);
			if (!GStaticMeshFunctionRegistration.IsValid()) return false;
			GStaticMeshCollisionFunctionRegistration = RegisterBuildFunction(
				StaticMeshCollisionFunctionIdentity,
				std::make_shared<FStaticMeshCollisionBuildFunction>(), std::move(Gate), OutError);
			if (!GStaticMeshCollisionFunctionRegistration.IsValid())
			{
				GStaticMeshFunctionRegistration.Reset();
				return false;
			}
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

	auto InitializeStaticMeshBuildFunctions(FModuleOwnedCallbackGate Gate,
		std::string* OutError) -> bool
	{
		return EnsureStaticMeshBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownStaticMeshBuildFunctions() -> void
	{
		std::lock_guard Lock(GStaticMeshFunctionMutex);
		GStaticMeshCollisionFunctionRegistration.Reset();
		GStaticMeshFunctionRegistration.Reset();
	}

	auto FStaticMeshBuildOperations::CaptureReconciliationSnapshot(
		const DStaticMesh& Mesh) -> FStaticMeshReconciliationSnapshot
	{
		CheckGameThread();
		return {
			.MaterialSlots = std::vector<FStaticMeshMaterialSlotDefinition>(
				Mesh.MaterialSlots.begin(), Mesh.MaterialSlots.end()),
			.NormalizedSize = Mesh.NormalizedSize,
			.StableObjectPath = Mesh.GetObjectPath(),
			.Provenance = Mesh.SourceImportData,
			.ImportSettings = Mesh.SourceImportData.ImportSettings};
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
				CaptureReconciliationSnapshot(Mesh), ImportedData,
				std::move(SourceImportData), SourceLabel,
				Product, OutError)
			&& PublishImportedProduct(Mesh, std::move(Product), OutError);
	}

	auto FStaticMeshBuildOperations::BuildImportedProduct(
		const FStaticMeshReconciliationSnapshot& Reconciliation,
		const FStaticMeshImportedData& ImportedData,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceLabel,
		FStaticMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!EnsureStaticMeshBuildFunctions(&OutError)) return false;
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
			Reconciliation.MaterialSlots,
			Reconciliation.NormalizedSize,
			Reconciliation.StableObjectPath,
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
		FBuildValue CandidateValue;
		if (!EncodeRenderData(*Product.RenderData, CandidateValue, OutError))
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::DerivedDataWrite;
			return false;
		}
		const std::vector<uint8> KeyBytes = BuildStaticMeshDerivedDataKeyBytes(KeyInput, OutError);
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(StaticMeshFunctionIdentity, std::string(StaticMeshValueName));
		Builder.SetKey(FBuildKey::FromString(Product.DerivedDataKey), KeyBytes)
			.AddTargetFact("Platform", "Win64")
			.AddInput(FBuildValue::FromOwned(std::string(StaticMeshInputName),
				std::vector<uint8>(CandidateValue.GetBytes().begin(), CandidateValue.GetBytes().end())));
		if (!Builder.Build(Definition, &OutError))
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::Key;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = true,
			.bStoreBuildResult = true, .bRequireStoreSuccess = true});
		if (!Output.Succeeded())
		{
			Product.FailureStage = Output.FailurePhase == EBuildFailurePhase::CacheStore
				? EStaticMeshAuthoringFailureStage::DerivedDataWrite
				: EStaticMeshAuthoringFailureStage::RenderConversion;
			OutError = Output.Diagnostic;
			return false;
		}
		std::unique_ptr<FStaticMeshRenderData> SelectedRenderData;
		if (!DecodeRenderData(Output.Value, SelectedRenderData, OutError)
			|| !RestoreRuntimeMetadata(Product.MaterialSlots, *SelectedRenderData, OutError))
		{
			Product.FailureStage = EStaticMeshAuthoringFailureStage::RenderConversion;
			return false;
		}
		Product.RenderData = std::move(SelectedRenderData);

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
		const FStaticMeshReconciliationSnapshot& Reconciliation,
		FStaticMeshSourceImportData SourceImportData,
		bool bSourceAvailable,
		FStaticMeshBuildProduct& OutProduct,
		EStaticMeshDerivedDataStatus& OutStatus,
		std::string& OutMessage,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!EnsureStaticMeshBuildFunctions(&OutError))
		{
			OutStatus = EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = OutError;
			return false;
		}
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
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(StaticMeshFunctionIdentity, std::string(StaticMeshValueName));
		Builder.SetKey(FBuildKey::FromString(Key)).AddTargetFact("Platform", "Win64");
		if (!Builder.Build(Definition, &OutError))
		{
			OutStatus = EStaticMeshDerivedDataStatus::Incompatible;
			OutMessage = OutError;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false, .bReturnData = true});
		if (!Output.Succeeded())
		{
			OutStatus = Output.Status == EBuildStatus::CacheMiss
				? EStaticMeshDerivedDataStatus::Missing : EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = Output.Diagnostic;
			OutError = OutMessage;
			return false;
		}
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		if (!DecodeRenderData(Output.Value, RenderData, OutError)
			|| !RestoreRuntimeMetadata(Reconciliation.MaterialSlots, *RenderData, OutError))
		{
			OutStatus = EStaticMeshDerivedDataStatus::Corrupt;
			OutMessage = OutError;
			return false;
		}
		OutProduct.RenderData = std::move(RenderData);
		OutProduct.MaterialSlots.assign(
			Reconciliation.MaterialSlots.begin(), Reconciliation.MaterialSlots.end());
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
		if (!EnsureStaticMeshBuildFunctions(&OutError)) return false;
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

		const std::vector<uint8> KeyBytes =
			BuildStaticMeshCollisionDerivedDataKeyBytes(KeyInput, OutError);
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(
			StaticMeshCollisionFunctionIdentity, std::string(CollisionValueName));
		Builder.SetKey(FBuildKey::FromString(OutProduct.DerivedDataKey), KeyBytes)
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Mode", std::to_string(static_cast<uint32>(Mode)))
			.AddTargetFact("Policy", std::to_string(static_cast<uint32>(Policy)))
			.AddInput(FBuildValue::FromOwned(std::string(CollisionInputName),
				EncodeCollisionInput(Positions, Indices, Mode, Policy)));
		if (!Builder.Build(Definition, &OutError)) return false;
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = true,
			.bStoreBuildResult = true, .bRequireStoreSuccess = true});
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			return false;
		}
		FCollisionGeometryRef Geometry;
		if (!DecodeCollisionValue(Output.Value, Mode, Policy, Geometry, OutError)) return false;
		if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
			OutProduct.Simple = Geometry;
		else OutProduct.Complex = Geometry;
		OutProduct.Status = Output.Status == EBuildStatus::CacheHit
			? EBodySetupCollisionBuildStatus::CacheHit
			: EBodySetupCollisionBuildStatus::Rebuilt;
		OutProduct.PayloadBytes = Output.Value.GetSize();
		OutProduct.Diagnostic = Output.Status == EBuildStatus::CacheHit
			? std::format("StaticMesh collision DDC hit for key {}.", OutProduct.DerivedDataKey)
			: std::format("Rebuilt StaticMesh collision key {} ({} bytes).",
				OutProduct.DerivedDataKey, Geometry.GetRetainedBytes());
		OutError.clear();
		return true;
	}
}
