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

		// Native storage identity shared by typed access and detached field construction.
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

		// The parent dies before its owned children. Aliasing shared ownership keeps
		// concrete destructors and child lifetimes without capture-based deleters.
		template<typename P> class TFunctionPropertyTreeBuilder
		{
			struct FStorage
			{
				std::vector<std::shared_ptr<FProperty>> Children;
				P Property;
				template<typename... Args> explicit FStorage(Args&&... Arguments)
					: Property(std::forward<Args>(Arguments)...) {}
			};
			std::shared_ptr<FStorage> Storage;
		public:
			template<typename... Args> explicit TFunctionPropertyTreeBuilder(Args&&... Arguments)
				: Storage(std::make_shared<FStorage>(std::forward<Args>(Arguments)...)) {}
			TFunctionPropertyTreeBuilder(const TFunctionPropertyTreeBuilder&) = delete;
			auto operator=(const TFunctionPropertyTreeBuilder&) -> TFunctionPropertyTreeBuilder& = delete;
			auto Get() -> P& { return Storage->Property; }
			auto AddChild(std::shared_ptr<FProperty> Child) -> FProperty*
			{
				Child->Owner = FFieldVariant(&Get());
				Storage->Children.push_back(std::move(Child));
				return Storage->Children.back().get();
			}
			auto Finish() && -> std::shared_ptr<P>
			{
				auto* Property = &Get();
				return {std::move(Storage), Property};
			}
		};
		template<typename T> auto BuildFunctionProperty(const char* Name, uint16 Offset) -> std::shared_ptr<FProperty>;

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
		const auto& Fields = GetFunction().GetSignature().Parameters;
		require(Index < Fields.size() && Fields[Index].Property);
		return Private::FunctionValue<T>(*Fields[Index].Property, GetParameters());
	}

	// Owns initialized native parameter aggregate storage. Metadata/module lifetime must
	// cover this scope; GetView() is borrowed by the synchronous ProcessEvent call.
	class FFunctionParameterBuffer final
	{
	public:
		COREDOBJECT_API explicit FFunctionParameterBuffer(const FFunction& Function);
		COREDOBJECT_API ~FFunctionParameterBuffer();
		FFunctionParameterBuffer(const FFunctionParameterBuffer&) = delete;
		auto operator=(const FFunctionParameterBuffer&) -> FFunctionParameterBuffer& = delete;
		auto GetView() const -> FMutableByteView { return {static_cast<std::byte*>(Storage), Signature.ParmsSize}; }
		template<typename T> auto GetParameter(size_t Index) -> T&
		{
			require(Index < Signature.Parameters.size());
			return Private::FunctionValue<T>(*Signature.Parameters[Index].Property, GetView());
		}
		template<typename T> auto GetReturnValue() -> T&
		{
			require(Signature.ReturnProperty);
			return Private::FunctionValue<T>(*Signature.ReturnProperty, GetView());
		}
	private:
		const FFunctionSignature& Signature;
		void* Storage = nullptr;
		friend auto Private::AddFunctionParameterReferences(FReferenceCollector&) -> void;
	};
}

namespace Durin::DurinCodeGen
{
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

	// Publish only the completed tree; generated code and callers retain the same API.
	template<typename T> auto MakeFunctionProperty(const char* Name, uint16 Offset) -> std::shared_ptr<const FProperty>
	{
		return Private::BuildFunctionProperty<T>(Name, Offset);
	}

	template<typename T> auto MakeFunctionSignature(std::vector<FFunctionParameter> Parameters,
		std::shared_ptr<const FProperty> ReturnProperty = {}) -> FFunctionSignature
	{
		static_assert(sizeof(T) <= std::numeric_limits<uint16>::max(), "Native function parameter aggregate is too large.");
		return {std::move(Parameters), std::move(ReturnProperty), sizeof(T), alignof(T),
			&InitializePropertyValue<T>, &DestroyPropertyValue<T>};
	}
}

