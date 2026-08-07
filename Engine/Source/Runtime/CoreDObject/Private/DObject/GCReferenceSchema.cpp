#include "GCReferenceSchema.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Private
{
	namespace
	{
		enum class EGCReferenceOperation : uint8
		{
			Object,
			Struct,
			Array,
			Map
		};

		struct FGCReferenceOperation
		{
			EGCReferenceOperation Kind;
			FProperty* Property = nullptr;
			std::shared_ptr<const FGCReferenceOperation> Inner;
			std::shared_ptr<const FGCReferenceOperation> Key;
			std::shared_ptr<const FGCReferenceOperation> Value;
		};

		thread_local std::unordered_set<const DStructBase*> GAssemblingSchemas;
		std::vector<DStructBase*> GAssembledTypes;

		class FSchemaAssemblyScope
		{
		public:
			explicit FSchemaAssemblyScope(const DStructBase* InType)
				: Type(InType)
			{
				check(Type);
				check(!GAssemblingSchemas.contains(Type) && "Recursive reflected value layouts cannot assemble a GC reference schema.");
				GAssemblingSchemas.insert(Type);
			}

			~FSchemaAssemblyScope()
			{
				GAssemblingSchemas.erase(Type);
			}

		private:
			const DStructBase* Type;
		};

		auto CompileProperty(FProperty* Property) -> std::shared_ptr<const FGCReferenceOperation>;
		auto VisitOperation(const FGCReferenceOperation& Operation, void* Container, FReferenceCollector& Collector) -> void;

		struct FArrayReferenceVisitContext
		{
			const FGCReferenceOperation& Inner;
			FReferenceCollector& Collector;
		};

		auto VisitArrayReference(void* RawContext, uint64, const void* Element) -> bool
		{
			auto& Context = *static_cast<FArrayReferenceVisitContext*>(RawContext);
			VisitOperation(Context.Inner, const_cast<void*>(Element), Context.Collector);
			return true;
		}

		struct FMapReferenceVisitContext
		{
			const FGCReferenceOperation& Operation;
			FReferenceCollector& Collector;
		};

		auto VisitMapReference(void* RawContext, const void* Key, const void* Value) -> bool
		{
			auto& Context = *static_cast<FMapReferenceVisitContext*>(RawContext);
			if (Context.Operation.Key)
				VisitOperation(*Context.Operation.Key, const_cast<void*>(Key), Context.Collector);
			if (Context.Operation.Value)
				VisitOperation(*Context.Operation.Value, const_cast<void*>(Value), Context.Collector);
			return true;
		}
	}

	class FGCReferenceSchema
	{
	public:
		std::vector<std::shared_ptr<const FGCReferenceOperation>> Operations;
	};

	namespace
	{
		auto MakeOperation(EGCReferenceOperation Kind, FProperty* Property) -> std::shared_ptr<FGCReferenceOperation>
		{
			auto Operation = std::make_shared<FGCReferenceOperation>();
			Operation->Kind = Kind;
			Operation->Property = Property;
			return Operation;
		}

		auto CompileProperty(FProperty* Property) -> std::shared_ptr<const FGCReferenceOperation>
		{
			if (!Property) return nullptr;

			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				return ObjectProperty->IsObjectPtrWrapper() ? MakeOperation(EGCReferenceOperation::Object, Property) : nullptr;
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				check(Struct && "Reflected struct properties must resolve their DStruct before GC schema assembly.");
				FGCReferenceSchemaRegistry::Assemble(Struct);
				return FGCReferenceSchemaRegistry::HasReferences(Struct)
					? MakeOperation(EGCReferenceOperation::Struct, Property)
					: nullptr;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Property);
				auto Inner = CompileProperty(ArrayProperty->GetInner());
				if (!Inner) return nullptr;
				check(ArrayProperty->HasArrayOps()
					&& ArrayProperty->HasCapability(EArrayOpsFlags::ConstTraversal)
					&& "Reference-bearing reflected arrays require const traversal.");
				auto Operation = MakeOperation(EGCReferenceOperation::Array, Property);
				Operation->Inner = std::move(Inner);
				return Operation;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* MapProperty = static_cast<FMapProperty*>(Property);
				auto Key = CompileProperty(MapProperty->GetKeyProp());
				auto Value = CompileProperty(MapProperty->GetValueProp());
				if (!Key && !Value) return nullptr;
				check(MapProperty->HasMapOps()
					&& MapProperty->HasCapability(EMapOpsFlags::ConstTraversal)
					&& "Reference-bearing reflected maps require const traversal.");
				auto Operation = MakeOperation(EGCReferenceOperation::Map, Property);
				Operation->Key = std::move(Key);
				Operation->Value = std::move(Value);
				return Operation;
			}
			default:
				return nullptr;
			}
		}

		auto VisitOperationValue(const FGCReferenceOperation& Operation, void* Container, uint32 ArrayIndex, FReferenceCollector& Collector) -> void
		{
			switch (Operation.Kind)
			{
			case EGCReferenceOperation::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Operation.Property);
				DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				Collector.AddReferencedObject(ReferencedObject);
				break;
			}
			case EGCReferenceOperation::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Operation.Property);
				void* StructValue = StructProperty->GetValuePtr(Container, ArrayIndex);
				FGCReferenceSchemaRegistry::Visit(StructProperty->GetStruct(), StructValue, Collector);
				break;
			}
			case EGCReferenceOperation::Array:
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Operation.Property);
				FArrayReferenceVisitContext Context{*Operation.Inner, Collector};
				const EContainerOpResult Result = ArrayProperty->VisitElements(
					Container, &VisitArrayReference, &Context, ArrayIndex);
				checkf(Result == EContainerOpResult::Success,
					"Reference-bearing reflected array traversal failed.");
				break;
			}
			case EGCReferenceOperation::Map:
			{
				auto* MapProperty = static_cast<FMapProperty*>(Operation.Property);
				FMapReferenceVisitContext Context{Operation, Collector};
				const EContainerOpResult Result = MapProperty->VisitEntries(
					Container, &VisitMapReference, &Context, ArrayIndex);
				checkf(Result == EContainerOpResult::Success,
					"Reference-bearing reflected map traversal failed.");
				break;
			}
			}
		}

		auto VisitOperation(const FGCReferenceOperation& Operation, void* Container, FReferenceCollector& Collector) -> void
		{
			for (uint32 ArrayIndex = 0; ArrayIndex < Operation.Property->GetArrayDim(); ++ArrayIndex)
			{
				VisitOperationValue(Operation, Container, ArrayIndex, Collector);
			}
		}
	}

	auto FGCReferenceSchemaRegistry::Assemble(DStructBase* Type) -> void
	{
		if (!Type || Type->ReferenceSchema) return;
		FSchemaAssemblyScope AssemblyScope(Type);

		auto Schema = std::make_shared<FGCReferenceSchema>();
		if (DStructBase* Super = Type->GetSuperStructBase())
		{
			Assemble(Super);
			if (Super->ReferenceSchema)
			{
				Schema->Operations.insert(
					Schema->Operations.end(),
					Super->ReferenceSchema->Operations.begin(),
					Super->ReferenceSchema->Operations.end()
				);
			}
		}

		Type->ForEachProperty([&](FProperty* Property) {
			if (auto Operation = CompileProperty(Property)) Schema->Operations.push_back(std::move(Operation));
		}, false);
		Type->ReferenceSchema = std::move(Schema);
		if (std::ranges::find(GAssembledTypes, Type) == GAssembledTypes.end()) GAssembledTypes.push_back(Type);
	}

	auto FGCReferenceSchemaRegistry::FinalizeAndAssemble(DStructBase* Type) -> void
	{
		if (!Type) return;

		// A generated derived class can observe its superclass's inner metadata before
		// the superclass's outer registration attaches properties. Rebuild any schema
		// that copied that provisional superclass view when the superclass finalizes.
		std::vector<DStructBase*> Dependents;
		for (DStructBase* Candidate : GAssembledTypes)
		{
			for (DStructBase* Super = Candidate ? Candidate->GetSuperStructBase() : nullptr; Super; Super = Super->GetSuperStructBase())
			{
				if (Super == Type)
				{
					Dependents.push_back(Candidate);
					break;
				}
			}
		}

		Type->ReferenceSchema.reset();
		for (DStructBase* Dependent : Dependents) Dependent->ReferenceSchema.reset();
		Assemble(Type);
		std::ranges::sort(Dependents, [](const DStructBase* Left, const DStructBase* Right) {
			auto Depth = [](const DStructBase* Candidate) {
				uint32 Result = 0;
				for (; Candidate; Candidate = Candidate->GetSuperStructBase()) ++Result;
				return Result;
			};
			return Depth(Left) < Depth(Right);
		});
		for (DStructBase* Dependent : Dependents) Assemble(Dependent);
	}

	auto FGCReferenceSchemaRegistry::Visit(const DStructBase* Type, void* Instance, FReferenceCollector& Collector) -> void
	{
		if (!Type || !Instance) return;
		if (!Type->ReferenceSchema) Assemble(const_cast<DStructBase*>(Type));
		for (const auto& Operation : Type->ReferenceSchema->Operations)
		{
			VisitOperation(*Operation, Instance, Collector);
		}
	}

	auto FGCReferenceSchemaRegistry::Visit(
		const DStruct* Type,
		void* Instance,
		FReferenceCollector& Collector) -> void
	{
		Visit(static_cast<const DStructBase*>(Type), Instance, Collector);
		if (Type && Instance && Type->HasReferenceCollector())
		{
			Type->GetOps().CollectReferences(Instance, Collector);
		}
	}

	auto FGCReferenceSchemaRegistry::VisitProperty(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FReferenceCollector& Collector) -> void
	{
		if (!Property || !Container || ArrayIndex >= Property->GetArrayDim()) return;
		if (auto Operation = CompileProperty(Property))
		{
			VisitOperationValue(*Operation, Container, ArrayIndex, Collector);
		}
	}

	auto FGCReferenceSchemaRegistry::HasReferences(const DStructBase* Type) -> bool
	{
		return Type && Type->ReferenceSchema && !Type->ReferenceSchema->Operations.empty();
	}

	auto FGCReferenceSchemaRegistry::HasReferences(const DStruct* Type) -> bool
	{
		return Type && (HasReferences(static_cast<const DStructBase*>(Type)) || Type->HasReferenceCollector());
	}
}
