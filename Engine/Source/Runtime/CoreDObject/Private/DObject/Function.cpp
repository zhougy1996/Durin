#include "DObject/FunctionParameters.h"

#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "GCReferenceSchema.h"
#include "Threading/RunnableThread.h"

namespace
{
	thread_local const Durin::FFunctionFrame* GCurrentFunctionFrame = nullptr;
	auto GetFunctionBuffers() -> std::unordered_set<Durin::FFunctionParameterBuffer*>&
	{
		static std::unordered_set<Durin::FFunctionParameterBuffer*> Buffers;
		return Buffers;
	}
}

namespace Durin
{
	namespace
	{
		auto ValidFunctionProperty(const FProperty* Property, uint32 Depth = 0) -> bool
		{
			if (!Property || Depth > 4 || Property->GetArrayDim() != 1 || Property->HasValueAccessors()
				|| !Property->HasValueLifecycle() || !Property->CanCopyConstructValue() || !Property->CanCopyAssignValue()) return false;
			using K = DurinCodeGen::EPropertyGenFlags;
			switch (Property->GetKind())
			{
			case K::Enum:
			{
				if (!Property->ClassPrivate || !Property->ClassPrivate->IsChildOf(FEnumProperty::StaticClass())) return false;
				const auto* Enum = static_cast<const FEnumProperty*>(Property)->GetEnum();
				if (!Enum || Enum->GetUnderlyingSize() != Property->GetValueSize()) return false;
				auto Matches = [&]<typename T>() { return Property->GetValueSize() == sizeof(T) && Property->GetValueAlignment() == alignof(T); };
				using U = DurinCodeGen::EEnumUnderlyingType;
				switch (Enum->GetUnderlyingType())
				{
				case U::Int8: return Matches.operator()<int8>();
				case U::Int16: return Matches.operator()<int16>();
				case U::Int32: return Matches.operator()<int32>();
				case U::Int64: return Matches.operator()<int64>();
				case U::UInt8: return Matches.operator()<uint8>();
				case U::UInt16: return Matches.operator()<uint16>();
				case U::UInt32: return Matches.operator()<uint32>();
				case U::UInt64: return Matches.operator()<uint64>();
				default: return false;
				}
			}
			case K::SoftObject:
				return Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FSoftObjectProperty::StaticClass())
					&& Property->GetReferencedClass() && static_cast<const FSoftObjectProperty*>(Property)->HasSoftValueAccessors()
					&& Property->GetValueSize() == sizeof(TSoftObjectPtr<DObject>) && Property->GetValueAlignment() == alignof(TSoftObjectPtr<DObject>);
			case K::WeakObject:
				return Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FWeakObjectProperty::StaticClass())
					&& Property->GetReferencedClass() && static_cast<const FWeakObjectProperty*>(Property)->HasWeakValueAccessors()
					&& Property->GetValueSize() == sizeof(TWeakObjectPtr<DObject>) && Property->GetValueAlignment() == alignof(TWeakObjectPtr<DObject>);
			case K::Object:
				return (Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FObjectProperty::StaticClass())) && Property->GetReferencedClass()
					&& Property->GetValueSize() == (Property->IsObjectPtrWrapper() ? sizeof(TObjectPtr<DObject>) : sizeof(DObject*))
					&& Property->GetValueAlignment() == (Property->IsObjectPtrWrapper() ? alignof(TObjectPtr<DObject>) : alignof(DObject*))
					&& static_cast<const FObjectProperty*>(Property)->HasObjectValueWriter();
			case K::Struct:
			{
				if (!(Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FStructProperty::StaticClass()))) return false;
				const auto* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				return Struct && Struct->AreOpsInitialized() && Struct->CanDefaultConstruct() && Struct->CanDestroy()
					&& Struct->CanCopyConstruct() && Struct->CanCopyAssign()
					&& Property->GetValueSize() == Struct->PropertiesSize && Property->GetValueAlignment() == Struct->MinAlignment;
			}
			case K::Array:
			{
				if (!(Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FArrayProperty::StaticClass())) || Depth == 4) return false;
				const auto* Array = static_cast<const FArrayProperty*>(Property);
				return Array->HasArrayOps() && IsValidArrayOps(&Array->GetOps()) && Array->GetInner()
					&& Property->GetValueSize() == Array->GetOps().ContainerSize
					&& Property->GetValueAlignment() == Array->GetOps().ContainerAlignment
					&& Array->GetInner()->GetOffset() == 0 && Array->GetInner()->GetKind() != K::Bool
					&& ValidFunctionProperty(Array->GetInner(), Depth + 1);
			}
			case K::Map:
			{
				if (!(Property->ClassPrivate && Property->ClassPrivate->IsChildOf(FMapProperty::StaticClass())) || Depth == 4) return false;
				const auto* Map = static_cast<const FMapProperty*>(Property);
				return Map->HasMapOps() && IsValidMapOps(&Map->GetOps()) && Map->GetKeyProp() && Map->GetValueProp()
					&& Property->GetValueSize() == Map->GetOps().ContainerSize
					&& Property->GetValueAlignment() == Map->GetOps().ContainerAlignment
					&& Map->GetKeyProp()->GetOffset() == 0 && Map->GetValueProp()->GetOffset() == 0
					&& Map->GetKeyProp()->GetKind() != K::Object && Map->GetKeyProp()->GetKind() != K::Struct
					&& Map->GetKeyProp()->GetKind() != K::SoftObject && Map->GetKeyProp()->GetKind() != K::WeakObject
					&& Map->GetKeyProp()->GetKind() != K::Array && Map->GetKeyProp()->GetKind() != K::Map
					&& ValidFunctionProperty(Map->GetKeyProp(), Depth + 1) && ValidFunctionProperty(Map->GetValueProp(), Depth + 1);
			}
			default:
			{
				auto Matches = [&]<typename T>() { return Private::FunctionTypeMatches<T>(*Property); };
				switch (Property->GetKind())
				{
				case K::Bool: return Matches.operator()<bool>();
				case K::Int8: return Matches.operator()<int8>();
				case K::Int16: return Matches.operator()<int16>();
				case K::Int32: return Matches.operator()<int32>();
				case K::Int64: return Matches.operator()<int64>();
				case K::UInt8: return Matches.operator()<uint8>();
				case K::UInt16: return Matches.operator()<uint16>();
				case K::UInt32: return Matches.operator()<uint32>();
				case K::UInt64: return Matches.operator()<uint64>();
				case K::Float: return Matches.operator()<float>();
				case K::Double: return Matches.operator()<double>();
				case K::String: return Matches.operator()<std::string>();
				case K::Name: return Matches.operator()<FName>();
				case K::Guid: return Matches.operator()<FGuid>();
				default: return false;
				}
			}
			}
		}
	}

	auto FFunctionSignature::IsValid() const -> bool
	{
		if (IsVoidNoArgs()) return ParmsSize == 0 && ParmsAlignment == 1 && !Initialize && !Destroy;
		if (!ParmsSize || ParmsSize > std::numeric_limits<uint16>::max() || !ParmsAlignment
			|| (ParmsAlignment & (ParmsAlignment - 1)) || ParmsSize % ParmsAlignment || !Initialize || !Destroy) return false;
		std::vector<std::pair<uint32, uint32>> Ranges;
		auto CheckProperty = [&](const FProperty* Property) {
			if (!ValidFunctionProperty(Property)) return false;
			const uint32 Offset = Property->GetOffset(), Size = Property->GetValueSize(), Alignment = Property->GetValueAlignment();
			if (Offset > ParmsSize || Size > ParmsSize - Offset
				|| ParmsAlignment < Alignment || Offset % Alignment) return false;
			Ranges.emplace_back(Offset, Offset + Size);
			return true;
		};
		std::unordered_set<FName> Names;
		for (const auto& Parameter : Parameters)
		{
			if (!CheckProperty(Parameter.Property.get()) || Parameter.Property->NamePrivate.IsNone()
				|| !Names.insert(Parameter.Property->NamePrivate).second
				|| (Parameter.Passing != EFunctionParameterPassing::Value
					&& Parameter.Passing != EFunctionParameterPassing::ConstReference
					&& Parameter.Passing != EFunctionParameterPassing::Reference)) return false;
		}
		if (ReturnProperty && !CheckProperty(ReturnProperty.get())) return false;
		std::ranges::sort(Ranges);
		for (size_t Index = 1; Index < Ranges.size(); ++Index)
			if (Ranges[Index - 1].second > Ranges[Index].first) return false;
		return true;
	}

	auto FFunctionSignature::IsCompatibleWith(const FFunctionSignature& Other) const -> bool
	{
		return IsValid() && Other.IsValid() && HasCompatibleTypes(Other);
	}

	auto FFunctionSignature::HasCompatibleTypes(const FFunctionSignature& Other) const -> bool
	{
		if (Parameters.size() != Other.Parameters.size() || bool(ReturnProperty) != bool(Other.ReturnProperty)) return false;

		for (size_t Index = 0; Index < Parameters.size(); ++Index)
			if (Parameters[Index].Passing != Other.Parameters[Index].Passing
				|| !ArePropertyTypesSame(Parameters[Index].Property.get(), Other.Parameters[Index].Property.get())) return false;
		return !ReturnProperty || ArePropertyTypesSame(ReturnProperty.get(), Other.ReturnProperty.get());
	}

	FFunctionParameterBuffer::FFunctionParameterBuffer(const FFunction& Function) : Signature(Function.GetSignature())
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FGarbageCollectionDeferralScope Deferral;
		require(Function.bValidSignature);
		if (!Signature.ParmsSize) return;
		const auto Alignment = std::align_val_t(std::max<size_t>(Signature.ParmsAlignment, alignof(std::max_align_t)));
		Storage = ::operator new(Signature.ParmsSize, Alignment);
		try { Signature.Initialize(Storage); }
		catch (...) { ::operator delete(Storage, Alignment); throw; }
		try { GetFunctionBuffers().insert(this); }
		catch (...) { Signature.Destroy(Storage); ::operator delete(Storage, Alignment); throw; }
	}

	FFunctionParameterBuffer::~FFunctionParameterBuffer()
	{
		if (!Storage) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FGarbageCollectionDeferralScope Deferral;
		GetFunctionBuffers().erase(this);
		Signature.Destroy(Storage);
		::operator delete(Storage, std::align_val_t(std::max<size_t>(Signature.ParmsAlignment, alignof(std::max_align_t))));
	}

	namespace Private
	{
		auto AddFunctionParameterReferences(FReferenceCollector& Collector) -> void
		{
			for (auto* Buffer : GetFunctionBuffers())
			{
				for (const auto& Parameter : Buffer->Signature.Parameters)
					FGCReferenceSchemaRegistry::VisitProperty(const_cast<FProperty*>(Parameter.Property.get()), Buffer->Storage, 0, Collector, true);
				if (Buffer->Signature.ReturnProperty)
					FGCReferenceSchemaRegistry::VisitProperty(const_cast<FProperty*>(Buffer->Signature.ReturnProperty.get()), Buffer->Storage, 0, Collector, true);
			}
		}
	}

	auto FFunction::GetSuperFunction() const -> const FFunction*
	{
		require(Owner && Owner->AreFunctionsLinked());
		return SuperFunction;
	}

	auto FFunction::GetRootFunction() const -> const FFunction&
	{
		const auto* Super = GetSuperFunction();
		return Super ? Super->GetRootFunction() : *this;
	}

	FFunctionFrame::FFunctionFrame(DObject& InObject, const FFunction& InFunction, FMutableByteView InParameters)
		: Object(InObject), Function(InFunction), Parameters(InParameters), PreviousFrame(GCurrentFunctionFrame)
	{
		GCurrentFunctionFrame = this;
	}

	FFunctionFrame::~FFunctionFrame()
	{
		require(GCurrentFunctionFrame == this);
		GCurrentFunctionFrame = PreviousFrame;
	}

	auto FFunctionFrame::GetCurrentFrame() -> const FFunctionFrame*
	{
		return GCurrentFunctionFrame;
	}

	auto FFunction::ValidateInvocation(DObject& Target, FMutableByteView Parameters) const -> EFunctionInvokeError
	{
		if (!Owner || !Target.IsA(Owner)) return EFunctionInvokeError::WrongClass;
		if (!Owner->AreFunctionsLinked()) return EFunctionInvokeError::UnlinkedFunction;
		if (!bValidSignature) return EFunctionInvokeError::UnsupportedSignature;
		if (Parameters.size() != Signature.ParmsSize || (Signature.ParmsSize
			&& (!Parameters.data() || reinterpret_cast<uintptr_t>(Parameters.data()) % Signature.ParmsAlignment)))
			return EFunctionInvokeError::InvalidParameters;
		if (!HasAnyFlags(EFunctionFlags::Native) || !NativeFunction) return EFunctionInvokeError::MissingInvoker;
		return EFunctionInvokeError::None;
	}

	auto FFunction::Invoke(FFunctionFrame& Frame, void* Result) const -> void
	{
		NativeFunction(Frame.GetObject(), Frame, Result);
	}

}
