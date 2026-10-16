#pragma once

#include "CoreMinimal.h"
#include "DObject/Function.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include "DObject/ObjectPtr.h"
#include "DObject/MathStructs.h"
#include "DObject/NativeEnum.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	namespace Private
	{
		template<typename T, typename = void> struct TFunctionStruct { static constexpr bool Value = false; };
		template<typename T> struct TFunctionStruct<T, std::void_t<decltype(T::StaticStruct())>>
		{
			static constexpr bool Value = true;
			static auto Get() -> DStruct* { return T::StaticStruct(); }
		};
		// Intrinsic value structs have metadata factories instead of GENERATED_BODY.
		#define DURIN_FUNCTION_INTRINSIC_STRUCT(Type) \
			template<> struct TFunctionStruct<Type> { \
				static constexpr bool Value = true; \
				static auto Get() -> DStruct* { return Z_Construct_DStruct_##Type(); } };
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector2f)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector3f)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector4f)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector2)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector3)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FVector4)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FQuatf)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FQuat)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FMatrix4f)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FTransform)
		DURIN_FUNCTION_INTRINSIC_STRUCT(FLinearColor)
		#undef DURIN_FUNCTION_INTRINSIC_STRUCT

		// Native storage identity shared by typed access and generated property records.
		template<DurinCodeGen::EPropertyGenFlags InKind, typename P> struct TNativePropertyType
		{
			static constexpr auto Kind = InKind;
			using PropertyType = P;
		};
		template<typename T, typename = void> struct TNativePropertyTraits
		{
			static_assert(!std::is_same_v<T, T>, "Unsupported reflected function value type.");
		};
		#define DURIN_NATIVE_SCALAR_PROPERTY(Type, Kind, Property) \
			template<> struct TNativePropertyTraits<Type> : TNativePropertyType<DurinCodeGen::EPropertyGenFlags::Kind, Property> {};
		DURIN_NATIVE_SCALAR_PROPERTY(bool, Bool, FBoolProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(int8, Int8, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(int16, Int16, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(int32, Int32, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(int64, Int64, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(uint8, UInt8, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(uint16, UInt16, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(uint32, UInt32, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(uint64, UInt64, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(float, Float, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(double, Double, FNumericProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(std::string, String, FStringProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(FName, Name, FNameProperty)
		DURIN_NATIVE_SCALAR_PROPERTY(FGuid, Guid, FGuidProperty)
		#undef DURIN_NATIVE_SCALAR_PROPERTY

		template<typename T, DurinCodeGen::EPropertyGenFlags Kind, typename P>
		struct TNativeObjectPropertyType : TNativePropertyType<Kind, P>
		{
			using Target = T;
			static auto GetReferencedClass() -> DClass* { return T::StaticClass(); }
		};
		template<typename T> struct TNativePropertyTraits<T*>
			: TNativeObjectPropertyType<T, DurinCodeGen::EPropertyGenFlags::Object, FObjectProperty>
		{ static constexpr bool Wrapper = false; };
		template<typename T> struct TNativePropertyTraits<TObjectPtr<T>>
			: TNativeObjectPropertyType<T, DurinCodeGen::EPropertyGenFlags::Object, FObjectProperty>
		{ static constexpr bool Wrapper = true; };
		template<typename T> struct TNativePropertyTraits<TSoftObjectPtr<T>>
			: TNativeObjectPropertyType<T, DurinCodeGen::EPropertyGenFlags::SoftObject, FSoftObjectProperty> {};
		template<typename T> struct TNativePropertyTraits<TWeakObjectPtr<T>>
			: TNativeObjectPropertyType<T, DurinCodeGen::EPropertyGenFlags::WeakObject, FWeakObjectProperty> {};
		template<typename T> struct TNativePropertyTraits<std::vector<T>>
			: TNativePropertyType<DurinCodeGen::EPropertyGenFlags::Array, FArrayProperty>
		{ using Element = T; };
		template<typename K, typename V> struct TNativePropertyTraits<std::unordered_map<K, V>>
			: TNativePropertyType<DurinCodeGen::EPropertyGenFlags::Map, FMapProperty>
		{ using Key = K; using Mapped = V; };
		template<typename T> struct TNativePropertyTraits<T, std::enable_if_t<std::is_enum_v<T>>>
			: TNativePropertyType<DurinCodeGen::EPropertyGenFlags::Enum, FEnumProperty>
		{
			static auto GetEnum() -> DEnum* { return StaticEnum<T>(); }
			static constexpr auto UnderlyingType = DurinCodeGen::NativeEnumUnderlyingType<T>();
		};
		template<typename T> struct TNativePropertyTraits<T, std::enable_if_t<TFunctionStruct<T>::Value>>
			: TNativePropertyType<DurinCodeGen::EPropertyGenFlags::Struct, FStructProperty>
		{
			static auto GetStruct() -> DStruct* { return TFunctionStruct<T>::Get(); }
		};

		template<typename T> auto FunctionTypeMatches(const FProperty& Property) -> bool
		{
			using Traits = TNativePropertyTraits<T>;
			using K = DurinCodeGen::EPropertyGenFlags;
			constexpr auto Kind = Traits::Kind;
			if (Property.GetKind() != Kind || Property.GetValueSize() != sizeof(T)
				|| Property.GetValueAlignment() != alignof(T)) return false;
			if constexpr (Kind == K::Enum)
				return static_cast<const FEnumProperty&>(Property).GetEnum() == Traits::GetEnum()
					&& static_cast<const FEnumProperty&>(Property).GetUnderlyingType() == Traits::UnderlyingType;
			else if constexpr (Kind == K::Object || Kind == K::SoftObject || Kind == K::WeakObject)
			{
				if (Property.GetReferencedClass() != Traits::GetReferencedClass()) return false;
				if constexpr (Kind == K::Object) return Property.IsObjectPtrWrapper() == Traits::Wrapper;
				else return true;
			}
			else if constexpr (Kind == K::Array)
			{
				const auto* Inner = static_cast<const FArrayProperty&>(Property).GetInner();
				return Inner && FunctionTypeMatches<typename Traits::Element>(*Inner);
			}
			else if constexpr (Kind == K::Map)
			{
				const auto& Map = static_cast<const FMapProperty&>(Property);
				return Map.GetKeyProp() && Map.GetValueProp()
					&& FunctionTypeMatches<typename Traits::Key>(*Map.GetKeyProp())
					&& FunctionTypeMatches<typename Traits::Mapped>(*Map.GetValueProp());
			}
			else if constexpr (Kind == K::Struct)
				return static_cast<const FStructProperty&>(Property).GetStruct() == Traits::GetStruct();
			else return true;
		}

		template<typename T> auto FunctionValue(const FProperty& Property, FMutableByteView Storage) -> T&
		{
			require(FunctionTypeMatches<T>(Property));
			require(Property.GetOffset() <= Storage.size() && sizeof(T) <= Storage.size() - Property.GetOffset());
			auto* Address = Property.GetValuePtr(Storage.data());
			require(reinterpret_cast<uintptr_t>(Address) % alignof(T) == 0);
			return *static_cast<T*>(Address);
		}
	}

	template<typename T> auto FFunctionFrame::GetParameter(size_t Index) const -> T&
	{
		return Private::FunctionValue<T>(*GetFunction().GetParameterProperty(Index), GetParameters());
	}

	// Owns initialized native parameter aggregate storage. Metadata/module lifetime must
	// cover this scope; GetView() is borrowed by the synchronous ProcessEvent call.
	class FFunctionParameterBuffer final
	{
	public:
		COREDOBJECT_API explicit FFunctionParameterBuffer(const DFunction& Function);
		COREDOBJECT_API ~FFunctionParameterBuffer();
		FFunctionParameterBuffer(const FFunctionParameterBuffer&) = delete;
		auto operator=(const FFunctionParameterBuffer&) -> FFunctionParameterBuffer& = delete;
		auto GetView() const -> FMutableByteView { return {static_cast<std::byte*>(Storage), Function.GetParmsSize()}; }
		template<typename T> auto GetParameter(size_t Index) -> T&
		{
			return Private::FunctionValue<T>(*Function.GetParameterProperty(Index), GetView());
		}
		template<typename T> auto GetReturnValue() -> T&
		{
			require(Function.GetReturnProperty());
			return Private::FunctionValue<T>(*Function.GetReturnProperty(), GetView());
		}
	private:
		const DFunction& Function;
		void* Storage = nullptr;
		friend auto Private::AddFunctionParameterReferences(FReferenceCollector&) -> void;
	};
}

namespace Durin::DurinCodeGen
{
	// These holders keep nested generated records alive until the shared constructor
	// consumes them. All concrete field creation stays in ConstructProperty.
	template<typename T> auto MakeNativeLeafPropertyParams(const char* Name, uint32 Offset, EPropertyFlags Flags)
	{
		using Traits = Private::TNativePropertyTraits<T>;
		constexpr auto Kind = Traits::Kind;
		using K = EPropertyGenFlags;
		if constexpr (Kind == K::Enum) return FEnumPropertyParams(Name, Flags, 1, Offset, &Traits::GetEnum);
		else if constexpr (Kind == K::Struct) return FStructPropertyParams(Name, Flags, 1, Offset, &Traits::GetStruct);
		else if constexpr (Kind == K::SoftObject)
			return FSoftObjectPropertyParams::Create<T>(Name, Flags, 1, Offset, &Traits::GetReferencedClass);
		else if constexpr (Kind == K::WeakObject)
			return FWeakObjectPropertyParams::Create<T>(Name, Flags | EPropertyFlags::Transient, 1, Offset, &Traits::GetReferencedClass);
		else if constexpr (Kind == K::Object)
		{
			using Target = typename Traits::Target;
			static_assert(std::is_base_of_v<DObject, Target> && !std::is_const_v<Target>);
			if constexpr (Traits::Wrapper) return FObjectPropertyParams::ObjectPtr<Target>(Name, Flags, 1, Offset, &Traits::GetReferencedClass);
			else return FObjectPropertyParams::Raw<Target>(Name, Flags, 1, Offset, &Traits::GetReferencedClass);
		}
		else return TPlainPropertyParams<T, Kind>(Name, Flags, 1, Offset);
	}

	template<typename T, EPropertyGenFlags Kind = Private::TNativePropertyTraits<T>::Kind>
	struct TNativePropertyParams
	{
		decltype(MakeNativeLeafPropertyParams<T>(nullptr, 0, EPropertyFlags::None)) Params;
		TNativePropertyParams(const char* Name, uint32 Offset, EPropertyFlags Flags = EPropertyFlags::None)
			: Params(MakeNativeLeafPropertyParams<T>(Name, Offset, Flags))
		{ Params.NativeValueOps = MakePropertyValueOps<T>(); }
		TNativePropertyParams(const TNativePropertyParams&) = delete;
		auto operator=(const TNativePropertyParams&) -> TNativePropertyParams& = delete;
	};
	template<typename T> struct TNativePropertyParams<T, EPropertyGenFlags::Array>
	{
		TNativePropertyParams<typename Private::TNativePropertyTraits<T>::Element> Inner;
		FArrayPropertyParams Params;
		TNativePropertyParams(const char* Name, uint32 Offset, EPropertyFlags Flags = EPropertyFlags::None)
			: Inner("Inner", 0), Params(Name, Flags, 1, Offset, &Inner.Params, &ResolveArrayOps<T>)
		{ Params.NativeValueOps = MakePropertyValueOps<T>(); }
		TNativePropertyParams(const TNativePropertyParams&) = delete;
		auto operator=(const TNativePropertyParams&) -> TNativePropertyParams& = delete;
	};
	template<typename T> struct TNativePropertyParams<T, EPropertyGenFlags::Map>
	{
		TNativePropertyParams<typename Private::TNativePropertyTraits<T>::Key> Key;
		TNativePropertyParams<typename Private::TNativePropertyTraits<T>::Mapped> Value;
		FMapPropertyParams Params;
		TNativePropertyParams(const char* Name, uint32 Offset, EPropertyFlags Flags = EPropertyFlags::None)
			: Key("Key", 0), Value("Value", 0), Params(Name, Flags, 1, Offset, &Key.Params, &Value.Params, &ResolveMapOps<T>)
		{ Params.NativeValueOps = MakePropertyValueOps<T>(); }
		TNativePropertyParams(const TNativePropertyParams&) = delete;
		auto operator=(const TNativePropertyParams&) -> TNativePropertyParams& = delete;
	};

	template<typename T> constexpr auto MakeFunctionLayoutParams(const FPropertyParamsBase* const* Properties, size_t NumProperties) -> FFunctionLayoutParams
	{
		static_assert(sizeof(T) <= std::numeric_limits<uint16>::max());
		return {Properties, NumProperties, sizeof(T), alignof(T), &InitializePropertyValue<T>, &DestroyPropertyValue<T>};
	}


	// Generated assertions compare extracted schemas with the actual target-compiler
	// method type, preventing implicit conversions from hiding a parser/type mismatch.
	template<typename T> struct TNativeFunctionTraits;
	template<typename R, typename C, typename... Args> struct TNativeFunctionTraits<R(C::*)(Args...)>
	{
		using ReturnType = R;
		static constexpr size_t NumArguments = sizeof...(Args);
		template<size_t Index> using ArgumentType = std::tuple_element_t<Index, std::tuple<Args...>>;
		static constexpr bool IsConst = false;
	};
	template<typename R, typename C, typename... Args> struct TNativeFunctionTraits<R(C::*)(Args...) const>
		: TNativeFunctionTraits<R(C::*)(Args...)> { static constexpr bool IsConst = true; };
	template<typename R, typename C, typename... Args> struct TNativeFunctionTraits<R(C::*)(Args...) noexcept>
		: TNativeFunctionTraits<R(C::*)(Args...)> {};
	template<typename R, typename C, typename... Args> struct TNativeFunctionTraits<R(C::*)(Args...) const noexcept>
		: TNativeFunctionTraits<R(C::*)(Args...) const> {};
}
