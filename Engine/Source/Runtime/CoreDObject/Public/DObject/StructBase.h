#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"

namespace Durin
{
	class FField;
	class FProperty;
	namespace Private { class FGCReferenceSchema; class FGCReferenceSchemaRegistry; }
	// Provides the common object identity for all reflected runtime metadata.
	class DType : public DObject
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DType, DObject, COREDOBJECT_API)

		DType(EStaticConstructor, EObjectFlags InFlags)
			: DObject(EC_StaticConstructor, InFlags)
		{
		}
	};

	// Describes reflected fields and memory layout shared by classes and value structs.
	class DStructBase : public DType
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DStructBase, DType, COREDOBJECT_API)

		DStructBase(EStaticConstructor, uint32 InSize, uint32 InMinAlignment, EObjectFlags InFlags)
			: DType(EC_StaticConstructor, InFlags)
			, PropertiesSize(InSize)
			, MinAlignment(InMinAlignment)
		{
		}

	private:
		FField* ChildProperties = nullptr;
		bool bPropertiesLinked = false;

		uint32 PropertiesSize = 0;

		uint32 MinAlignment = 0;

		DStructBase* SuperStructBase = nullptr;
		std::shared_ptr<const Private::FGCReferenceSchema> ReferenceSchema;
		std::vector<std::shared_ptr<const FProperty>> OwnedProperties;

		friend class Private::FGCReferenceSchemaRegistry;

	public:
		// Seals field configuration. Class dispatch admission is a separate step.
		virtual auto Link() -> bool { bPropertiesLinked = true; return true; }
		auto ArePropertiesLinked() const -> bool { return bPropertiesLinked; }
		COREDOBJECT_API auto CheckPropertiesMutable() const -> void;
		auto GetChildProperties() const -> FField* { return ChildProperties; }
		auto SetStructureLayout(uint32 Size, uint32 Alignment) -> void
		{ CheckPropertiesMutable(); PropertiesSize = Size; MinAlignment = Alignment; }
		// Borrowed chains are for native metadata whose caller owns field lifetime.
		COREDOBJECT_API auto SetChildProperties(FField* Properties) -> void;
		COREDOBJECT_API auto AddCppProperty(FProperty* Property) -> void;
		COREDOBJECT_API auto AddOwnedProperty(std::shared_ptr<const FProperty> Property) -> FProperty*;

		auto SetSuperStructBase(DStructBase* InSuperStructBase) -> void
		{
			CheckPropertiesMutable();
			SuperStructBase = InSuperStructBase;
		}

		auto GetSuperStructBase() const -> DStructBase* { return SuperStructBase; }
		virtual auto GetInheritanceSuper() const -> DStructBase* { return SuperStructBase; }
		auto GetStructureSize() const -> uint32 { return PropertiesSize; }
		auto GetMinAlignment() const -> uint32 { return MinAlignment; }

		COREDOBJECT_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		auto RegisterDependencies() -> void;

		COREDOBJECT_API auto ForEachProperty(const std::function<void(FProperty*)>& Visitor, bool bIncludeSuper = true) const -> void;

		COREDOBJECT_API auto FindPropertyByName(FName InName, bool bIncludeSuper = true) const -> FProperty*;
		// Resolves a serialized field name through current names and owner-scoped legacy aliases.
		COREDOBJECT_API auto FindPropertyBySerializedName(
			FName InName, bool bIncludeSuper = true) const -> FProperty*;
		COREDOBJECT_API auto ValidateSerializedPropertyNames() const -> void;
	};

}
