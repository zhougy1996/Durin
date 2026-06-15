#pragma once

#include "CoreDObjectAPI.h"
#include "ObjectMacros.h"

namespace Durin
{
	class DObject;
	class DClass;

	struct FStaticConstructObjectParameters
	{
		DClass* Class = nullptr;

		DObject* Outer = nullptr;

		FName Name;

		size_t Size = 0;
	};

	class FObjectInitializer
	{
	public:
		FORCEINLINE auto GetObj() const -> DObject* { return Obj; }

		static COREDOBJECT_API auto Get() -> const FObjectInitializer&;

		DObject* Obj = nullptr;

		DClass* Class = nullptr;

		DObject* Outer = nullptr;

		FName Name;
	};

	COREDOBJECT_API auto DObjectInit() -> void;

	COREDOBJECT_API auto DObjectForceRegistration(DObject* Object) -> void;

	COREDOBJECT_API auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*;

	COREDOBJECT_API auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*;

	template<typename T>
	auto NewObject(DObject* Outer, FName Name) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>, "T must be derived from DObject");

		FStaticConstructObjectParameters Params;
		Params.Class = T::StaticClass();
		Params.Outer = Outer;
		Params.Name = Name;
		Params.Size = sizeof(T);

		DObject* Obj = StaticConstructObject(Params);

		DObjectForceRegistration(Obj);
		return static_cast<T*>(Obj);
	}

	namespace DurinCodeGen
	{
		enum class EPropertyGenFlags : uint8
		{
			None = 0,
			Bool,
			Int8,
			Int16,
			Int32,
			Int64,
			UInt8,
			UInt16,
			UInt32,
			UInt64,
			Float,
			Double,
			String,
			Enum,
			Object
		};

		struct FPropertyParamsBase;

		struct FClassParams
		{
			DClass* (*ClassNoRegisterFunc)();
			const char* QualifiedClassName;
			const char* ShortClassName;
			const FPropertyParamsBase* const* PropertyParams;
			size_t NumProperties;
		};

		struct FPropertyParamsBase
		{
			const char* NameUTF8;
			EPropertyFlags Flags;
			uint16 ArrayDim;
			uint16 Offset;
			uint16 ElementSize;
			EPropertyGenFlags Kind;
			DClass* (*ReferencedClassFunc)();
		};

		struct FGenericPropertyParams : public FPropertyParamsBase
		{
			// Meta data
		};

		using FInt8PropertyParams = FPropertyParamsBase;
		using FInt16PropertyParams = FPropertyParamsBase;
		using FInt32PropertyParams = FPropertyParamsBase;
		using FInt64PropertyParams = FPropertyParamsBase;
		using FUInt8PropertyParams = FPropertyParamsBase;
		using FUInt16PropertyParams = FPropertyParamsBase;
		using FUInt32PropertyParams = FPropertyParamsBase;
		using FUInt64PropertyParams = FPropertyParamsBase;
		using FFloatPropertyParams = FPropertyParamsBase;
		using FDoublePropertyParams = FPropertyParamsBase;
		using FBoolPropertyParams = FPropertyParamsBase;
		using FStringPropertyParams = FPropertyParamsBase;
		using FEnumPropertyParams = FPropertyParamsBase;
		using FObjectPropertyParams = FPropertyParamsBase;

		COREDOBJECT_API auto ConstructDClass(const FClassParams& Params) -> DClass*;

	} // namespace DurinCodeGen
}
