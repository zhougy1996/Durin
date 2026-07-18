#pragma once

namespace Durin
{
	class DStructBase;
	class FReferenceCollector;

	namespace Private
	{
		class FGCReferenceSchema;

		// Reflection construction is the only writer. GC only observes the immutable
		// schema after the owning reflected type has finished attaching properties.
		class FGCReferenceSchemaRegistry
		{
		public:
			static auto Assemble(DStructBase* Type) -> void;
			static auto FinalizeAndAssemble(DStructBase* Type) -> void;
			static auto HasReferences(const DStructBase* Type) -> bool;
			static auto Visit(const DStructBase* Type, void* Instance, FReferenceCollector& Collector) -> void;
		};
	}
}
