#pragma once

#include "DObject/AssetPath.h"
#include "DObject/WeakObjectPtr.h"

	namespace Durin
	{
		class DClass;
		class FAssetLoadService;

	enum class ESoftObjectPtrState : uint8 { Null, Pending, Valid, Stale };
	COREDOBJECT_API auto GetSoftObjectCacheEpoch() -> uint64;
	COREDOBJECT_API auto InvalidateSoftObjectCaches() -> void;

	class FSoftObjectPtr
	{
	public:
		FSoftObjectPtr() = default;
		FSoftObjectPtr(std::nullptr_t) {}
		explicit FSoftObjectPtr(FObjectPath InPath) : AuthoredPath(std::move(InPath)) {}
		FSoftObjectPtr(const FSoftObjectPtr&) = default;
		auto operator=(const FSoftObjectPtr&) -> FSoftObjectPtr& = default;
		COREDOBJECT_API FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept;
		COREDOBJECT_API auto operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&;
		auto operator=(std::nullptr_t) -> FSoftObjectPtr& { Reset(); return *this; }

		COREDOBJECT_API auto SetPath(FObjectPath InPath) -> void;
		COREDOBJECT_API auto TrySetObject(DObject* InObject, const DClass* ExpectedClass = nullptr, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API auto TrySetLoadedObject(DObject* InObject, const DClass* ExpectedClass = nullptr, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API auto Get(const DClass* ExpectedClass = nullptr) const -> DObject*;
		COREDOBJECT_API auto GetState(const DClass* ExpectedClass = nullptr) const -> ESoftObjectPtrState;
		auto IsLoaded(const DClass* ExpectedClass = nullptr) const -> bool { return Get(ExpectedClass) != nullptr; }
		auto IsNull() const -> bool { return !AuthoredPath.IsValid(); }
		auto GetPath() const -> const FObjectPath& { return AuthoredPath; }
		auto Reset() -> void { AuthoredPath = {}; ResetCache(); }

		friend auto operator==(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> bool { return Left.AuthoredPath == Right.AuthoredPath; }
		friend auto operator<=>(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> std::strong_ordering { return Left.AuthoredPath <=> Right.AuthoredPath; }

	private:
		COREDOBJECT_API auto TrySetResolvedObject(DObject* InObject, const FObjectPath& AuthoredPath, const FObjectPath& ResolvedPath, const DClass* ExpectedClass, std::string* OutError) -> bool;
		auto ResetCache() -> void { WeakObject.Reset(); CacheEpoch = 0; }
		FObjectPath AuthoredPath;
		FWeakObjectPtr WeakObject;
		uint64 CacheEpoch = 0;
		friend class FAssetLoadService;
	};

	template<typename T>
	class TSoftObjectPtr
	{
	public:
		TSoftObjectPtr() = default;
		TSoftObjectPtr(std::nullptr_t) {}
		explicit TSoftObjectPtr(FObjectPath InPath) : SoftObjectPtr(std::move(InPath)) {}
		explicit TSoftObjectPtr(T* InObject) { (void)TrySetObject(InObject); }
		auto operator=(std::nullptr_t) -> TSoftObjectPtr& { Reset(); return *this; }
		auto operator=(T* InObject) -> TSoftObjectPtr& { (void)TrySetObject(InObject); return *this; }
		auto SetPath(FObjectPath InPath) -> void { SoftObjectPtr.SetPath(std::move(InPath)); }
		auto TrySetObject(T* InObject, std::string* OutError = nullptr) -> bool { return SoftObjectPtr.TrySetObject(ToDObject(InObject), GetExpectedClass(), OutError); }
		auto TrySetLoadedObject(T* InObject, std::string* OutError = nullptr) -> bool { return SoftObjectPtr.TrySetLoadedObject(ToDObject(InObject), GetExpectedClass(), OutError); }
		auto Get() const -> T* { return FromDObject(SoftObjectPtr.Get(GetExpectedClass())); }
		auto GetState() const -> ESoftObjectPtrState { return SoftObjectPtr.GetState(GetExpectedClass()); }
		auto IsLoaded() const -> bool { return Get() != nullptr; }
		auto IsNull() const -> bool { return SoftObjectPtr.IsNull(); }
		auto GetPath() const -> const FObjectPath& { return SoftObjectPtr.GetPath(); }
		auto Reset() -> void { SoftObjectPtr.Reset(); }
		auto GetBase() -> FSoftObjectPtr& { return SoftObjectPtr; }
		auto GetBase() const -> const FSoftObjectPtr& { return SoftObjectPtr; }
		friend auto operator==(const TSoftObjectPtr& Left, const TSoftObjectPtr& Right) -> bool { return Left.SoftObjectPtr == Right.SoftObjectPtr; }
		friend auto operator<=>(const TSoftObjectPtr& Left, const TSoftObjectPtr& Right) -> std::strong_ordering { return Left.SoftObjectPtr <=> Right.SoftObjectPtr; }
	private:
		static auto ToDObject(T* Object) -> DObject* { if constexpr (TIsCompleteType<T>::value) { static_assert(std::is_base_of_v<DObject, T>, "TSoftObjectPtr<T> requires T to derive from DObject"); return static_cast<DObject*>(Object); } else return reinterpret_cast<DObject*>(Object); }
		static auto FromDObject(DObject* Object) -> T* { if constexpr (TIsCompleteType<T>::value) { static_assert(std::is_base_of_v<DObject, T>, "TSoftObjectPtr<T> requires T to derive from DObject"); return static_cast<T*>(Object); } else return reinterpret_cast<T*>(Object); }
		static auto GetExpectedClass() -> DClass* { static_assert(std::is_base_of_v<DObject, T>, "TSoftObjectPtr<T> requires T to derive from DObject"); return T::StaticClass(); }
		FSoftObjectPtr SoftObjectPtr;
	};
}

template<> struct std::hash<Durin::FSoftObjectPtr> { auto operator()(const Durin::FSoftObjectPtr& Value) const noexcept -> size_t { return std::hash<Durin::FObjectPath>{}(Value.GetPath()); } };
template<typename T> struct std::hash<Durin::TSoftObjectPtr<T>> { auto operator()(const Durin::TSoftObjectPtr<T>& Value) const noexcept -> size_t { return std::hash<Durin::FObjectPath>{}(Value.GetPath()); } };
