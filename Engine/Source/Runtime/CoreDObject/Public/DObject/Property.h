#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectGlobals.h"
#include "Field.h"

namespace Durin
{
	class FProperty : public FField
	{
		DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass
		);

		auto GetPropertyFlags() const -> EPropertyFlags { return PropertyFlags; }
		auto GetArrayDim() const -> uint16 { return ArrayDim; }
		auto GetOffset() const -> uint16 { return Offset; }
		auto GetKind() const -> DurinCodeGen::EPropertyGenFlags { return Kind; }
		auto GetReferencedClass() const -> DClass* { return ReferencedClass; }

	private:
		EPropertyFlags PropertyFlags = EPropertyFlags::None;
		uint16 ArrayDim = 1;
		uint16 Offset = 0;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		DClass* ReferencedClass = nullptr;
	};
}
