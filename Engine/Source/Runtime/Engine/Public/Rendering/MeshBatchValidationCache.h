#pragma once

#include "CoreMinimal.h"

#include "Math/Operations.h"
#include "Rendering/PrimitiveComponentId.h"
#include "Rendering/PrimitiveCustomData.h"

namespace Durin
{
	// Owned by one render preparation command, used serially during collection.
	// Exact current values are checked even when the primitive/batch identity matches.
	class FMeshBatchValidationCache
	{
	public:
		struct FTransform
		{
			FMatrix LocalToWorld{1.0};
			FMatrix4f NormalToWorld{1.0f};
			FMatrix WorldToLocal{1.0};
			double Determinant = 0.0;
			// Admission accepts an invertible double matrix; rendering additionally
			// needs a finite determinant and a representable normal matrix.
			bool bAdmissible = false;
			bool bValid = false;
		};

		auto ValidateValues(FPrimitiveComponentId PrimitiveId, uint64 BatchId, const FBox& Bounds,
			const FPrimitiveCustomData& PrimitiveData, const FPrimitiveCustomData& InstanceData) -> bool
		{
			auto& Entry = Entries[{PrimitiveId.Value, BatchId}];
			if (Entry.bHasValues && Entry.Bounds.bIsValid == Bounds.bIsValid && Entry.Bounds.Min == Bounds.Min
				&& Entry.Bounds.Max == Bounds.Max
				&& std::memcmp(Entry.PrimitiveData.Values.data(), PrimitiveData.Values.data(),
					sizeof(PrimitiveData.Values)) == 0
				&& std::memcmp(Entry.InstanceData.Values.data(), InstanceData.Values.data(),
					sizeof(InstanceData.Values)) == 0)
				return Entry.bValuesValid;
			Entry.bHasValues = true;
			Entry.Bounds = Bounds;
			Entry.PrimitiveData = PrimitiveData;
			Entry.InstanceData = InstanceData;
			Entry.bValuesValid = PrimitiveData.IsValid() && InstanceData.IsValid() && Bounds.bIsValid
				&& Math::IsFinite(Bounds.Min) && Math::IsFinite(Bounds.Max) && Bounds.Min.x <= Bounds.Max.x
				&& Bounds.Min.y <= Bounds.Max.y && Bounds.Min.z <= Bounds.Max.z;
			return Entry.bValuesValid;
		}

		auto ResolveTransform(FPrimitiveComponentId PrimitiveId, uint64 BatchId, const FMatrix& LocalToWorld)
			-> const FTransform&
		{
			auto& Entry = Entries[{PrimitiveId.Value, BatchId}];
			auto& Fact = Entry.Transform;
			if (Entry.bHasTransform && Fact.LocalToWorld == LocalToWorld) return Fact;
			Entry.bHasTransform = true;
			++TransformBuilds;
			Fact = FTransform{};
			Fact.LocalToWorld = LocalToWorld;
			if (!Math::IsFinite(LocalToWorld) || !Math::TryInverse(LocalToWorld, Fact.WorldToLocal)) return Fact;
			Fact.bAdmissible = true;
			Fact.Determinant = Math::LinearDeterminant(LocalToWorld);
			Fact.NormalToWorld = Math::TransposeToFloat(Math::Transpose(Fact.WorldToLocal));
			Fact.bValid = std::isfinite(Fact.Determinant) && Math::IsFinite(FMatrix(Fact.NormalToWorld))
				&& Math::IsFinite(FMatrix(Fact.WorldToLocal));
			return Fact;
		}
		auto GetTransformBuilds() const -> size_t { return TransformBuilds; }

	private:
		struct FEntry
		{
			FTransform Transform;
			FBox Bounds;
			FPrimitiveCustomData PrimitiveData;
			FPrimitiveCustomData InstanceData;
			bool bHasTransform = false;
			bool bHasValues = false;
			bool bValuesValid = false;
		};
		std::map<std::pair<uint64, uint64>, FEntry> Entries;
		size_t TransformBuilds = 0;
	};
}
