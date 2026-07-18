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
				check(ArrayProperty->HasArrayHelper() && "Reference-bearing reflected arrays require an array helper.");
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
				check(MapProperty->HasMapHelper() && "Reference-bearing reflected maps require a map helper.");
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
				const uint64 Num = ArrayProperty->Num(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Element = ArrayProperty->GetMutableElementPtr(Container, Index, ArrayIndex);
					VisitOperation(*Operation.Inner, Element, Collector);
				}
				break;
			}
			case EGCReferenceOperation::Map:
			{
				auto* MapProperty = static_cast<FMapProperty*>(Operation.Property);
				const uint64 Num = MapProperty->Num(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					if (Operation.Key)
					{
						void* Key = const_cast<void*>(MapProperty->GetKeyPtr(Container, Index, ArrayIndex));
						VisitOperation(*Operation.Key, Key, Collector);
					}
					if (Operation.Value)
					{
						void* Value = const_cast<void*>(MapProperty->GetMappedValuePtr(Container, Index, ArrayIndex));
						VisitOperation(*Operation.Value, Value, Collector);
					}
				}
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

	auto FGCReferenceSchemaRegistry::HasReferences(const DStructBase* Type) -> bool
	{
		return Type && Type->ReferenceSchema && !Type->ReferenceSchema->Operations.empty();
	}
}