namespace Durin::Private
{
	template<typename T> auto BuildFunctionProperty(const char* Name, uint16 Offset) -> std::shared_ptr<FProperty>
	{
		static_assert(std::is_default_constructible_v<T> && std::is_copy_constructible_v<T>
			&& std::is_copy_assignable_v<T> && std::is_destructible_v<T>, "Function values require native value semantics.");
		static_assert(sizeof(T) <= std::numeric_limits<uint16>::max());
		using Traits = TNativePropertyTraits<T>;
		using P = typename Traits::PropertyType;
		using K = DurinCodeGen::EPropertyGenFlags;
		using namespace DurinCodeGen;
		constexpr auto Kind = Traits::Kind;
		constexpr auto ValueSize = static_cast<uint16>(sizeof(T));
		auto InstallOps = [](std::shared_ptr<P> Property) -> std::shared_ptr<FProperty> {
			const auto Ops = MakePropertyValueOps<T>();
			Property->SetValueLifecycle(Ops.ValueSize, Ops.ValueAlignment, Ops.InitializeValue, Ops.DestroyValue,
				Ops.CopyConstructValue, Ops.CopyAssignValue);
			return Property;
		};
		if constexpr (Kind == K::Enum)
		{
			auto* Enum = Traits::GetEnum();
			require(Enum && Enum->GetUnderlyingSize() == sizeof(T) && Enum->GetUnderlyingType() == Traits::UnderlyingType);
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, Offset, ValueSize, Kind, nullptr, Enum));
		}
		else if constexpr (Kind == K::SoftObject)
		{
			using Target = typename Traits::Target;
			static_assert(std::is_base_of_v<DObject, Target> && !std::is_const_v<Target>);
			const auto Params = FSoftObjectPropertyParams::Create<T>(Name, EPropertyFlags::None, 1, Offset, nullptr);
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, Offset, ValueSize, Traits::GetReferencedClass(), Params.MutableSoftValueAccessor, Params.ConstSoftValueAccessor));
		}
		else if constexpr (Kind == K::WeakObject)
		{
			using Target = typename Traits::Target;
			static_assert(std::is_base_of_v<DObject, Target> && !std::is_const_v<Target>);
			const auto Params = FWeakObjectPropertyParams::Create<T>(Name, EPropertyFlags::Transient, 1, Offset, nullptr);
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::Transient, 1, Offset, ValueSize, Traits::GetReferencedClass(), Params.MutableWeakValueAccessor, Params.ConstWeakValueAccessor));
		}
		else if constexpr (Kind == K::Object)
		{
			using Target = typename Traits::Target;
			static_assert(std::is_base_of_v<DObject, Target> && !std::is_const_v<Target>);
			const auto Params = [] {
				if constexpr (Traits::Wrapper) return FObjectPropertyParams::ObjectPtr<Target>(nullptr, EPropertyFlags::None, 1, 0, nullptr);
				else return FObjectPropertyParams::Raw<Target>(nullptr, EPropertyFlags::None, 1, 0, nullptr);
			}();
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, Offset, ValueSize, Kind, Traits::GetReferencedClass(), Traits::Wrapper,
				Params.ReadObjectValue, Params.WriteObjectValue));
		}
		else if constexpr (Kind == K::Array)
		{
			TFunctionPropertyTreeBuilder<P> Builder(FFieldVariant(nullptr), FName(Name),
				EObjectFlags::NoFlags, EPropertyFlags::None, 1, Offset, ValueSize, Kind, nullptr, ResolveArrayOps<T>());
			Builder.Get().SetInner(Builder.AddChild(BuildFunctionProperty<typename Traits::Element>("Inner", 0)));
			return InstallOps(std::move(Builder).Finish());
		}
		else if constexpr (Kind == K::Map)
		{
			TFunctionPropertyTreeBuilder<P> Builder(FFieldVariant(nullptr), FName(Name),
				EObjectFlags::NoFlags, EPropertyFlags::None, 1, Offset, ValueSize, Kind, nullptr, ResolveMapOps<T>());
			Builder.Get().SetKeyProp(Builder.AddChild(BuildFunctionProperty<typename Traits::Key>("Key", 0)));
			Builder.Get().SetValueProp(Builder.AddChild(BuildFunctionProperty<typename Traits::Mapped>("Value", 0)));
			return InstallOps(std::move(Builder).Finish());
		}
		else if constexpr (Kind == K::Struct)
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, Offset, Traits::GetStruct()));
		else
		{
			return InstallOps(std::make_shared<P>(FFieldVariant(nullptr), FName(Name), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, Offset, ValueSize, Kind, nullptr));
		}
	}
}
