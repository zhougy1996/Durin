#pragma once

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "DObject/Object.h"
#include "Math/Vector.h"

#include "Skeleton.gen.h"

namespace Durin
{
	inline constexpr uint32 SkeletonCompatibilityEncodingVersion = 1;
	inline constexpr uint32 MaximumSkeletonBones = 65535;

	// Stores one canonical float32 matrix without exposing a source-format type.
	DSTRUCT()
	struct FSkeletonTransform
	{
		GENERATED_BODY()

		DPROPERTY()
		FVector4 Row0{1.0, 0.0, 0.0, 0.0};

		DPROPERTY()
		FVector4 Row1{0.0, 1.0, 0.0, 0.0};

		DPROPERTY()
		FVector4 Row2{0.0, 0.0, 1.0, 0.0};

		DPROPERTY()
		FVector4 Row3{0.0, 0.0, 0.0, 1.0};

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;
		ENGINE_API auto CanonicalizeFloat32() -> void;
		ENGINE_API auto ToMatrix4f() const -> FMatrix4f;
		auto operator==(const FSkeletonTransform&) const -> bool = default;
	};

	// Bone array order is the canonical runtime index and always precedes children.
	DSTRUCT()
	struct FSkeletonBone
	{
		GENERATED_BODY()

		DPROPERTY()
		FName Name;

		DPROPERTY()
		int32 ParentIndex = -1;

		DPROPERTY()
		FSkeletonTransform ReferenceTransform;

		auto operator==(const FSkeletonBone&) const -> bool = default;
	};

	DCLASS()
	class DSkeleton : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkeleton(const FObjectInitializer& ObjectInitializer);

		auto GetBones() const -> std::span<const FSkeletonBone> { return Bones; }
		auto GetBoneCount() const -> uint32 { return static_cast<uint32>(Bones.size()); }
		auto GetCompatibilityIdentity() const -> const std::string& { return CompatibilityIdentity; }

		ENGINE_API auto InitializeCanonicalBones(
			std::vector<FSkeletonBone> InBones,
			std::string& OutError) -> bool;
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	public:
		ENGINE_API static auto ComputeCompatibilityIdentity(
			std::span<const FSkeletonBone> InBones,
			std::string& OutIdentity,
			std::string& OutError) -> bool;

	private:
		DPROPERTY()
		std::vector<FSkeletonBone> Bones;

		DPROPERTY()
		std::string CompatibilityIdentity;
	};
}
