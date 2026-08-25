#include "DObject/Class.h"

#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "GCReferenceSchema.h"
#include "Misc/StringHelper.h"
#include "QualifiedTypeRegistry.h"

namespace
{
	uint32 GDStructRegistrationBatchDepth = 0;

	struct FQualifiedTypeRegistry
	{
		std::unordered_map<Durin::FName, Durin::DClass*> Classes;
		std::unordered_map<Durin::FName, Durin::DStruct*> Structs;
		std::unordered_map<Durin::FName, Durin::DEnum*> Enums;
		std::unordered_map<Durin::FName, Durin::DClass*> LegacyClasses;
		std::unordered_map<Durin::FName, Durin::DStruct*> LegacyStructs;
		std::unordered_map<Durin::FName, Durin::DEnum*> LegacyEnums;
	};

	auto GetQualifiedTypeRegistry() -> FQualifiedTypeRegistry&
	{
		// Reflected types are process-lifetime objects. Function-local storage avoids
		// constructing FNames from global initializers before FNameInit establishes None.
		static FQualifiedTypeRegistry Registry;
		return Registry;
	}

	auto IsLegacyQualifiedName(const FQualifiedTypeRegistry& Registry, Durin::FName Name) -> bool
	{
		return Registry.LegacyClasses.contains(Name)
			|| Registry.LegacyStructs.contains(Name)
			|| Registry.LegacyEnums.contains(Name);
	}

	auto IsCurrentQualifiedName(const FQualifiedTypeRegistry& Registry, Durin::FName Name) -> bool
	{
		return Registry.Classes.contains(Name)
			|| Registry.Structs.contains(Name)
			|| Registry.Enums.contains(Name);
	}

	template<typename T>
	auto RegisterQualifiedType(std::unordered_map<Durin::FName, T*>& Types,
		const std::unordered_map<Durin::FName, T*>& LegacyTypes,
		Durin::FName QualifiedName, T* Type) -> void
	{
		check(Type);
		check(!QualifiedName.IsNone());
		check(!LegacyTypes.contains(QualifiedName) && "Current reflected names must not collide with legacy aliases.");
		auto [It, bInserted] = Types.emplace(QualifiedName, Type);
		check((bInserted || It->second == Type) && "Reflected qualified names must be unique.");
	}

	auto ValidateLegacyTypeNames(const FQualifiedTypeRegistry& Registry,
		std::span<const char* const> LegacyNames) -> void
	{
		for (const char* LegacyNameString : LegacyNames)
		{
			const Durin::FName LegacyName(LegacyNameString ? LegacyNameString : "");
			check(!LegacyName.IsNone());
			check(!IsCurrentQualifiedName(Registry, LegacyName)
				&& "Legacy reflected names must not collide with current names.");
			check(!IsLegacyQualifiedName(Registry, LegacyName)
				&& "Legacy reflected names must be globally unique.");
		}
	}

	template<typename T>
	auto RegisterLegacyTypeNames(const std::unordered_map<Durin::FName, T*>& Types,
		std::unordered_map<Durin::FName, T*>& LegacyTypes, T* Type,
		std::span<const char* const> LegacyNames) -> void
	{
		check(Type);
		for (const char* LegacyNameString : LegacyNames)
		{
			const Durin::FName LegacyName(LegacyNameString ? LegacyNameString : "");
			check(!LegacyName.IsNone());
			check(!Types.contains(LegacyName) && "Legacy reflected names must not collide with current names.");
			auto [It, bInserted] = LegacyTypes.emplace(LegacyName, Type);
			check((bInserted || It->second == Type) && "Legacy reflected names must be unique.");
		}
	}

	template<typename T>
	auto FindSerializedType(const std::unordered_map<Durin::FName, T*>& Types,
		const std::unordered_map<Durin::FName, T*>& LegacyTypes, Durin::FName Name) -> T*
	{
		if (const auto It = Types.find(Name); It != Types.end()) return It->second;
		const auto Legacy = LegacyTypes.find(Name);
		return Legacy != LegacyTypes.end() ? Legacy->second : nullptr;
	}

	auto MakeDefaultObjectName(std::string_view ShortName) -> std::string
	{
		const size_t Separator = ShortName.rfind("::");
		if (Separator != std::string_view::npos) ShortName.remove_prefix(Separator + 2);
		if (ShortName.size() >= 2 && (ShortName.front() == 'A' || ShortName.front() == 'D')
			&& std::isupper(static_cast<unsigned char>(ShortName[1])))
		{
			ShortName.remove_prefix(1);
		}
		return std::string(ShortName);
	}

