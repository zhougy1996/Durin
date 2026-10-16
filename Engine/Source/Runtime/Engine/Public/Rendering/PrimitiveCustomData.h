#pragma once

#include "CoreMinimal.h"

#include "Math/Box.h"
#include "Math/Operations.h"

namespace Durin
{
	inline constexpr uint32 PrimitiveCustomDataFloatCount = 32;
	inline constexpr uint32 MaximumStaticMeshRenderInstances = 4096;

	struct FPrimitiveCustomData
	{
		std::array<float, PrimitiveCustomDataFloatCount> Values{};
		auto Set(uint32 First, std::span<const float> Input) -> bool
		{
			if (Input.empty() || First >= Values.size() || Input.size() > Values.size() - First
				|| std::ranges::any_of(Input, [](float Value) { return !std::isfinite(Value); })) return false;
			auto Candidate = Values;
			for (size_t Index = 0; Index < Input.size(); ++Index)
				Candidate[First + Index] = Input[Index] == 0.f ? 0.f : Input[Index];
			Values = Candidate;
			return true;
		}
		auto IsValid() const -> bool
		{
			return std::ranges::all_of(Values, [](float Value) { return std::isfinite(Value); });
		}
		auto operator==(const FPrimitiveCustomData&) const -> bool = default;
	};
	static_assert(sizeof(FPrimitiveCustomData) == 128);

	struct FStaticMeshRenderInstance
	{
		uint32 Id = 0;
		FMatrix LocalToPrimitive{1.0};
		FPrimitiveCustomData CustomData;
		auto operator==(const FStaticMeshRenderInstance&) const -> bool = default;
	};
	using FStaticMeshRenderInstances = std::shared_ptr<const std::vector<FStaticMeshRenderInstance>>;

	inline auto ValidateStaticMeshRenderInstances(std::span<const FStaticMeshRenderInstance> Instances) -> bool
	{
		if (Instances.size() > MaximumStaticMeshRenderInstances) return false;
		std::unordered_set<uint32> Ids;
		for (const auto& Instance : Instances)
		{
			FMatrix Inverse;
			const auto& Transform = Instance.LocalToPrimitive;
			if (Instance.Id == 0 || !Ids.insert(Instance.Id).second || !Instance.CustomData.IsValid()
				|| !Math::IsFinite(Transform) || !Math::TryInverse(Transform, Inverse)
				|| Transform[0][3] != 0.0 || Transform[1][3] != 0.0 || Transform[2][3] != 0.0
				|| Transform[3][3] != 1.0) return false;
		}
		return true;
	}

	inline auto TransformPrimitiveBounds(const FBox& Bounds, const FMatrix& Transform) -> FBox
	{
		FBox Result;
		if (!Bounds.bIsValid) return Result;
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 Point((Corner & 1) ? Bounds.Max.x : Bounds.Min.x,
				(Corner & 2) ? Bounds.Max.y : Bounds.Min.y, (Corner & 4) ? Bounds.Max.z : Bounds.Min.z);
			Result.AddPoint(FVector3(Transform * FVector4(Point, 1.0)));
		}
		return Result;
	}
}
