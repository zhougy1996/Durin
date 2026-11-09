#pragma once

#include "CoreMinimal.h"

#include "DObject/AssetPath.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	enum class ESoftObjectError : uint8
	{
		InvalidObjectPath, PackageObject, TransientObject, ClassMismatch, UnpackagedObject,
	};

	struct FSoftObjectError
	{
		ESoftObjectError Code = ESoftObjectError::InvalidObjectPath;
		std::string Subject;
		std::string Expected;
		std::string Actual;
		std::optional<FObjectPathError> PathCause;
	};
	COREDOBJECT_API auto ToString(const FSoftObjectError& Error) -> std::string;

	class DClass;

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
		auto operator=(FObjectPath InPath) -> FSoftObjectPtr& { AuthoredPath = std::move(InPath); ResetWeakPtr(); return *this; }

		COREDOBJECT_API auto TrySetObject(DObject* InObject, const DClass* ExpectedClass = nullptr) -> std::expected<void, FSoftObjectError>;
		// Owner-thread lookup of an already loaded object; never loads a package.
		COREDOBJECT_API auto Get(const DClass* ExpectedClass = nullptr) const -> DObject*;
		COREDOBJECT_API auto LoadSynchronous(const DClass* ExpectedClass = nullptr) const -> DObject*;
		auto IsValid(const DClass* ExpectedClass = nullptr) const -> bool { return Get(ExpectedClass) != nullptr; }
		auto IsPending(const DClass* ExpectedClass = nullptr) const -> bool { return !IsNull() && !IsValid(ExpectedClass); }
		auto IsNull() const -> bool { return !AuthoredPath.IsValid(); }
		auto ToSoftObjectPath() const -> const FObjectPath& { return AuthoredPath; }
		auto Reset() -> void { AuthoredPath = {}; ResetWeakPtr(); }
		auto ResetWeakPtr() const -> void { WeakObject.Reset(); CacheEpoch = 0; }
		explicit operator bool() const { return IsValid(); }

		friend auto operator==(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> bool { return Left.AuthoredPath == Right.AuthoredPath; }
		friend auto operator<=>(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> std::strong_ordering { return Left.AuthoredPath <=> Right.AuthoredPath; }

	private:
		FObjectPath AuthoredPath;
		mutable FWeakObjectPtr WeakObject;
		mutable uint64 CacheEpoch = 0;
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
		auto operator=(FObjectPath InPath) -> TSoftObjectPtr& { SoftObjectPtr = std::move(InPath); return *this; }
		auto TrySetObject(T* InObject) -> std::expected<void, FSoftObjectError> { return SoftObjectPtr.TrySetObject(ToDObject(InObject), GetExpectedClass()); }
		auto Get() const -> T* { return FromDObject(SoftObjectPtr.Get(GetExpectedClass())); }
		auto LoadSynchronous() const -> T* { return FromDObject(SoftObjectPtr.LoadSynchronous(GetExpectedClass())); }
		auto IsValid() const -> bool { return Get() != nullptr; }
		auto IsPending() const -> bool { return !IsNull() && !IsValid(); }
		auto IsNull() const -> bool { return SoftObjectPtr.IsNull(); }
		auto ToSoftObjectPath() const -> const FObjectPath& { return SoftObjectPtr.ToSoftObjectPath(); }
		auto Reset() -> void { SoftObjectPtr.Reset(); }
		auto ResetWeakPtr() const -> void { SoftObjectPtr.ResetWeakPtr(); }
		explicit operator bool() const { return IsValid(); }
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

template<> struct std::hash<Durin::FSoftObjectPtr> { auto operator()(const Durin::FSoftObjectPtr& Value) const noexcept -> size_t { return std::hash<Durin::FObjectPath>{}(Value.ToSoftObjectPath()); } };
template<typename T> struct std::hash<Durin::TSoftObjectPtr<T>> { auto operator()(const Durin::TSoftObjectPtr<T>& Value) const noexcept -> size_t { return std::hash<Durin::FObjectPath>{}(Value.ToSoftObjectPath()); } };
