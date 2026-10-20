#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"

#include "DObject/Object.h"

namespace Durin
{
	auto PrepareTransactionAnnotationReplacement(const std::shared_ptr<ITransactionObjectAnnotation>& Source,
		const FObjectReplacementMap& Map) -> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
	{
		if (!Source) return Source;
		class FCheck final : public FReferenceCollector
		{
		public:
			explicit FCheck(const FObjectReplacementMap& InMap) : Map(InMap) {}
			auto AddReferencedObject(DObject*& Object) -> void override { bMapped |= Map.Find(Object) != nullptr; }
			const FObjectReplacementMap& Map;
			bool bMapped = false;
		} Before(Map), After(Map);
		Source->AddReferencedObjects(Before);
		if (!Before.bMapped) return Source;
		auto Result = Source->CreateReplacement(Map);
		if (!Result) return Result;
		if (!*Result || Result->get() == Source.get())
			return std::unexpected("Annotation reinstancing must return an independent snapshot.");
		(*Result)->AddReferencedObjects(After);
		if (After.bMapped) return std::unexpected("Annotation reinstancing left old-generation dependencies.");
		if (auto Valid = (*Result)->Validate(); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto ITransactionObjectAnnotation::CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		return Object.FindOrCreateTransactionAnnotation();
	}
}
