#include "DObject/DObjectGlobals.h"
#include "QualifiedTypeRegistry.h"
#include "Misc/Name.h"

#include "Misc/AppConfig.h"
#include "Misc/Time.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/Package.h"
#include "GCReferenceSchema.h"

#include <limits>

namespace Durin
{
	namespace
	{
		auto GetGeneratedPropertyOwnerName(const FFieldVariant& Owner) -> std::string
		{
			if (DObject* Object = Owner.ToDObject()) return Object->GetName();
			if (FField* Field = Owner.ToField()) return Field->NamePrivate.ToString();
			return "<null>";
		}

		auto ConstructGeneratedProperty(
			const FFieldVariant& Owner,
			const DurinCodeGen::FPropertyParamsBase* PropertyParams
		) -> FProperty*
		{
			FProperty* Property = nullptr;
			DurinCodeGen::EPropertyParamLayout ExpectedLayout = DurinCodeGen::EPropertyParamLayout::Legacy;
			if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Struct) ExpectedLayout = DurinCodeGen::EPropertyParamLayout::Struct;
			else if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Array) ExpectedLayout = DurinCodeGen::EPropertyParamLayout::Array;
			else if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Map) ExpectedLayout = DurinCodeGen::EPropertyParamLayout::Map;
			else if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::SoftObject) ExpectedLayout = DurinCodeGen::EPropertyParamLayout::SoftObject;
			if (PropertyParams->Layout != ExpectedLayout)
			{
				checkf(
					false,
					"PropertyRegistration.KindLayoutMismatch owner '{}' property '{}'.",
					GetGeneratedPropertyOwnerName(Owner),
					PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>"
				);
				return nullptr;
			}
			if (ExpectedLayout != DurinCodeGen::EPropertyParamLayout::Legacy)
			{
				const bool bHasMutableAccessor = PropertyParams->MutableValueAccessor != nullptr;
				const bool bHasConstAccessor = PropertyParams->ConstValueAccessor != nullptr;
				if (bHasMutableAccessor != bHasConstAccessor || (bHasMutableAccessor && PropertyParams->Offset != 0))
				{
					checkf(false, "PropertyRegistration.AccessorPairMismatch owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
					return nullptr;
				}
				if ((PropertyParams->MetaData == nullptr) != (PropertyParams->NumMetaData == 0))
				{
					checkf(false, "PropertyRegistration.MetadataMismatch owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
					return nullptr;
				}
			}

			auto ResolveReferencedClass = [PropertyParams]() -> DClass*
			{
				return PropertyParams->ReferencedClassFunc ? PropertyParams->ReferencedClassFunc() : nullptr;
			};
			auto ResolveReferencedEnum = [PropertyParams]() -> DEnum*
			{
				return PropertyParams->ReferencedEnumFunc ? PropertyParams->ReferencedEnumFunc() : nullptr;
			};

			switch (PropertyParams->Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
				Property = new FBoolProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::String:
				Property = new FStringProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Name:
				Property = new FNameProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Guid:
				Property = new FGuidProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Enum:
				Property = new FEnumProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass(),
					ResolveReferencedEnum()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Object:
				Property = new FObjectProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass(),
					PropertyParams->bIsObjectPtrWrapper
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				const auto* SoftParams = static_cast<const DurinCodeGen::FSoftObjectPropertyParams*>(PropertyParams);
				const std::string OwnerName = GetGeneratedPropertyOwnerName(Owner);
				const char* PropertyName = PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>";
				DClass* ExpectedClass = ResolveReferencedClass();
				if (!ExpectedClass || !SoftParams->MutableSoftValueAccessor || !SoftParams->ConstSoftValueAccessor
					|| PropertyParams->bIsObjectPtrWrapper || PropertyParams->ReferencedEnumFunc
					|| PropertyParams->ReferencedStructFunc || PropertyParams->Inner || PropertyParams->Key || PropertyParams->Value
					|| PropertyParams->ElementSize == 0 || PropertyParams->ValueSize != PropertyParams->ElementSize
					|| PropertyParams->ValueAlignment == 0 || !PropertyParams->InitializeValue || !PropertyParams->DestroyValue
					|| !PropertyParams->CopyConstructValue || !PropertyParams->CopyAssignValue)
				{
					checkf(false, "SoftObjectPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				Property = new FSoftObjectProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					ExpectedClass,
					SoftParams->MutableSoftValueAccessor,
					SoftParams->ConstSoftValueAccessor
				);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* StructParams = static_cast<const DurinCodeGen::FStructPropertyParams*>(PropertyParams);
				const std::string OwnerName = GetGeneratedPropertyOwnerName(Owner);
				const char* PropertyName = PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>";
				if (!StructParams->StructResolver)
				{
					checkf(false, "StructPropertyRegistration.MissingResolver owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				DStruct* ReferencedStruct = StructParams->StructResolver();
				if (!ReferencedStruct)
				{
					checkf(false, "StructPropertyRegistration.NullDescriptor owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				if (ReferencedStruct->PropertiesSize == 0 || ReferencedStruct->PropertiesSize > std::numeric_limits<uint16>::max())
				{
					checkf(false, "StructPropertyRegistration.InvalidSize owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				const uint32 Alignment = ReferencedStruct->MinAlignment;
				if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
				{
					checkf(false, "StructPropertyRegistration.InvalidAlignment owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				const bool bHasMutableAccessor = PropertyParams->MutableValueAccessor != nullptr;
				const bool bHasConstAccessor = PropertyParams->ConstValueAccessor != nullptr;
				if (bHasMutableAccessor != bHasConstAccessor || (bHasMutableAccessor && PropertyParams->Offset != 0))
				{
					checkf(false, "StructPropertyRegistration.AccessorPairMismatch owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				if ((PropertyParams->MetaData == nullptr) != (PropertyParams->NumMetaData == 0))
				{
					checkf(false, "StructPropertyRegistration.MetadataMismatch owner '{}' property '{}'.", OwnerName, PropertyName);
					return nullptr;
				}
				Property = new FStructProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					ReferencedStruct
				);
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const auto* ArrayParams = static_cast<const DurinCodeGen::FArrayPropertyParams*>(PropertyParams);
				const FArrayOps* Ops = ArrayParams->OpsResolver ? ArrayParams->OpsResolver() : nullptr;
				if (!IsValidArrayOps(Ops) || !ArrayParams->InnerParams || Ops->ContainerSize > std::numeric_limits<uint16>::max())
				{
					checkf(false, "ArrayPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
					return nullptr;
				}
				Property = new FArrayProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					static_cast<uint16>(Ops->ContainerSize),
					PropertyParams->Kind,
					nullptr,
					Ops
				);
				static_cast<FArrayProperty*>(Property)->SetInner(ConstructGeneratedProperty(FFieldVariant(Property), ArrayParams->InnerParams));
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto* MapParams = static_cast<const DurinCodeGen::FMapPropertyParams*>(PropertyParams);
				const FMapOps* Ops = MapParams->OpsResolver ? MapParams->OpsResolver() : nullptr;
				if (!IsValidMapOps(Ops) || !MapParams->KeyParams || !MapParams->ValueParams || Ops->ContainerSize > std::numeric_limits<uint16>::max())
				{
					checkf(false, "MapPropertyRegistration.InvalidDescriptor owner '{}' property '{}'.", GetGeneratedPropertyOwnerName(Owner), PropertyParams->NameUTF8 ? PropertyParams->NameUTF8 : "<null>");
					return nullptr;
				}
				Property = new FMapProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					static_cast<uint16>(Ops->ContainerSize),
					PropertyParams->Kind,
					nullptr,
					Ops
				);
				static_cast<FMapProperty*>(Property)->SetKeyProp(ConstructGeneratedProperty(FFieldVariant(Property), MapParams->KeyParams));
				static_cast<FMapProperty*>(Property)->SetValueProp(ConstructGeneratedProperty(FFieldVariant(Property), MapParams->ValueParams));
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
				Property = new FNumericProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::None:
			default:
				Property = new FProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ResolveReferencedClass()
				);
				break;
			}

			Property->SetValueAccessors(PropertyParams->MutableValueAccessor, PropertyParams->ConstValueAccessor);
			if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Array)
			{
				const FArrayOps& Ops = static_cast<FArrayProperty*>(Property)->GetOps();
				Property->SetValueLifecycle(Ops.ContainerSize, Ops.ContainerAlignment, Ops.Initialize, Ops.Destroy);
			}
			else if (PropertyParams->Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				const FMapOps& Ops = static_cast<FMapProperty*>(Property)->GetOps();
				Property->SetValueLifecycle(Ops.ContainerSize, Ops.ContainerAlignment, Ops.Initialize, Ops.Destroy);
			}
			else if (PropertyParams->Kind != DurinCodeGen::EPropertyGenFlags::Struct)
			{
				Property->SetValueLifecycle(
					PropertyParams->ValueSize,
					PropertyParams->ValueAlignment,
					PropertyParams->InitializeValue,
					PropertyParams->DestroyValue,
					PropertyParams->CopyConstructValue,
					PropertyParams->CopyAssignValue
				);
			}
			for (size_t Index = 0; PropertyParams->MetaData && Index < PropertyParams->NumMetaData; ++Index)
			{
				const DurinCodeGen::FMetaDataPair& Pair = PropertyParams->MetaData[Index];
				if (Pair.Key && Pair.Key[0] != '\0') Property->SetMetaData(FName(Pair.Key), Pair.Value ? Pair.Value : "");
			}
			return Property;
		}
	}

	auto FObjectInitializer::Get() -> const FObjectInitializer&
	{
		static thread_local FObjectInitializer Instance;
		return Instance;
	}

	auto DObjectInit() -> void
	{
		check(IsFNameInitialized() && "FNameInit must run before reflected type initialization.");
		ProcessNewlyLoadedDObjects();
		DObjectProcessRegistrants();
		AttachCoreIntrinsicTypesToCppPackage();

		FModuleManager::Get().SetProcessLoadedObjectsCallback(ProcessNewlyLoadedDObjects);
		FModuleManager::Get().StartProcessingNewlyLoadedObjects();

		auto& array = GDObjectArray;

		// CoreDObject owns its schema so Launch only coordinates subsystem initialization.
		const FYamlNodeView GCConfig = GetModuleConfig("CoreDObject").GetView("GC");
		FGarbageCollectionSettings GCSettings;
		GCSettings.bEnabled = GCConfig.GetView("Enabled").GetBool(true);
		GCSettings.IntervalSeconds = GCConfig.GetView("IntervalSeconds").GetDouble(60.0);
		GCSettings.MaxIntervalSeconds = GCConfig.GetView("MaxIntervalSeconds").GetDouble(600.0);
		GCSettings.IntervalBackoffMultiplier = GCConfig.GetView("IntervalBackoffMultiplier").GetDouble(2.0);
		GCSettings.PendingKillThreshold = GCConfig.GetView("PendingKillThreshold").GetUInt(128);
		GCSettings.ObjectGrowthThreshold = GCConfig.GetView("ObjectGrowthThreshold").GetUInt(1024);
		ConfigureAutomaticGarbageCollection(GCSettings, FTime::Seconds());
	}

	auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*
	{
		// Allocate memory and zero it out
		DObject* Obj = nullptr;
		Obj = static_cast<DObject*>(::operator new(Size));
		assert(Obj && "Memory allocation failed");
		new (Obj) DObject(Class, Outer, Name);

		return Obj;
	}

	auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*
	{
		DObject* Obj = StaticAllocateObject(Params.Class, Params.Outer, Params.Name, Params.Size);

		DClass* InClass = Params.Class;

		assert(InClass && InClass->ClassConstructor);

		FObjectInitializer ObjectInitializer;
		ObjectInitializer.Obj = Obj;
		ObjectInitializer.Class = InClass;
		ObjectInitializer.Outer = Params.Outer;
		ObjectInitializer.Name = Params.Name;

		InClass->ClassConstructor(ObjectInitializer);

		Obj->SetOuterPrivate(Params.Outer);
		Obj->AddObject(Params.Name);

		return Obj;
	}

	auto NewObject(DClass* Class, DObject* Outer, FName Name) -> DObject*
	{
		if (!CanConstructObjectOfClass(Class, DObject::StaticClass())) return nullptr;
		FStaticConstructObjectParameters Params;
		Params.Class = Class;
		Params.Outer = Outer;
		Params.Name = Name;
		Params.Size = Class->PropertiesSize;
		DObject* Object = StaticConstructObject(Params);
		DObjectForceRegistration(Object);
		return Object;
	}

	auto CanConstructObjectOfClass(const DClass* Class, const DClass* RequiredBaseClass) -> bool
	{
		return Class && RequiredBaseClass && Class->IsChildOf(RequiredBaseClass) && Class->ClassConstructor
			&& !Class->HasAnyClassFlags(EClassFlags::Abstract) && Class->PropertiesSize >= sizeof(DObject);
	}


	auto DurinCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
	{
		DClass* Class = Params.ClassNoRegisterFunc();

		DObjectForceRegistration(Class);
		Class->SetQualifiedName(FName(Params.QualifiedClassName));
		Class->SetTypeNames(
			Params.ShortClassName ? Params.ShortClassName : "",
			Params.DisplayName ? Params.DisplayName : "",
			Params.DefaultObjectName ? Params.DefaultObjectName : ""
		);

		if (!Class->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				const FPropertyParamsBase* PropertyParams = Params.PropertyParams[Index];
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Class), PropertyParams);

				if (LastProperty)
				{
					LastProperty->Next = Property;
				}
				else
				{
					Class->ChildProperties = Property;
				}
				LastProperty = Property;
			}
		}
		Private::FGCReferenceSchemaRegistry::FinalizeAndAssemble(Class);
		return Class;
	}

	auto DurinCodeGen::ConstructDEnum(const FEnumParams& Params) -> DEnum*
	{
		DEnum* Enum = Params.EnumNoRegisterFunc();

		DObjectForceRegistration(Enum);
		Private::RegisterQualifiedEnum(Enum);

		return Enum;
	}

	auto DurinCodeGen::ConstructDStruct(const FStructParams& Params) -> DStruct*
	{
		DStruct* Struct = Params.StructNoRegisterFunc();
		DObjectForceRegistration(Struct);
		Private::RegisterQualifiedStruct(Struct);
		Struct->InitializeOps(Params.Ops);
		if (!Struct->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Struct), Params.PropertyParams[Index]);
				if (LastProperty) LastProperty->Next = Property;
				else Struct->ChildProperties = Property;
				LastProperty = Property;
			}
		}
		Private::FGCReferenceSchemaRegistry::FinalizeAndAssemble(Struct);
		return Struct;
	}
}
