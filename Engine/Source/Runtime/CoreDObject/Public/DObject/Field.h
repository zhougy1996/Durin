#pragma once

#include "CoreDObjectAPI.h"
#include "ObjectMacros.h"

namespace Durin
{
	class DObject;
	class DClass;
	class FField;
	class FFieldVariant;

	using FFieldConstructFuncType = FField* (*)(const FFieldVariant&, const FName&, EObjectFlags);

	class FFieldClass
	{
		/** Name of this field class */
		FName Name;
		/** Unique id of this field class (for casting) */
		uint64 Id;
		/** Cast flags used for casting to other classes */
		uint64 CastFlags;
		/** Class flags */
		EClassFlags ClassFlags;
		/** Super of this class */
		FFieldClass* SuperClass;
		/** Function to construct an instance of this class */
		FFieldConstructFuncType ConstructFunc;

		/** Default instance of this class */
		// FField* DefaultObject;

	public:
		COREDOBJECT_API FFieldClass(const char* InCPPName, uint64 InId, uint64 InCastFlags, FFieldClass* InSuperClass, FFieldConstructFuncType InConstructFunc);

		inline auto GetId() const -> uint64 { return Id; }

		inline auto IsChildOf(const FFieldClass* InClass) const -> bool
		{
			const uint64 InClassId = InClass->GetId();
			return (Id & InClassId) == InClassId;
		}

		/** Get all registered field classes */
		static COREDOBJECT_API auto GetAllFieldClasses() -> std::vector<FFieldClass*>&;

		/** Get a mapping from name to field class */
		static COREDOBJECT_API auto GetNameToFieldClassMap() -> std::unordered_map<FName, FFieldClass*>;

		auto Construct(const FFieldVariant& InOwner, const FName& InName, EObjectFlags InFlags = EObjectFlags::NoFlags) const -> FField*
		{
			return ConstructFunc(InOwner, InName, InFlags);
		}
	};

	#define DECLARE_FIELD(TClass, TSuperClass, TStaticFlags, TRequiredAPI) \
		public: \
		TClass& operator=(TClass&&) = delete; \
		TClass& operator=(const TClass&) = delete; \
		\
		using Super = DOGE_REMOVE_OPTIONAL_PARENS(TSuperClass); \
		static TRequiredAPI auto StaticClass() -> FFieldClass*; \
		static TRequiredAPI FField* Construct(const FFieldVariant& InOwner, const FName& InName, EObjectFlags InFlags = EObjectFlags::NoFlags); \
		/* Internal ClassCastFlags without super class flags */ \
		inline static constexpr auto StaticClassCastFlagsPrivate() -> uint64 { return static_cast<uint64>(TStaticFlags); } \
		/* ClassCastFlags including super class flags, also used as id in FFieldClass */ \
		inline static constexpr auto StaticClassCastFlags() -> uint64 { return static_cast<uint64>(TStaticFlags) | Super::StaticClassCastFlags(); }

	#define IMPLEMENT_FIELD(TClass, TSuperClass, TStaticFlags, TRequiredAPI) \
		TRequiredAPI auto TClass::StaticClass() -> FFieldClass* \
		{ \
		static FFieldClass StaticFieldClass(STR(#TClass), TClass::StaticClassCastFlagsPrivate(), TClass::StaticClassCastFlags(), TSuperClass::StaticClass(), &TClass::Construct); \
		return &StaticFieldClass; \
		} \
		TRequiredAPI FField* TClass::Construct(const FFieldVariant& InOwner, const FName& InName, EObjectFlags InFlags) \
		{ \
		return new TClass(InOwner, InName, InFlags); \
		}

	class FFieldVariant
	{
		union FFieldObjectUnion
		{
			FField* Field;
			DObject* Object;
		} Container{};

		static constexpr uintptr_t DObjectMask = 0x1;

	public:

		FFieldVariant()
		{
			Container.Field = nullptr;
		}

		explicit FFieldVariant(const FField* InField)
		{
			Container.Field = const_cast<FField*>(InField);
		}

		explicit FFieldVariant(DObject* InObject)
		{
			Container.Object = reinterpret_cast<DObject*>(reinterpret_cast<uintptr_t>(InObject) | DObjectMask);
			// TODO: mark as reachable
		}

		explicit FFieldVariant(decltype(nullptr))
			: FFieldVariant()
		{
		}

		inline auto IsDObject() const -> bool
		{
			return !!ToDObjectUnSafe();
		}

		/** For internal use only, return as a FField pointer without checking if it is actually a FField */
		inline auto ToFieldUnSafe() const -> FField*
		{
			return Container.Field;
		}

		/**  For internal use only, return as a DObject pointer without checking if it is actually a DObject */
		inline auto ToDObjectUnSafe() const -> DObject*
		{
			return reinterpret_cast<DObject*>(reinterpret_cast<uintptr_t>(Container.Object) & ~DObjectMask);
		}

		inline auto IsValid() const -> bool
		{
			return Container.Field != nullptr;
		}

		auto ToDObject() const -> DObject*
		{
			if (IsDObject())
			{
				return ToDObjectUnSafe();
			}
			return nullptr;
		}

		auto ToField() const -> FField*
		{
			if (!IsDObject())
			{
				return Container.Field;
			}
			return nullptr;
		}

		COREDOBJECT_API auto IsA(const DClass* InClass) const -> bool;
	};

	class FField
	{
	public:
		DOGE_NONCOPYABLE(FField)

		static COREDOBJECT_API auto StaticClass() -> FFieldClass*;

		static COREDOBJECT_API FField* Construct(const FFieldVariant& InOwner, const FName& InName, EObjectFlags InFlags = EObjectFlags::NoFlags);

		inline static constexpr auto StaticClassCastFlagsPrivate() -> uint64
		{
			return static_cast<uint64>(EClassCastFlags::FField);
		}

		inline static constexpr auto StaticClassCastFlags() -> uint64
		{
			return static_cast<uint64>(EClassCastFlags::FField);
		}

		FFieldClass* ClassPrivate = nullptr;

		FFieldVariant Owner;

		FField* Next;

		FName NamePrivate;

		EObjectFlags FlagsPrivate;

		COREDOBJECT_API FField(FFieldVariant InOwner, FName InName, EObjectFlags InFlags);

	public:

		COREDOBJECT_API auto SetMetaData(const FName& InKey, const std::string& InValue) -> void;

		COREDOBJECT_API auto GetMetaData(const FName& InKey) const -> const std::string&;

	private:

		std::unordered_map<FName, std::string>* MetaDataMap;
	};
}

