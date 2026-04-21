#pragma once

#include "CoreDObject/API.h"
#include "ObjectMacros.h"

namespace Doge
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

	namespace DogeCodeGen
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
			Enum
		};

		struct FClassParams
		{
			DClass* (*ClassNoRegisterFunc)();
			const char* ClassName;
		};

		struct FPropertyParamsBase
		{
			const char* NameUTF8;
			EPropertyFlags Flags;
			uint16 ArrayDim;
			uint16 Offset;
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

		COREDOBJECT_API auto ConstructDClass(const FClassParams& Params) -> DClass*;

	} // namespace DogeCodeGen
}