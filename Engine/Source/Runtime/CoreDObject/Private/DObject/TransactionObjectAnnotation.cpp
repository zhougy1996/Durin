#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"

#include "DObject/Object.h"

namespace Durin
{
	auto ReinstanceTransactionAnnotation(const std::shared_ptr<ITransactionObjectAnnotation>& Source,
		const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		if (!Source) return {};
		class FCheck final : public FReferenceCollector
		{
		public:
			explicit FCheck(const FObjectReplacementMap& InMap) : Map(InMap) {}
			auto AddReferencedObject(DObject*& Object) -> void override { bMapped |= Map.Find(Object) != nullptr; }
			const FObjectReplacementMap& Map;
			bool bMapped = false;
		} Before(Map), After(Map);
		Source->AddReferencedObjects(Before);
		if (!Before.bMapped) return {};
		if (auto Result = Source->FindAndReplaceInstances(Map); !Result) return Result;
		Source->AddReferencedObjects(After);
		if (After.bMapped) return std::unexpected("Annotation retained a replaced object.");
		return Source->Validate();
	}

	auto ITransactionObjectAnnotation::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		FReinstancingReferenceCollector Collector(Map);
		AddReferencedObjects(Collector);
		return {};
	}

	auto ITransactionObjectAnnotation::CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		return Object.FindOrCreateTransactionAnnotation();
	}
}
