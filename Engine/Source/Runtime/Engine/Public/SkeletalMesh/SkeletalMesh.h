#pragma once

#include "CookedAsset.h"
#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Math/Box.h"
#include "SkeletalMesh/Skeleton.h"

#include "SkeletalMesh.gen.h"

namespace Durin
{
	class DMaterialInterface;

	inline constexpr uint32 MaximumSkeletalMeshMaterialSlots = 4'096;
	inline constexpr uint32 MaximumSkeletalMeshSections = 65'536;
	inline constexpr uint32 MaximumSkeletalMeshVertices = 100'000'000;
	inline constexpr uint32 MaximumSkeletalMeshIndices = 300'000'000;
	inline constexpr uint32 MaximumSkeletalMeshUVChannels = 4;
	inline constexpr uint32 MaximumSkeletalMeshInfluences = 4;
	inline constexpr uint64 MaximumSkeletalMeshPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;

	DSTRUCT()
	struct FSkeletalMeshBounds
	{
		GENERATED_BODY()

		DPROPERTY()
		FVector3 Minimum{0.0};

		DPROPERTY()
		FVector3 Maximum{0.0};

		DPROPERTY()
		bool bIsValid = false;

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;
		ENGINE_API auto ToBox() const -> FBox;
		ENGINE_API static auto FromBox(const FBox& Box) -> FSkeletalMeshBounds;
		auto operator==(const FSkeletalMeshBounds&) const -> bool = default;
	};

	DSTRUCT()
	struct FSkeletalMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

		DPROPERTY()
		FName Name;

		DPROPERTY()
		std::string SourceName;

		DPROPERTY()
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> DefaultMaterial;

		auto operator==(const FSkeletalMeshMaterialSlotDefinition&) const -> bool = default;
	};

	DSTRUCT()
	struct FSkeletalMeshSummary
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 VertexCount = 0;

		DPROPERTY()
		uint32 IndexCount = 0;

		DPROPERTY()
		uint32 SectionCount = 0;

		DPROPERTY()
		FSkeletalMeshBounds LocalBounds;

		auto operator==(const FSkeletalMeshSummary&) const -> bool = default;
	};

	struct FSkeletalMeshVertexInfluences
	{
		std::array<uint16, MaximumSkeletalMeshInfluences> BoneIndices{};
		std::array<float, MaximumSkeletalMeshInfluences> Weights{};
		uint8 Count = 0;

		auto operator==(const FSkeletalMeshVertexInfluences&) const -> bool = default;
	};

	struct FSkeletalMeshSection
	{
		FName Name;
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;

		auto operator==(const FSkeletalMeshSection& Other) const -> bool
		{
			return Name == Other.Name && FirstIndex == Other.FirstIndex
				&& IndexCount == Other.IndexCount && MinVertexIndex == Other.MinVertexIndex
				&& MaxVertexIndex == Other.MaxVertexIndex
				&& MaterialSlotIndex == Other.MaterialSlotIndex
				&& LocalBounds.bIsValid == Other.LocalBounds.bIsValid
				&& LocalBounds.Min == Other.LocalBounds.Min
				&& LocalBounds.Max == Other.LocalBounds.Max;
		}
	};

	// Detached immutable CPU data. It contains no source tokens, DObjects, or RHI state.
	struct FSkeletalMeshPayloadData
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaximumSkeletalMeshUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		std::vector<FSkeletalMeshVertexInfluences> Influences;
		std::vector<FSkeletalMeshSection> Sections;
		std::vector<uint16> PaletteBoneIndices;
		std::vector<FMatrix4f> InverseBindMatrices;
		FBox LocalBounds;

		auto operator==(const FSkeletalMeshPayloadData& Other) const -> bool
		{
			return Positions == Other.Positions && Normals == Other.Normals
				&& Tangents == Other.Tangents && UVChannels == Other.UVChannels
				&& Colors == Other.Colors && Indices == Other.Indices
				&& Influences == Other.Influences && Sections == Other.Sections
				&& PaletteBoneIndices == Other.PaletteBoneIndices
				&& InverseBindMatrices == Other.InverseBindMatrices
				&& LocalBounds.bIsValid == Other.LocalBounds.bIsValid
				&& LocalBounds.Min == Other.LocalBounds.Min
				&& LocalBounds.Max == Other.LocalBounds.Max;
		}
	};

	struct FSkeletalMeshImportedData
	{
		DSkeleton* Skeleton = nullptr;
		std::string SkeletonCompatibilityIdentity;
		FSkeletonTransform MeshNodeBindTransform;
		std::vector<FSkeletalMeshMaterialSlotDefinition> MaterialSlots;
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		Asset::FCookedPayloadDescriptor CookedPayload;
	};

	ENGINE_API auto ValidateSkeletalMeshPayload(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		std::string& OutError) -> bool;

	class FSkeletalMeshImportedStateExchange;

	DCLASS()
	class DSkeletalMesh : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkeletalMesh(const FObjectInitializer& ObjectInitializer);

		auto GetSkeleton() const -> DSkeleton* { return Skeleton.Get(); }
		auto GetSkeletonCompatibilityIdentity() const -> const std::string& { return SkeletonCompatibilityIdentity; }
		auto GetMeshNodeBindTransform() const -> const FSkeletonTransform& { return MeshNodeBindTransform; }
		auto GetMaterialSlots() const -> std::span<const FSkeletalMeshMaterialSlotDefinition> { return MaterialSlots; }
		auto GetSummary() const -> const FSkeletalMeshSummary& { return Summary; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto GetPayloadData() const -> std::shared_ptr<const FSkeletalMeshPayloadData> { return PayloadData; }

		ENGINE_API auto InitializeFromImportedData(
			FSkeletalMeshImportedData InData,
			std::string& OutError) -> bool;
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PrepareImportedStateExchange(
			DSkeletalMesh& Candidate,
			std::string& OutError) -> std::unique_ptr<FSkeletalMeshImportedStateExchange>;

	private:
		DPROPERTY()
		TObjectPtr<DSkeleton> Skeleton;

		DPROPERTY()
		std::string SkeletonCompatibilityIdentity;

		DPROPERTY()
		FSkeletonTransform MeshNodeBindTransform;

		DPROPERTY()
		std::vector<FSkeletalMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		FSkeletalMeshSummary Summary;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::shared_ptr<const FSkeletalMeshPayloadData> PayloadData;

		friend class FSkeletalMeshImportedStateExchange;
	};

	class ENGINE_API FSkeletalMeshImportedStateExchange
	{
	public:
		~FSkeletalMeshImportedStateExchange();
		FSkeletalMeshImportedStateExchange(const FSkeletalMeshImportedStateExchange&) = delete;
		auto operator=(const FSkeletalMeshImportedStateExchange&)
			-> FSkeletalMeshImportedStateExchange& = delete;

		auto Commit() noexcept -> void;
		auto Reverse() noexcept -> void;
		auto Finalize() noexcept -> void;

	private:
		FSkeletalMeshImportedStateExchange(DSkeletalMesh& InTarget, DSkeletalMesh& InCandidate);
		auto Swap() noexcept -> void;

		DSkeletalMesh* Target = nullptr;
		DSkeletalMesh* Candidate = nullptr;
		bool bCommitted = false;

		friend class DSkeletalMesh;
	};
}
