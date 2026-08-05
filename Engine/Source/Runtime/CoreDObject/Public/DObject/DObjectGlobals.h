#pragma once

#include "CoreDObjectAPI.h"
#include "ContainerOps.h"
#include "ObjectMacros.h"
#include "StructOps.h"

namespace Durin
{
	class DObject;
	class DClass;
	class DEnum;
	class DStruct;

	// Collects the class, Outer, name, and flags needed to allocate one DObject.
	struct FStaticConstructObjectParameters
	{
		DClass* Class = nullptr;

		DObject* Outer = nullptr;

		FName Name;

		size_t Size = 0;
	};

	// Carries the preallocated object storage and construction context into generated constructors.
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
	COREDOBJECT_API auto NewObject(DClass* Class, DObject* Outer, FName Name) -> DObject*;
	COREDOBJECT_API auto CanConstructObjectOfClass(const DClass* Class, const DClass* RequiredBaseClass) -> bool;

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

	template<typename T>
	auto NewObject(DClass* Class, DObject* Outer, FName Name) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>, "T must be derived from DObject");
		return CanConstructObjectOfClass(Class, T::StaticClass()) ? static_cast<T*>(NewObject(Class, Outer, Name)) : nullptr;
	}

	namespace DurinCodeGen
	{
		enum class EEnumUnderlyingType : uint8
		{
			Unknown = 0,
			Int8,
			Int16,
			Int32,
			Int64,
			UInt8,
			UInt16,
			UInt32,
			UInt64
		};

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
			Object,
			Array,
			Map,
			Struct,
			Name,
			Guid
		};

		enum class EPropertyParamLayout : uint8
		{
			Legacy = 0,
			Struct,
			Array,
			Map
		};

		struct FPropertyParamsBase;

		template<typename T>
		auto InitializePropertyValue(void* Memory) -> void
		{
			std::construct_at(static_cast<T*>(Memory));
		}

		template<typename T>
		auto DestroyPropertyValue(void* Memory) -> void
		{
			std::destroy_at(static_cast<T*>(Memory));
		}

		struct FEnumValueParams
		{
			const char* NameUTF8;
			uint64 Value;
			const char* DisplayName;
		};

		struct FEnumParams
		{
			DEnum* (*EnumNoRegisterFunc)();
			const char* QualifiedEnumName;
			const char* ShortEnumName;
			const char* DisplayName;
			bool bIsScoped;
			EEnumUnderlyingType UnderlyingType;
			uint16 UnderlyingSize;
			const FEnumValueParams* Values;
			size_t NumValues;
		};

		struct FClassParams
		{
			DClass* (*ClassNoRegisterFunc)();
			const char* QualifiedClassName;
			const char* ShortClassName;
			const FPropertyParamsBase* const* PropertyParams;
			size_t NumProperties;
			const char* DisplayName = nullptr;
			const char* DefaultObjectName = nullptr;
		};

		struct FMetaDataPair
		{
			const char* Key;
			const char* Value;
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
			DEnum* (*ReferencedEnumFunc)();
			const FPropertyParamsBase* Inner;
			const FPropertyParamsBase* Key;
			const FPropertyParamsBase* Value;
			bool bIsObjectPtrWrapper = false;
			DStruct* (*ReferencedStructFunc)() = nullptr;
			void* (*MutableValueAccessor)(void* Container, uint32 ArrayIndex) = nullptr;
			const void* (*ConstValueAccessor)(const void* Container, uint32 ArrayIndex) = nullptr;
			const FMetaDataPair* MetaData = nullptr;
			size_t NumMetaData = 0;
			uint32 ValueSize = 0;
			uint32 ValueAlignment = 0;
			void (*InitializeValue)(void* Memory) = nullptr;
			void (*DestroyValue)(void* Memory) = nullptr;
			EPropertyParamLayout Layout = EPropertyParamLayout::Legacy;
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
		using FNamePropertyParams = FPropertyParamsBase;
		using FGuidPropertyParams = FPropertyParamsBase;
		using FEnumPropertyParams = FPropertyParamsBase;
		using FObjectPropertyParams = FPropertyParamsBase;

		struct FArrayPropertyParams final : public FPropertyParamsBase
		{
			using FOpsResolver = const FArrayOps* (*)();
			using FMutableValueAccessor = void* (*)(void* Container, uint32 ArrayIndex);
			using FConstValueAccessor = const void* (*)(const void* Container, uint32 ArrayIndex);

			constexpr FArrayPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				const FPropertyParamsBase* InInnerParams,
				FOpsResolver InOpsResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase{}
				, InnerParams(InInnerParams)
				, OpsResolver(InOpsResolver)
			{
				NameUTF8 = InNameUTF8;
				Flags = InFlags;
				ArrayDim = InArrayDim;
				Offset = InOffset;
				Kind = EPropertyGenFlags::Array;
				MetaData = InMetaData;
				NumMetaData = InNumMetaData;
				Layout = EPropertyParamLayout::Array;
			}

			static constexpr auto WithAccessors(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				const FPropertyParamsBase* InInnerParams,
				FOpsResolver InOpsResolver,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FArrayPropertyParams
			{
				FArrayPropertyParams Params(InNameUTF8, InFlags, InArrayDim, 0, InInnerParams, InOpsResolver, InMetaData, InNumMetaData);
				Params.MutableValueAccessor = InMutableValueAccessor;
				Params.ConstValueAccessor = InConstValueAccessor;
				return Params;
			}

			const FPropertyParamsBase* InnerParams = nullptr;
			FOpsResolver OpsResolver = nullptr;
		};

		struct FMapPropertyParams final : public FPropertyParamsBase
		{
			using FOpsResolver = const FMapOps* (*)();
			using FMutableValueAccessor = void* (*)(void* Container, uint32 ArrayIndex);
			using FConstValueAccessor = const void* (*)(const void* Container, uint32 ArrayIndex);

			constexpr FMapPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				const FPropertyParamsBase* InKeyParams,
				const FPropertyParamsBase* InValueParams,
				FOpsResolver InOpsResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase{}
				, KeyParams(InKeyParams)
				, ValueParams(InValueParams)
				, OpsResolver(InOpsResolver)
			{
				NameUTF8 = InNameUTF8;
				Flags = InFlags;
				ArrayDim = InArrayDim;
				Offset = InOffset;
				Kind = EPropertyGenFlags::Map;
				MetaData = InMetaData;
				NumMetaData = InNumMetaData;
				Layout = EPropertyParamLayout::Map;
			}

			static constexpr auto WithAccessors(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				const FPropertyParamsBase* InKeyParams,
				const FPropertyParamsBase* InValueParams,
				FOpsResolver InOpsResolver,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FMapPropertyParams
			{
				FMapPropertyParams Params(InNameUTF8, InFlags, InArrayDim, 0, InKeyParams, InValueParams, InOpsResolver, InMetaData, InNumMetaData);
				Params.MutableValueAccessor = InMutableValueAccessor;
				Params.ConstValueAccessor = InConstValueAccessor;
				return Params;
			}

			const FPropertyParamsBase* KeyParams = nullptr;
			const FPropertyParamsBase* ValueParams = nullptr;
			FOpsResolver OpsResolver = nullptr;
		};

		struct FStructPropertyParams final : public FPropertyParamsBase
		{
			using FStructResolver = DStruct* (*)();
			using FMutableValueAccessor = void* (*)(void* Container, uint32 ArrayIndex);
			using FConstValueAccessor = const void* (*)(const void* Container, uint32 ArrayIndex);

			constexpr FStructPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FStructResolver InStructResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase{}
				, StructResolver(InStructResolver)
			{
				NameUTF8 = InNameUTF8;
				Flags = InFlags;
				ArrayDim = InArrayDim;
				Offset = InOffset;
				Kind = EPropertyGenFlags::Struct;
				MetaData = InMetaData;
				NumMetaData = InNumMetaData;
				Layout = EPropertyParamLayout::Struct;
			}

			static constexpr auto WithAccessors(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				FStructResolver InStructResolver,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FStructPropertyParams
			{
				FStructPropertyParams Params(
					InNameUTF8,
					InFlags,
					InArrayDim,
					0,
					InStructResolver,
					InMetaData,
					InNumMetaData
				);
				Params.MutableValueAccessor = InMutableValueAccessor;
				Params.ConstValueAccessor = InConstValueAccessor;
				return Params;
			}

			FStructResolver StructResolver = nullptr;
		};

		struct FStructParams
		{
			DStruct* (*StructNoRegisterFunc)();
			const char* QualifiedStructName;
			const char* ShortStructName;
			uint32 Size;
			uint32 Alignment;
			const FPropertyParamsBase* const* PropertyParams;
			size_t NumProperties;
			const FDStructOps* Ops = nullptr;
		};

		COREDOBJECT_API auto ConstructDClass(const FClassParams& Params) -> DClass*;
		COREDOBJECT_API auto ConstructDEnum(const FEnumParams& Params) -> DEnum*;
		COREDOBJECT_API auto ConstructDStruct(const FStructParams& Params) -> DStruct*;

	} // namespace DurinCodeGen
}