	auto MakeDefaultDisplayName(std::string_view ShortName, std::string_view ConventionalPrefixes) -> std::string
	{
		const size_t Separator = ShortName.rfind("::");
		if (Separator != std::string_view::npos) ShortName.remove_prefix(Separator + 2);
		if (ShortName.size() >= 2 && ConventionalPrefixes.find(ShortName.front()) != std::string_view::npos
			&& std::isupper(static_cast<unsigned char>(ShortName[1])))
		{
			ShortName.remove_prefix(1);
		}
		return Durin::StringUtils::HumanizeName(ShortName);
	}

}

namespace Durin
{
	auto DStructBase::RegisterDependencies() -> void
	{
		if (SuperStructBase)
		{
			SuperStructBase->RegisterDependencies();
		}
	}

	auto DStructBase::ForEachProperty(const std::function<void(FProperty*)>& Visitor, bool bIncludeSuper) const -> void
	{
		if (bIncludeSuper && SuperStructBase)
		{
			SuperStructBase->ForEachProperty(Visitor, true);
		}

		for (FField* Field = ChildProperties; Field; Field = Field->Next)
		{
			Visitor(static_cast<FProperty*>(Field));
		}
	}

	auto DStructBase::FindPropertyByName(FName InName, bool bIncludeSuper) const -> FProperty*
	{
		FProperty* FoundProperty = nullptr;
		ForEachProperty(
			[&](FProperty* Property)
			{
				if (!FoundProperty && Property->NamePrivate == InName)
				{
					FoundProperty = Property;
				}
			},
			bIncludeSuper
		);
		return FoundProperty;
	}

	auto DStructBase::FindPropertyBySerializedName(FName InName, bool bIncludeSuper) const -> FProperty*
	{
		if (FProperty* Current = FindPropertyByName(InName, bIncludeSuper)) return Current;
		FProperty* FoundProperty = nullptr;
		ForEachProperty(
			[&](FProperty* Property)
			{
				if (!FoundProperty && Property->MatchesSerializedName(InName)) FoundProperty = Property;
			},
			bIncludeSuper
		);
		return FoundProperty;
	}

	auto DStructBase::ValidateSerializedPropertyNames() const -> void
	{
		std::unordered_set<FName> CurrentNames;
		std::unordered_set<FName> LegacyNames;
		ForEachProperty(
			[&](FProperty* Property)
			{
				check(Property);
				check(!Property->NamePrivate.IsNone());
				check(CurrentNames.emplace(Property->NamePrivate).second
					&& "Current property names must be unique within their declaring type.");
			},
			false
		);
		ForEachProperty(
			[&](FProperty* Property)
			{
				for (FName LegacyName : Property->GetLegacyNames())
				{
					check(!CurrentNames.contains(LegacyName)
						&& "Property legacy names must not collide with current names in their declaring type.");
					check(LegacyNames.emplace(LegacyName).second
						&& "Property legacy names must be unique within their declaring type.");
				}
			},
			false
		);
		std::unordered_set<FName> HistoricalNames;
		ForEachProperty(
			[&](FProperty* Property)
			{
				const FPropertyDeprecation* Deprecation = Property->GetDeprecation();
				if (!Deprecation) return;
				check(Property->HasAnyPropertyFlags(EPropertyFlags::Deprecated));
				check(HistoricalNames.emplace(Deprecation->HistoricalName).second
					&& "Deprecated historical routes must be unique within their declaring type.");
				for (FName TargetName : Deprecation->MigrationTargets)
				{
					FProperty* Target = FindPropertyByName(TargetName, false);
					check(Target && !Target->IsDeprecated()
						&& "Deprecated migration targets must name current properties on the same type.");
				}
			},
			false
		);
	}

	DStruct::~DStruct()
	{
		DestroyDefaultStorage(PendingDefaultValue);
		DestroyDefaultStorage(DefaultValue);
		PendingDefaultValue = nullptr;
		DefaultValue = nullptr;
	}

	auto DStruct::GetDefaultValue() const -> const void*
	{
		const EDStructDefaultState State = DefaultState.load(std::memory_order_acquire);
		if (State == EDStructDefaultState::Constructing)
		{
			bRecursiveDefaultAccess.store(true, std::memory_order_relaxed);
			return nullptr;
		}
		return State == EDStructDefaultState::Ready ? DefaultValue : nullptr;
	}

