#include "InstanceUniformShaderParameters.h"

namespace Durin
{
	namespace
	{
		auto PackUint4(uint32 X, uint32 Y, uint32 Z, uint32 W) -> FVector4f
		{ return {std::bit_cast<float>(X), std::bit_cast<float>(Y), std::bit_cast<float>(Z), std::bit_cast<float>(W)}; }

		auto PackTransform(std::span<FVector4f, 16> Data, const FMatrix& Transform, const FBox& Bounds) -> bool
		{
			FMatrix Inverse;
			if (!Math::IsFinite(Transform) || !Math::TryInverse(Transform, Inverse)
				|| Transform[0][3] != 0.0 || Transform[1][3] != 0.0 || Transform[2][3] != 0.0 || Transform[3][3] != 1.0) return false;
			const FVector3 Origin(Transform[3]);
			FMatrix Relative = Transform;
			Relative[3] = FVector4(0.0, 0.0, 0.0, 1.0);
			const FMatrix RelativeInverse = Inverse * Math::TranslationMatrix(Origin);
			const auto LocalToWorld = Math::TransposeToFloat(Relative);
			const auto WorldToLocal = Math::TransposeToFloat(RelativeInverse);
			// Normal matrix is the inverse transpose of the translation-free matrix.
			FMatrix4f NormalToWorld(1.f);
			for (uint32 Row = 0; Row < 3; ++Row)
				for (uint32 Column = 0; Column < 3; ++Column) NormalToWorld[Row][Column] = WorldToLocal[Column][Row];
			for (uint32 Row = 0; Row < 4; ++Row)
			{
				Data[Row] = LocalToWorld[Row]; Data[4 + Row] = WorldToLocal[Row]; Data[8 + Row] = NormalToWorld[Row];
			}
			const FVector3f High(Origin);
			Data[12] = FVector4f(High, 0.f);
			Data[13] = FVector4f(FVector3f(Origin - FVector3(High)), 0.f);
			const bool ValidBounds = Bounds.bIsValid && Math::IsFinite(Bounds.Min) && Math::IsFinite(Bounds.Max);
			Data[14] = FVector4f(ValidBounds ? FVector3f(Bounds.Min - Origin) : FVector3f(0.f), ValidBounds ? 1.f : 0.f);
			Data[15] = FVector4f(ValidBounds ? FVector3f(Bounds.Max - Origin) : FVector3f(0.f), Math::LinearDeterminant(Transform) < 0.0 ? -1.f : 1.f);
			return std::ranges::all_of(Data, [](const auto& Value) { return Math::IsFinite(Value); });
		}
	}

	auto FPrimitiveUniformShaderParametersBuilder::Build() const -> std::optional<FPrimitiveUniformShaderParameters>
	{
		std::array<FVector4f, 16> Packed;
		if (!Parameters.CustomPrimitiveData.IsValid() || !PackTransform(Packed, Parameters.LocalToWorld, Parameters.WorldBounds)) return {};
		auto Result = Parameters;
		Result.Flags = 1;
		return Result;
	}

	FPrimitiveSceneShaderData::FPrimitiveSceneShaderData(const FPrimitiveUniformShaderParameters& Parameters)
	{
		if (!PackTransform(std::span<FVector4f, 16>(Data.data(), 16), Parameters.LocalToWorld, Parameters.WorldBounds)) return;
		Data[HeaderOffset] = PackUint4(Parameters.InstanceSceneDataOffset, Parameters.NumInstanceSceneDataEntries, Parameters.Flags, 0);
		for (uint32 Index = 0; Index < PrimitiveCustomDataFloatCount / 4; ++Index)
			for (uint32 Channel = 0; Channel < 4; ++Channel) Data[CustomDataOffset + Index][Channel] = Parameters.CustomPrimitiveData.Values[Index * 4 + Channel];
	}

	auto FInstanceSceneShaderData::Build(uint32 PrimitiveId, uint32 InstanceRelativeId,
		uint32 InstancePayloadDataOffset, FPrimitiveInstanceId StableId,
		const FMatrix& LocalToWorld, const FBox& WorldBounds) -> bool
	{
		Data = {};
		if (!PackTransform(std::span<FVector4f, 16>(Data.data(), 16), LocalToWorld, WorldBounds)) { Data = {}; return false; }
		Data[HeaderOffset] = PackUint4(PrimitiveId, InstanceRelativeId, 1, InstancePayloadDataOffset);
		Data[PayloadOffset] = PackUint4(PrimitiveCustomDataFloatCount, StableId.Id, 0, 0);
		return true;
	}
}
