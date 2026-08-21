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
	class FSoftObjectPtr;
	struct FGuid;
	template<typename T>
	class TObjectPtr;

	// Identifies why an object is being constructed so constructors never infer lifecycle from identity.
	enum class EObjectConstructionPurpose : uint8
	{
		RuntimeObject,
		ClassDefaultObject,
		ClassDefaultSubobject,
		AssetLoad,
		Duplication,
		Generated,
	};

	constexpr auto IsTemplateConstructionPurpose(EObjectConstructionPurpose Purpose) -> bool
	{
		return Purpose == EObjectConstructionPurpose::ClassDefaultObject
			|| Purpose == EObjectConstructionPurpose::ClassDefaultSubobject;
	}

	// Collects the class, Outer, name, and flags needed to allocate one DObject.
	struct FStaticConstructObjectParameters
	{
		DClass* Class = nullptr;

		DObject* Outer = nullptr;

		FName Name;

		size_t Size = 0;

		EObjectConstructionPurpose Purpose = EObjectConstructionPurpose::RuntimeObject;
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

		EObjectConstructionPurpose Purpose = EObjectConstructionPurpose::RuntimeObject;
	};

	COREDOBJECT_API auto DObjectInit() -> void;
	// Returns whether process-wide DObject initialization has completed.
	COREDOBJECT_API auto IsDObjectInitialized() -> bool;

	COREDOBJECT_API auto DObjectForceRegistration(DObject* Object) -> void;

	COREDOBJECT_API auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*;

	// Identifies the exact scalar channel used by numeric property metadata.
	enum class EPropertyMetadataNumericKind : uint8
	{
		None,
		Signed,
		Unsigned,
		Float,
		Double,
	};

	enum class EPropertyUnit : uint8
	{
		None,
		Unitless,
		Percent,
		Degrees,
		Radians,
		Seconds,
		Milliseconds,
		Meters,
		Centimeters,
		Millimeters,
		Kilometers,
	};

	// Stores one metadata number without narrowing 64-bit integers through double.
	struct FPropertyMetadataNumber
	{
		EPropertyMetadataNumericKind Kind = EPropertyMetadataNumericKind::None;
		int64 Signed = 0;
		uint64 Unsigned = 0;
		float Float = 0.0f;
		double Double = 0.0;

		static constexpr auto FromSigned(int64 Value) -> FPropertyMetadataNumber { return {.Kind = EPropertyMetadataNumericKind::Signed, .Signed = Value}; }
		static constexpr auto FromUnsigned(uint64 Value) -> FPropertyMetadataNumber { return {.Kind = EPropertyMetadataNumericKind::Unsigned, .Unsigned = Value}; }
		static constexpr auto FromFloat(float Value) -> FPropertyMetadataNumber { return {.Kind = EPropertyMetadataNumericKind::Float, .Float = Value}; }
		static constexpr auto FromDouble(double Value) -> FPropertyMetadataNumber { return {.Kind = EPropertyMetadataNumericKind::Double, .Double = Value}; }
	};

	// Generated immutable first-party metadata copied into an FProperty at registration.
	struct FPropertyMetadataParams
	{
		const char* DisplayName = nullptr;
		const char* ToolTip = nullptr;
		const char* Category = nullptr;
		EPropertyUnit Units = EPropertyUnit::None;
		FPropertyMetadataNumber Step;
		int8 Precision = -1;
		FPropertyMetadataNumber ClampMin;
		FPropertyMetadataNumber ClampMax;
		FPropertyMetadataNumber UIMin;
		FPropertyMetadataNumber UIMax;
	};

	COREDOBJECT_API auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*;
	COREDOBJECT_API auto NewObject(DClass* Class, DObject* Outer, FName Name) -> DObject*;
	COREDOBJECT_API auto CanConstructObjectOfClass(const DClass* Class, const DClass* RequiredBaseClass) -> bool;

	template<typename T>
	auto NewObject(
		DObject* Outer,
		FName Name,
		EObjectConstructionPurpose Purpose = EObjectConstructionPurpose::RuntimeObject
	) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>, "T must be derived from DObject");

		FStaticConstructObjectParameters Params;
		Params.Class = T::StaticClass();
		Params.Outer = Outer;
		Params.Name = Name;
		Params.Size = sizeof(T);
		Params.Purpose = Purpose;

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
			Guid,
			SoftObject
		};

		enum class EPropertyParamLayout : uint8
		{
			Invalid = 0,
			Plain,
			Enum,
			Object,
			Generic,
			Struct,
			Array,
			Map,
			SoftObject
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

		template<typename T>
		auto CopyConstructPropertyValue(void* Destination, const void* Source) -> void
		{
			std::construct_at(static_cast<T*>(Destination), *static_cast<const T*>(Source));
		}

		template<typename T>
		auto CopyAssignPropertyValue(void* Destination, const void* Source) -> void
		{
			*static_cast<T*>(Destination) = *static_cast<const T*>(Source);
		}

		struct FPropertyValueOps
		{
			uint32 ValueSize = 0;
			uint32 ValueAlignment = 0;
			void (*InitializeValue)(void* Memory) = nullptr;
			void (*DestroyValue)(void* Memory) = nullptr;
			void (*CopyConstructValue)(void* Destination, const void* Source) = nullptr;
			void (*CopyAssignValue)(void* Destination, const void* Source) = nullptr;
		};

		template<typename TValue>
		constexpr auto MakePropertyValueOps() -> FPropertyValueOps
		{
			return {
				sizeof(TValue),
				alignof(TValue),
				&InitializePropertyValue<TValue>,
				&DestroyPropertyValue<TValue>,
				&CopyConstructPropertyValue<TValue>,
				&CopyAssignPropertyValue<TValue>
			};
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
			const char* const* LegacyNames = nullptr;
			size_t NumLegacyNames = 0;
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
			const char* const* LegacyNames = nullptr;
			size_t NumLegacyNames = 0;
		};

		struct FMetaDataPair
		{
			const char* Key;
			const char* Value;
		};

		struct FPropertyParamsBase
		{
			using FMutableValueAccessor = void* (*)(void* Container, uint32 ArrayIndex);
			using FConstValueAccessor = const void* (*)(const void* Container, uint32 ArrayIndex);

			const char* NameUTF8;
			EPropertyFlags Flags;
			uint16 ArrayDim;
			uint16 Offset;
			EPropertyGenFlags Kind;
			EPropertyParamLayout Layout;
			FMutableValueAccessor MutableValueAccessor;
			FConstValueAccessor ConstValueAccessor;
			const FMetaDataPair* MetaData;
			size_t NumMetaData;
			const char* const* LegacyNames = nullptr;
			size_t NumLegacyNames = 0;
			const FPropertyMetadataParams* TypedMetadata = nullptr;

		protected:
			constexpr FPropertyParamsBase(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				EPropertyGenFlags InKind,
				EPropertyParamLayout InLayout,
				FMutableValueAccessor InMutableValueAccessor = nullptr,
				FConstValueAccessor InConstValueAccessor = nullptr,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: NameUTF8(InNameUTF8)
				, Flags(InFlags)
				, ArrayDim(InArrayDim)
				, Offset(InOffset)
				, Kind(InKind)
				, Layout(InLayout)
				, MutableValueAccessor(InMutableValueAccessor)
				, ConstValueAccessor(InConstValueAccessor)
				, MetaData(InMetaData)
				, NumMetaData(InNumMetaData)
			{
			}
		};

		template<typename TParams>
		constexpr auto WithTypedMetadata(TParams Params, const FPropertyMetadataParams* Metadata) -> TParams
		{
			static_assert(std::is_base_of_v<FPropertyParamsBase, TParams>);
			Params.TypedMetadata = Metadata;
			return Params;
		}

		template<typename TParams>
		constexpr auto WithLegacyNames(
			TParams Params,
			const char* const* LegacyNames,
			size_t NumLegacyNames
		) -> TParams
		{
			static_assert(std::is_base_of_v<FPropertyParamsBase, TParams>);
			Params.LegacyNames = LegacyNames;
			Params.NumLegacyNames = NumLegacyNames;
			return Params;
		}

		template<typename TValue, EPropertyGenFlags Kind>
		inline constexpr bool TIsPlainPropertyMapping = false;

		template<> inline constexpr bool TIsPlainPropertyMapping<bool, EPropertyGenFlags::Bool> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<int8, EPropertyGenFlags::Int8> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<int16, EPropertyGenFlags::Int16> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<int32, EPropertyGenFlags::Int32> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<int64, EPropertyGenFlags::Int64> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<uint8, EPropertyGenFlags::UInt8> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<uint16, EPropertyGenFlags::UInt16> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<uint32, EPropertyGenFlags::UInt32> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<uint64, EPropertyGenFlags::UInt64> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<float, EPropertyGenFlags::Float> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<double, EPropertyGenFlags::Double> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<std::string, EPropertyGenFlags::String> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<FName, EPropertyGenFlags::Name> = true;
		template<> inline constexpr bool TIsPlainPropertyMapping<FGuid, EPropertyGenFlags::Guid> = true;

		template<typename TValue, EPropertyGenFlags PropertyKind>
		struct TPlainPropertyParams final : public FPropertyParamsBase
		{
			static_assert(TIsPlainPropertyMapping<TValue, PropertyKind>, "Unsupported plain property type/kind mapping.");
			using ValueType = TValue;

			constexpr TPlainPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, PropertyKind, EPropertyParamLayout::Plain, nullptr, nullptr, InMetaData, InNumMetaData
				  )
			{
			}

			static constexpr auto WithAccessors(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> TPlainPropertyParams
			{
				return TPlainPropertyParams(
					InNameUTF8, InFlags, InArrayDim, InMutableValueAccessor,
					InConstValueAccessor, InMetaData, InNumMetaData
				);
			}

		private:
			constexpr TPlainPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData,
				size_t InNumMetaData
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, 0, PropertyKind, EPropertyParamLayout::Plain, InMutableValueAccessor, InConstValueAccessor, InMetaData, InNumMetaData
				  )
			{
			}
		};

		using FInt8PropertyParams = TPlainPropertyParams<int8, EPropertyGenFlags::Int8>;
		using FInt16PropertyParams = TPlainPropertyParams<int16, EPropertyGenFlags::Int16>;
		using FInt32PropertyParams = TPlainPropertyParams<int32, EPropertyGenFlags::Int32>;
		using FInt64PropertyParams = TPlainPropertyParams<int64, EPropertyGenFlags::Int64>;
		using FUInt8PropertyParams = TPlainPropertyParams<uint8, EPropertyGenFlags::UInt8>;
		using FUInt16PropertyParams = TPlainPropertyParams<uint16, EPropertyGenFlags::UInt16>;
		using FUInt32PropertyParams = TPlainPropertyParams<uint32, EPropertyGenFlags::UInt32>;
		using FUInt64PropertyParams = TPlainPropertyParams<uint64, EPropertyGenFlags::UInt64>;
		using FFloatPropertyParams = TPlainPropertyParams<float, EPropertyGenFlags::Float>;
		using FDoublePropertyParams = TPlainPropertyParams<double, EPropertyGenFlags::Double>;
		using FBoolPropertyParams = TPlainPropertyParams<bool, EPropertyGenFlags::Bool>;
		using FStringPropertyParams = TPlainPropertyParams<std::string, EPropertyGenFlags::String>;
		using FNamePropertyParams = TPlainPropertyParams<FName, EPropertyGenFlags::Name>;
		using FGuidPropertyParams = TPlainPropertyParams<FGuid, EPropertyGenFlags::Guid>;

		struct FEnumPropertyParams final : public FPropertyParamsBase
		{
			using FEnumResolver = DEnum* (*)();

			constexpr FEnumPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FEnumResolver InEnumResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::Enum, EPropertyParamLayout::Enum, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, EnumResolver(InEnumResolver)
			{
			}

			FEnumResolver EnumResolver = nullptr;
		};

		struct FObjectPropertyParams final : public FPropertyParamsBase
		{
			enum class EStorage : uint8
			{
				Invalid = 0,
				Raw,
				ObjectPtr
			};

			using FClassResolver = DClass* (*)();
			using FReadObjectValue = DObject* (*)(const void* Value);
			using FWriteObjectValue = void (*)(void* Value, DObject* Object);

			template<typename TObject>
			static constexpr auto Raw(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FClassResolver InClassResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FObjectPropertyParams
			{
				using TValue = TObject*;
				static_assert(sizeof(TValue) <= std::numeric_limits<uint16>::max());
				return FObjectPropertyParams(
					InNameUTF8, InFlags, InArrayDim, InOffset, InClassResolver,
					EStorage::Raw, MakePropertyValueOps<TValue>(),
					&ReadRaw<TObject>, &WriteRaw<TObject>, InMetaData, InNumMetaData
				);
			}

			template<typename TObject>
			static constexpr auto ObjectPtr(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FClassResolver InClassResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FObjectPropertyParams
			{
				using TValue = TObjectPtr<TObject>;
				static_assert(sizeof(TValue) <= std::numeric_limits<uint16>::max());
				return FObjectPropertyParams(
					InNameUTF8, InFlags, InArrayDim, InOffset, InClassResolver,
					EStorage::ObjectPtr, MakePropertyValueOps<TValue>(),
					&ReadObjectPtr<TObject>, &WriteObjectPtr<TObject>, InMetaData, InNumMetaData
				);
			}

			FClassResolver ClassResolver = nullptr;
			EStorage Storage = EStorage::Invalid;
			FPropertyValueOps ValueOps;
			FReadObjectValue ReadObjectValue = nullptr;
			FWriteObjectValue WriteObjectValue = nullptr;

		private:
			template<typename TObject, typename = void>
			struct TIsCompleteObjectTarget : std::false_type
			{
			};

			template<typename TObject>
			struct TIsCompleteObjectTarget<TObject, std::void_t<decltype(sizeof(TObject))>> : std::true_type
			{
			};

			constexpr FObjectPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FClassResolver InClassResolver,
				EStorage InStorage,
				FPropertyValueOps InValueOps,
				FReadObjectValue InReadObjectValue,
				FWriteObjectValue InWriteObjectValue,
				const FMetaDataPair* InMetaData,
				size_t InNumMetaData
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::Object, EPropertyParamLayout::Object, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, ClassResolver(InClassResolver)
				, Storage(InStorage)
				, ValueOps(InValueOps)
				, ReadObjectValue(InReadObjectValue)
				, WriteObjectValue(InWriteObjectValue)
			{
			}

			template<typename TObject>
			static auto ToDObject(TObject* Object) -> DObject*
			{
				if constexpr (TIsCompleteObjectTarget<TObject>::value)
				{
					static_assert(std::is_base_of_v<DObject, TObject>);
					return static_cast<DObject*>(Object);
				}
				else
				{
					return reinterpret_cast<DObject*>(Object);
				}
			}

			template<typename TObject>
			static auto FromDObject(DObject* Object) -> TObject*
			{
				if constexpr (TIsCompleteObjectTarget<TObject>::value)
				{
					static_assert(std::is_base_of_v<DObject, TObject>);
					return static_cast<TObject*>(Object);
				}
				else
				{
					return reinterpret_cast<TObject*>(Object);
				}
			}

			template<typename TObject>
			static auto ReadRaw(const void* Value) -> DObject*
			{
				TObject* const* Object = static_cast<TObject* const*>(Value);
				return Object ? ToDObject(*Object) : nullptr;
			}

			template<typename TObject>
			static auto WriteRaw(void* Value, DObject* Object) -> void
			{
				if (Value) *static_cast<TObject**>(Value) = FromDObject<TObject>(Object);
			}

			template<typename TObject>
			static auto ReadObjectPtr(const void* Value) -> DObject*
			{
				const auto* Object = static_cast<const TObjectPtr<TObject>*>(Value);
				return Object ? ToDObject(Object->Get()) : nullptr;
			}

			template<typename TObject>
			static auto WriteObjectPtr(void* Value, DObject* Object) -> void
			{
				if (Value) *static_cast<TObjectPtr<TObject>*>(Value) = FromDObject<TObject>(Object);
			}
		};

		struct FGenericPropertyParams final : public FPropertyParamsBase
		{
			constexpr FGenericPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				uint16 InElementSize,
				FPropertyValueOps InValueOps,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::None, EPropertyParamLayout::Generic, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, ElementSize(InElementSize)
				, ValueOps(InValueOps)
			{
			}

			uint16 ElementSize = 0;
			FPropertyValueOps ValueOps;
		};

		struct FSoftObjectPropertyParams final : public FPropertyParamsBase
		{
			using FExpectedClassResolver = DClass* (*)();
			using FMutableSoftValueAccessor = FSoftObjectPtr* (*)(void* Value);
			using FConstSoftValueAccessor = const FSoftObjectPtr* (*)(const void* Value);
			using FMutableValueAccessor = void* (*)(void* Container, uint32 ArrayIndex);
			using FConstValueAccessor = const void* (*)(const void* Container, uint32 ArrayIndex);

			template<typename TValue>
			static constexpr auto Create(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FExpectedClassResolver InExpectedClassResolver,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FSoftObjectPropertyParams
			{
				static_assert(sizeof(TValue) <= std::numeric_limits<uint16>::max());
				return FSoftObjectPropertyParams(
					InNameUTF8, InFlags, InArrayDim, InOffset, InExpectedClassResolver,
					MakePropertyValueOps<TValue>(), &AccessMutableSoftValue<TValue>,
					&AccessConstSoftValue<TValue>, nullptr, nullptr, InMetaData, InNumMetaData
				);
			}

			template<typename TValue>
			static constexpr auto WithAccessors(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				FExpectedClassResolver InExpectedClassResolver,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData = nullptr,
				size_t InNumMetaData = 0
			) -> FSoftObjectPropertyParams
			{
				static_assert(sizeof(TValue) <= std::numeric_limits<uint16>::max());
				return FSoftObjectPropertyParams(
					InNameUTF8, InFlags, InArrayDim, 0, InExpectedClassResolver,
					MakePropertyValueOps<TValue>(), &AccessMutableSoftValue<TValue>,
					&AccessConstSoftValue<TValue>, InMutableValueAccessor,
					InConstValueAccessor, InMetaData, InNumMetaData
				);
			}

			FExpectedClassResolver ExpectedClassResolver = nullptr;
			FPropertyValueOps ValueOps;
			FMutableSoftValueAccessor MutableSoftValueAccessor = nullptr;
			FConstSoftValueAccessor ConstSoftValueAccessor = nullptr;

		private:
			template<typename TValue>
			static auto AccessMutableSoftValue(void* Value) -> FSoftObjectPtr*
			{
				return Value ? &static_cast<TValue*>(Value)->GetBase() : nullptr;
			}

			template<typename TValue>
			static auto AccessConstSoftValue(const void* Value) -> const FSoftObjectPtr*
			{
				return Value ? &static_cast<const TValue*>(Value)->GetBase() : nullptr;
			}

			constexpr FSoftObjectPropertyParams(
				const char* InNameUTF8,
				EPropertyFlags InFlags,
				uint16 InArrayDim,
				uint16 InOffset,
				FExpectedClassResolver InExpectedClassResolver,
				FPropertyValueOps InValueOps,
				FMutableSoftValueAccessor InMutableSoftValueAccessor,
				FConstSoftValueAccessor InConstSoftValueAccessor,
				FMutableValueAccessor InMutableValueAccessor,
				FConstValueAccessor InConstValueAccessor,
				const FMetaDataPair* InMetaData,
				size_t InNumMetaData
			)
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::SoftObject, EPropertyParamLayout::SoftObject, InMutableValueAccessor, InConstValueAccessor, InMetaData, InNumMetaData
				  )
				, ExpectedClassResolver(InExpectedClassResolver)
				, ValueOps(InValueOps)
				, MutableSoftValueAccessor(InMutableSoftValueAccessor)
				, ConstSoftValueAccessor(InConstSoftValueAccessor)
			{
			}
		};

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
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::Array, EPropertyParamLayout::Array, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, InnerParams(InInnerParams)
				, OpsResolver(InOpsResolver)
			{
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
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::Map, EPropertyParamLayout::Map, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, KeyParams(InKeyParams)
				, ValueParams(InValueParams)
				, OpsResolver(InOpsResolver)
			{
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
				: FPropertyParamsBase(
					  InNameUTF8, InFlags, InArrayDim, InOffset, EPropertyGenFlags::Struct, EPropertyParamLayout::Struct, nullptr, nullptr, InMetaData, InNumMetaData
				  )
				, StructResolver(InStructResolver)
			{
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
			const char* const* LegacyNames = nullptr;
			size_t NumLegacyNames = 0;
		};

		COREDOBJECT_API auto ConstructDClass(const FClassParams& Params) -> DClass*;
		COREDOBJECT_API auto ConstructDEnum(const FEnumParams& Params) -> DEnum*;
		COREDOBJECT_API auto ConstructDStruct(const FStructParams& Params) -> DStruct*;

	} // namespace DurinCodeGen
} // namespace Durin