	auto DStruct::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		if (DefaultState.load(std::memory_order_acquire) == EDStructDefaultState::Ready)
		{
			Private::FGCReferenceSchemaRegistry::Visit(this, DefaultValue, Collector);
		}
	}

	auto DStruct::ResolveDefaultEligibility() -> bool
	{
		if (DefaultState.load(std::memory_order_relaxed) != EDStructDefaultState::Uninitialized) return false;
		EDStructDefaultReason Reason = EDStructDefaultReason::None;
		if (PropertiesSize == 0 || MinAlignment == 0 || (MinAlignment & (MinAlignment - 1)) != 0)
			Reason = EDStructDefaultReason::InvalidLayout;
		else if (!bOpsInitialized)
			Reason = EDStructDefaultReason::MissingInitializedOps;
		else if (!CanDefaultConstruct())
			Reason = EDStructDefaultReason::MissingDefaultConstructor;
		else if (!CanDestroy())
			Reason = EDStructDefaultReason::MissingDestructor;
		else if (!HasCompleteAuthoredFields())
			Reason = EDStructDefaultReason::IncompleteAuthoredFields;
		else if (HasSerializer())
			Reason = EDStructDefaultReason::CustomSerializer;
		else
		{
			bool bSupported = true;
			ForEachProperty([&](FProperty* Property) {
				if (bSupported && Property && !Property->HasAnyPropertyFlags(EPropertyFlags::Transient))
				{
					FPropertyIdentityDiagnostic Diagnostic;
					bSupported = ValidatePropertyIdentityDescriptor(Property, &Diagnostic);
					if (!bSupported && Diagnostic.Reason == EPropertyIdentityReason::DescriptorCycle)
						Reason = EDStructDefaultReason::RecursiveDependency;
				}
			}, false);
			if (!bSupported && Reason == EDStructDefaultReason::None)
				Reason = EDStructDefaultReason::UnsupportedPropertyIdentity;
		}

		if (Reason == EDStructDefaultReason::None) return true;
		DefaultReason.store(Reason, std::memory_order_relaxed);
		DefaultState.store(EDStructDefaultState::Unavailable, std::memory_order_release);
		return false;
	}

	auto DStruct::BeginDefaultConstruction() -> bool
	{
		EDStructDefaultState Expected = EDStructDefaultState::Uninitialized;
		if (!DefaultState.compare_exchange_strong(
				Expected, EDStructDefaultState::Constructing,
				std::memory_order_acq_rel, std::memory_order_acquire)) return false;
		DefaultReason.store(EDStructDefaultReason::None, std::memory_order_relaxed);
		bRecursiveDefaultAccess.store(false, std::memory_order_relaxed);
		return true;
	}

	auto DStruct::SetPendingDefaultValue(void* Value) -> void
	{
		check(DefaultState.load(std::memory_order_relaxed) == EDStructDefaultState::Constructing);
		check(Value && !PendingDefaultValue && !DefaultValue);
		PendingDefaultValue = Value;
	}

	auto DStruct::PublishDefaultValue() -> void
	{
		check(DefaultState.load(std::memory_order_relaxed) == EDStructDefaultState::Constructing);
		check(PendingDefaultValue && !DefaultValue);
		DefaultValue = PendingDefaultValue;
		PendingDefaultValue = nullptr;
		DefaultReason.store(EDStructDefaultReason::None, std::memory_order_relaxed);
		DefaultState.store(EDStructDefaultState::Ready, std::memory_order_release);
	}

	auto DStruct::FailDefaultConstruction(EDStructDefaultReason Reason) -> void*
	{
		void* FailedValue = PendingDefaultValue;
		PendingDefaultValue = nullptr;
		DefaultValue = nullptr;
		DefaultReason.store(Reason, std::memory_order_relaxed);
		DefaultState.store(EDStructDefaultState::Failed, std::memory_order_release);
		return FailedValue;
	}

	auto DStruct::ReleaseDefaultValue() -> void*
	{
		if (DefaultState.load(std::memory_order_acquire) != EDStructDefaultState::Ready) return nullptr;
		void* Value = DefaultValue;
		DefaultValue = nullptr;
		DefaultReason.store(EDStructDefaultReason::None, std::memory_order_relaxed);
		DefaultState.store(EDStructDefaultState::Released, std::memory_order_release);
		return Value;
	}

	auto DStruct::DestroyDefaultStorage(void* Value) const -> void
	{
		if (!Value) return;
		if (NeedsDestroy()) Ops->Destroy(Value);
		::operator delete(Value, std::align_val_t(MinAlignment));
	}

	DEnum::DEnum(
		EStaticConstructor,
		FName InName,
		FName InQualifiedName,
		FName InShortName,
		std::string_view InDisplayName,
		bool bInIsScoped,
		DurinCodeGen::EEnumUnderlyingType InUnderlyingType,
		uint16 InUnderlyingSize,
		std::vector<FEnumValue> InValues,
		EObjectFlags InFlags
	)
		: DType(EC_StaticConstructor, InFlags)
		, QualifiedName(InQualifiedName)
		, ShortName(InShortName)
		, DisplayName(InDisplayName.empty() ? MakeDefaultDisplayName(InShortName.ToString(), "E") : InDisplayName)
		, bIsScoped(bInIsScoped)
		, UnderlyingType(InUnderlyingType)
		, UnderlyingSize(InUnderlyingSize)
		, Values(std::move(InValues))
	{
		(void)InName;
		for (FEnumValue& Value : Values)
		{
			if (Value.DisplayName.empty()) Value.DisplayName = MakeDefaultDisplayName(Value.Name.ToString(), "");
		}
	}

	auto DEnum::FindValueRecordByName(FName InName) const -> const FEnumValue*
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Name == InName) return &Value;
		}
		return nullptr;
	}

	auto DEnum::FindValueRecordByValue(uint64 InValue) const -> const FEnumValue*
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Value == InValue) return &Value;
		}
		return nullptr;
	}

	auto DEnum::FindValueByName(FName InName, uint64& OutValue) const -> bool
	{
		const FEnumValue* Value = FindValueRecordByName(InName);
		if (!Value) return false;
		OutValue = Value->Value;
		return true;
	}

	auto DEnum::FindNameByValue(uint64 InValue, FName& OutName) const -> bool
	{
		const FEnumValue* Value = FindValueRecordByValue(InValue);
		if (!Value) return false;
		OutName = Value->Name;
		return true;
	}

	auto DEnum::ForEachValue(const std::function<void(const FEnumValue&)>& Visitor) const -> void
	{
		for (const FEnumValue& Value : Values)
		{
			Visitor(Value);
		}
	}

	auto GetPrivateStaticClassBody(
		const char* PackageName,
		const char* Name,
		DClass*& ReturnClass,
		void(*RegisterNativeFunc)(),
		uint32 InSize,
		uint32 InAlignment,
		EClassFlags InClassFlags,
		DClass::ClassConstructorType InClassConstructor,
		DClass::StaticClassFunctionType InSuperClassFn
	) -> DClass*
	{
		auto* Class = new DClass(
			EC_StaticConstructor,
			FName(Name),
			InSize,
			InAlignment,
			EObjectFlags::Intrinsic,
			InClassFlags,
			EClassCastFlags::DClass,
			InClassConstructor
		);

		ReturnClass = Class; // assign before setting superclass to handle circular dependencies

		DClass* SuperClass = InSuperClassFn ? InSuperClassFn() : nullptr;
		Class->SetSuperStructBase(SuperClass);

		Class->Register(DClass::StaticClass, PackageName, Name);

		return Class;
	}

	namespace Private
	{
		auto BeginDStructRegistrationBatch() -> void
		{
			++GDStructRegistrationBatchDepth;
		}

		auto EndDStructRegistrationBatch() -> void
		{
			check(GDStructRegistrationBatchDepth > 0);
			--GDStructRegistrationBatchDepth;
		}

		auto IsDStructRegistrationBatchActive() -> bool
		{
			return GDStructRegistrationBatchDepth > 0;
		}

		auto UpdateQualifiedClassName(DClass* Class, FName PreviousName) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			auto& Classes = Registry.Classes;
			if (!PreviousName.IsNone() && PreviousName != Class->GetQualifiedName())
			{
				auto Previous = Classes.find(PreviousName);
				if (Previous != Classes.end() && Previous->second == Class) Classes.erase(Previous);
			}
			check(!IsLegacyQualifiedName(Registry, Class->GetQualifiedName())
				&& "Current reflected names must not collide with legacy aliases.");
			RegisterQualifiedType(Classes, Registry.LegacyClasses,
				Class->GetQualifiedName(), Class);
		}

		auto RegisterQualifiedStruct(DStruct* Struct) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			check(!IsLegacyQualifiedName(Registry, Struct->GetQualifiedName())
				&& "Current reflected names must not collide with legacy aliases.");
			RegisterQualifiedType(Registry.Structs, Registry.LegacyStructs, Struct->GetQualifiedName(), Struct);
		}

		auto RegisterQualifiedEnum(DEnum* Enum) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			check(!IsLegacyQualifiedName(Registry, Enum->GetQualifiedName())
				&& "Current reflected names must not collide with legacy aliases.");
			RegisterQualifiedType(Registry.Enums, Registry.LegacyEnums, Enum->GetQualifiedName(), Enum);
		}

		auto RegisterLegacyClassNames(DClass* Class, std::span<const char* const> LegacyNames) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			ValidateLegacyTypeNames(Registry, LegacyNames);
			RegisterLegacyTypeNames(Registry.Classes, Registry.LegacyClasses, Class, LegacyNames);
		}

		auto RegisterLegacyStructNames(DStruct* Struct, std::span<const char* const> LegacyNames) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			ValidateLegacyTypeNames(Registry, LegacyNames);
			RegisterLegacyTypeNames(Registry.Structs, Registry.LegacyStructs, Struct, LegacyNames);
		}

		auto RegisterLegacyEnumNames(DEnum* Enum, std::span<const char* const> LegacyNames) -> void
		{
			auto& Registry = GetQualifiedTypeRegistry();
			ValidateLegacyTypeNames(Registry, LegacyNames);
			RegisterLegacyTypeNames(Registry.Enums, Registry.LegacyEnums, Enum, LegacyNames);
		}

		auto CreateDStructDefaultsForBatch(std::span<DStruct* const> Structs) -> bool
		{
			auto VisitContainedStructs = [](FProperty* Root, const std::function<void(DStruct*)>& Visitor) {
				std::function<void(FProperty*)> VisitProperty = [&](FProperty* Property) {
					if (!Property) return;
					switch (Property->GetKind())
					{
					case DurinCodeGen::EPropertyGenFlags::Struct:
						Visitor(static_cast<FStructProperty*>(Property)->GetStruct());
						break;
					case DurinCodeGen::EPropertyGenFlags::Array:
						VisitProperty(static_cast<FArrayProperty*>(Property)->GetInner());
						break;
					case DurinCodeGen::EPropertyGenFlags::Map:
						VisitProperty(static_cast<FMapProperty*>(Property)->GetKeyProp());
						VisitProperty(static_cast<FMapProperty*>(Property)->GetValueProp());
						break;
					default:
						break;
					}
				};
				VisitProperty(Root);
			};
			std::vector<DStruct*> BatchStructs;
			std::unordered_set<DStruct*> Added;
			auto AddStruct = [&](DStruct* Struct) {
				if (Struct && Added.insert(Struct).second) BatchStructs.push_back(Struct);
			};
			for (DStruct* Struct : Structs) AddStruct(Struct);
			for (size_t Index = 0; Index < BatchStructs.size(); ++Index)
			{
				BatchStructs[Index]->ForEachProperty([&](FProperty* Property) {
					VisitContainedStructs(Property, AddStruct);
				}, false);
			}
			std::ranges::sort(BatchStructs, [](const DStruct* Left, const DStruct* Right) {
				return Left->GetQualifiedName().ToString() < Right->GetQualifiedName().ToString();
			});

			std::unordered_set<DStruct*> Visiting;
			std::function<bool(DStruct*)> Resolve = [&](DStruct* Struct) -> bool {
				if (!Struct) return false;
				const EDStructDefaultState State = Struct->GetDefaultState();
				if (State == EDStructDefaultState::Ready || State == EDStructDefaultState::Constructing) return true;
				if (State != EDStructDefaultState::Uninitialized) return false;
				if (!Visiting.insert(Struct).second)
				{
					Struct->DefaultReason.store(EDStructDefaultReason::RecursiveDependency, std::memory_order_relaxed);
					Struct->DefaultState.store(EDStructDefaultState::Unavailable, std::memory_order_release);
					return false;
				}
				if (!Struct->ResolveDefaultEligibility())
				{
					Visiting.erase(Struct);
					return false;
				}
				bool bDependenciesReady = true;
				Struct->ForEachProperty([&](FProperty* Property) {
					if (!bDependenciesReady || !Property || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					VisitContainedStructs(Property, [&](DStruct* Dependency) {
						if (bDependenciesReady) bDependenciesReady = Resolve(Dependency);
					});
				}, false);
				Visiting.erase(Struct);
				if (!bDependenciesReady)
				{
					Struct->DefaultReason.store(EDStructDefaultReason::UnsupportedPropertyIdentity, std::memory_order_relaxed);
					Struct->DefaultState.store(EDStructDefaultState::Unavailable, std::memory_order_release);
					return false;
				}
				return true;
			};

			std::vector<DStruct*> Eligible;
			for (DStruct* Struct : BatchStructs)
			{
				if (Resolve(Struct) && Struct->GetDefaultState() == EDStructDefaultState::Uninitialized)
					Eligible.push_back(Struct);
			}

			std::vector<DStruct*> Constructing;
			auto FailBatch = [&](DStruct* FailedStruct, EDStructDefaultReason Reason) {
				DURIN_ERROR(STR("DStruct-default batch failed at '{}' with reason {}."),
					FailedStruct ? FailedStruct->GetQualifiedName().ToString() : std::string("<null>"),
					static_cast<uint32>(Reason));
				for (auto It = Constructing.rbegin(); It != Constructing.rend(); ++It)
				{
					DStruct* Struct = *It;
					void* Value = Struct->FailDefaultConstruction(
						Struct == FailedStruct ? Reason : EDStructDefaultReason::ConstructionFailed);
					Struct->DestroyDefaultStorage(Value);
				}
				for (DStruct* Struct : Eligible)
				{
					if (Struct->GetDefaultState() == EDStructDefaultState::Uninitialized)
					{
						Struct->DefaultReason.store(
							Struct == FailedStruct ? Reason : EDStructDefaultReason::ConstructionFailed,
							std::memory_order_relaxed);
						Struct->DefaultState.store(EDStructDefaultState::Failed, std::memory_order_release);
					}
				}
			};

			for (DStruct* Struct : Eligible)
			{
				if (!Struct->BeginDefaultConstruction())
				{
					FailBatch(Struct, EDStructDefaultReason::ConstructionFailed);
					return false;
				}
				Constructing.push_back(Struct);
				void* First = nullptr;
				void* Second = nullptr;
				bool bFirstLive = false;
				bool bSecondLive = false;
				EDStructDefaultReason Failure = EDStructDefaultReason::None;
				const std::vector<DObject*> ObjectsBefore = GDObjectArray.GetAll(
					EObjectQueryScope::IncludeTemplates);
				const std::unordered_set<DObject*> ExistingObjects(
					ObjectsBefore.begin(), ObjectsBefore.end());
				std::vector<DObject*> SideEffectObjects;
				try
				{
					First = ::operator new(Struct->PropertiesSize, std::align_val_t(Struct->MinAlignment));
					Second = ::operator new(Struct->PropertiesSize, std::align_val_t(Struct->MinAlignment));
					Struct->Ops->DefaultConstruct(First);
					bFirstLive = true;
					Struct->Ops->DefaultConstruct(Second);
					bSecondLive = true;
				}
				catch (...)
				{
					Failure = EDStructDefaultReason::ConstructionFailed;
				}

				if (Failure == EDStructDefaultReason::None)
				{
					for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
					{
						if (!ExistingObjects.contains(Object)) SideEffectObjects.push_back(Object);
					}
					if (!SideEffectObjects.empty()) Failure = EDStructDefaultReason::PublicationSideEffect;
				}
				if (Failure == EDStructDefaultReason::None
					&& Struct->bRecursiveDefaultAccess.load(std::memory_order_relaxed))
					Failure = EDStructDefaultReason::RecursiveConstruction;
				if (Failure == EDStructDefaultReason::None)
				{
					const EPropertyIdentityResult Identity = CompareStructValues(Struct, First, Second);
					if (Identity == EPropertyIdentityResult::Different)
						Failure = EDStructDefaultReason::NonDeterministicConstruction;
					else if (Identity == EPropertyIdentityResult::Unsupported)
						Failure = EDStructDefaultReason::UnsupportedPropertyIdentity;
				}

				if (bSecondLive) Struct->DestroyDefaultStorage(Second);
				else if (Second) ::operator delete(Second, std::align_val_t(Struct->MinAlignment));
				if (Failure != EDStructDefaultReason::None)
				{
					if (bFirstLive) Struct->DestroyDefaultStorage(First);
					else if (First) ::operator delete(First, std::align_val_t(Struct->MinAlignment));
					if (!SideEffectObjects.empty())
					{
						const std::unordered_set<DObject*> SideEffectSet(
							SideEffectObjects.begin(), SideEffectObjects.end());
						for (DObject* Object : SideEffectObjects)
						{
							if (!Object || SideEffectSet.contains(Object->GetOuter())) continue;
							if (Object->IsTemplateObject()) Private::MarkTemplateObjectHierarchyAsGarbage(Object);
							else MarkObjectHierarchyAsGarbage(Object);
						}
						CollectGarbage();
					}
					FailBatch(Struct, Failure);
					return false;
				}
				Struct->SetPendingDefaultValue(First);
			}

			for (DStruct* Struct : Constructing) Struct->PublishDefaultValue();
			return true;
		}
	}

	auto DClass::SetQualifiedName(FName InQualifiedName) -> void
	{
		FName PreviousName = QualifiedName;
		QualifiedName = InQualifiedName;
		Private::UpdateQualifiedClassName(this, PreviousName);
	}

	auto FindClassByQualifiedName(FName QualifiedName) -> DClass*
	{
		auto& Classes = GetQualifiedTypeRegistry().Classes;
		auto It = Classes.find(QualifiedName);
		return It != Classes.end() ? It->second : nullptr;
	}

	auto FindClassBySerializedName(FName SerializedName) -> DClass*
	{
		const auto& Registry = GetQualifiedTypeRegistry();
		return FindSerializedType(Registry.Classes, Registry.LegacyClasses, SerializedName);
	}

	auto DClass::IsChildOf(const DClass* InClass) const -> bool
	{
		for (const DClass* Class = this; Class; Class = Class->GetSuperClass())
		{
			if (Class == InClass) return true;
		}
		return false;
	}

	auto DClass::SetTypeNames(std::string_view InShortName, std::string_view InDisplayName, std::string_view InDefaultObjectName) -> void
	{
		ShortName = InShortName;
		DefaultObjectName = InDefaultObjectName.empty() ? MakeDefaultObjectName(ShortName) : std::string(InDefaultObjectName);
		DisplayName = InDisplayName.empty() ? MakeDefaultDisplayName(DefaultObjectName, "") : std::string(InDisplayName);
	}

	auto DClass::GetDefaultObject() const -> const DObject*
	{
		const EClassDefaultObjectState State = DefaultObjectState.load(std::memory_order_acquire);
		if (State == EClassDefaultObjectState::Constructing)
		{
			bRecursiveDefaultObjectAccess.store(true, std::memory_order_relaxed);
			return nullptr;
		}
		return State == EClassDefaultObjectState::Ready ? ClassDefaultObject : nullptr;
	}

	auto DClass::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		if (DefaultObjectState.load(std::memory_order_acquire) == EClassDefaultObjectState::Ready)
		{
			Collector.AddReferencedObject(ClassDefaultObject);
		}
	}

	auto DClass::ResolveDefaultObjectEligibility() -> bool
	{
		if (DefaultObjectState.load(std::memory_order_relaxed) != EClassDefaultObjectState::Uninitialized) return false;
		if (HasAnyClassFlags(EClassFlags::Abstract)) DefaultObjectReason = EClassDefaultObjectReason::Abstract;
		else if (HasAnyClassFlags(EClassFlags::Intrinsic)) DefaultObjectReason = EClassDefaultObjectReason::Intrinsic;
		else if (HasAnyClassFlags(EClassFlags::NoClassDefaultObject)) DefaultObjectReason = EClassDefaultObjectReason::NoClassDefaultObject;
		else if (!ClassConstructor) DefaultObjectReason = EClassDefaultObjectReason::MissingConstructor;
		else if (PropertiesSize < sizeof(DObject) || MinAlignment == 0) DefaultObjectReason = EClassDefaultObjectReason::InvalidLayout;
		else return true;

		DefaultObjectState.store(EClassDefaultObjectState::Ineligible, std::memory_order_release);
		return false;
	}

	auto DClass::BeginDefaultObjectConstruction() -> bool
	{
		EClassDefaultObjectState Expected = EClassDefaultObjectState::Uninitialized;
		if (!DefaultObjectState.compare_exchange_strong(
				Expected, EClassDefaultObjectState::Constructing,
				std::memory_order_acq_rel, std::memory_order_acquire)) return false;
		DefaultObjectReason = EClassDefaultObjectReason::None;
		bRecursiveDefaultObjectAccess.store(false, std::memory_order_relaxed);
		return true;
	}

	auto DClass::SetPendingDefaultObject(DObject* Object) -> void
	{
		check(DefaultObjectState.load(std::memory_order_relaxed) == EClassDefaultObjectState::Constructing);
		check(Object && Object->IsClassDefaultObject() && Object->GetClass() == this);
		PendingDefaultObject = Object;
	}

	auto DClass::PublishDefaultObject() -> void
	{
		check(DefaultObjectState.load(std::memory_order_relaxed) == EClassDefaultObjectState::Constructing);
		check(PendingDefaultObject);
		ClassDefaultObject = PendingDefaultObject;
		PendingDefaultObject = nullptr;
		DefaultObjectReason = EClassDefaultObjectReason::None;
		DefaultObjectState.store(EClassDefaultObjectState::Ready, std::memory_order_release);
	}

	auto DClass::FailDefaultObjectConstruction(EClassDefaultObjectReason Reason) -> DObject*
	{
		DObject* FailedObject = PendingDefaultObject;
		PendingDefaultObject = nullptr;
		ClassDefaultObject = nullptr;
		DefaultObjectReason = Reason;
		DefaultObjectState.store(EClassDefaultObjectState::Failed, std::memory_order_release);
		return FailedObject;
	}

	auto DClass::ReleaseDefaultObjectOwnership() -> DObject*
	{
		if (DefaultObjectState.load(std::memory_order_acquire) != EClassDefaultObjectState::Ready) return nullptr;
		DObject* Object = ClassDefaultObject;
		ClassDefaultObject = nullptr;
		DefaultObjectReason = EClassDefaultObjectReason::None;
		DefaultObjectState.store(EClassDefaultObjectState::Uninitialized, std::memory_order_release);
		return Object;
	}

	auto GetDerivedClasses(const DClass* BaseClass, bool bIncludeBase) -> std::vector<DClass*>
	{
		std::vector<DClass*> Classes;
		if (!BaseClass) return Classes;
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
		{
			auto* Class = Cast<DClass>(Object);
			if (Class && Class->IsChildOf(BaseClass) && (bIncludeBase || Class != BaseClass)) Classes.push_back(Class);
		}
		std::ranges::sort(Classes, [](const DClass* Left, const DClass* Right) {
			return Left->GetQualifiedName().ToString() < Right->GetQualifiedName().ToString();
		});
		return Classes;
	}

	auto FindStructByQualifiedName(FName QualifiedName) -> DStruct*
	{
		auto& Structs = GetQualifiedTypeRegistry().Structs;
		auto It = Structs.find(QualifiedName);
		return It != Structs.end() ? It->second : nullptr;
	}

	auto FindStructBySerializedName(FName SerializedName) -> DStruct*
	{
		const auto& Registry = GetQualifiedTypeRegistry();
		return FindSerializedType(Registry.Structs, Registry.LegacyStructs, SerializedName);
	}

	auto FindEnumByQualifiedName(FName QualifiedName) -> DEnum*
	{
		auto& Enums = GetQualifiedTypeRegistry().Enums;
		auto It = Enums.find(QualifiedName);
		return It != Enums.end() ? It->second : nullptr;
	}

	auto FindEnumBySerializedName(FName SerializedName) -> DEnum*
	{
		const auto& Registry = GetQualifiedTypeRegistry();
		return FindSerializedType(Registry.Enums, Registry.LegacyEnums, SerializedName);
	}

	auto CaptureSerializedReflectionAliases() -> std::vector<FSerializedReflectionAlias>
	{
		const auto& Registry = GetQualifiedTypeRegistry();
		std::vector<FSerializedReflectionAlias> Result;
		Result.reserve(Registry.LegacyClasses.size() + Registry.LegacyStructs.size()
			+ Registry.LegacyEnums.size());
		auto Append = [&]<typename T>(const std::unordered_map<FName, T*>& Aliases,
			ESerializedReflectedKind Kind)
		{
			for (const auto& [StoredName, Type] : Aliases)
				Result.push_back({StoredName.ToString(), Type->GetQualifiedName().ToString(), Kind});
		};
		Append(Registry.LegacyClasses, ESerializedReflectedKind::Class);
		Append(Registry.LegacyStructs, ESerializedReflectedKind::Struct);
		Append(Registry.LegacyEnums, ESerializedReflectedKind::Enum);
		std::ranges::sort(Result, [](const auto& Left, const auto& Right) {
			return std::tie(Left.StoredName, Left.CurrentName, Left.Kind)
				< std::tie(Right.StoredName, Right.CurrentName, Right.Kind);
		});
		return Result;
	}

	auto CaptureSerializedPropertyAliases() -> std::vector<FSerializedPropertyAlias>
	{
		const auto& Registry = GetQualifiedTypeRegistry();
		std::vector<FSerializedPropertyAlias> Result;
		auto Append = [&](const auto& Types)
		{
			for (const auto& [QualifiedName, Type] : Types)
			{
				if (!Type) continue;
				Type->ForEachProperty(
					[&](FProperty* Property)
					{
						for (FName LegacyName : Property->GetLegacyNames())
							Result.push_back({QualifiedName.ToString(), LegacyName.ToString(),
								Property->NamePrivate.ToString()});
					},
					false
				);
			}
		};
		Append(Registry.Classes);
		Append(Registry.Structs);
		std::ranges::sort(Result, [](const auto& Left, const auto& Right) {
			return std::tie(Left.DeclaringType, Left.StoredName, Left.CurrentName)
				< std::tie(Right.DeclaringType, Right.StoredName, Right.CurrentName);
		});
		return Result;
	}

	template<typename T>
	static auto FindTypeByPath(std::string_view ObjectPath) -> T*
	{
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
		{
			auto* Type = Cast<T>(Object);
			if (Type && Type->GetObjectPath() == ObjectPath) return Type;
		}
		return nullptr;
	}

	auto FindClassByPath(std::string_view ObjectPath) -> DClass* { return FindTypeByPath<DClass>(ObjectPath); }
	auto FindStructByPath(std::string_view ObjectPath) -> DStruct* { return FindTypeByPath<DStruct>(ObjectPath); }
	auto FindEnumByPath(std::string_view ObjectPath) -> DEnum* { return FindTypeByPath<DEnum>(ObjectPath); }
}

IMPLEMENT_INTRINSIC_CLASS(Durin::DType, DType, COREDOBJECT_API, Durin::DObject, DObject, COREDOBJECT_API, {})

IMPLEMENT_INTRINSIC_CLASS(Durin::DStructBase, DStructBase, COREDOBJECT_API, Durin::DType, DType, COREDOBJECT_API, {})

IMPLEMENT_INTRINSIC_CLASS(Durin::DClass, DClass, COREDOBJECT_API, Durin::DStructBase, DStructBase, COREDOBJECT_API, {})

IMPLEMENT_INTRINSIC_CLASS(Durin::DStruct, DStruct, COREDOBJECT_API, Durin::DStructBase, DStructBase, COREDOBJECT_API, {})

IMPLEMENT_INTRINSIC_CLASS(Durin::DEnum, DEnum, COREDOBJECT_API, Durin::DType, DType, COREDOBJECT_API, {})
