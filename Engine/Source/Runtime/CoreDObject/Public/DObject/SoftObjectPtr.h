#pragma once

#include "DObject/AssetPath.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DClass;
	namespace Asset
	{
		class FAssetManager;
	}

	// Stores the persistent identity of one package main asset without loading it.
	class FSoftObjectPath
	{
	public:
		FSoftObjectPath() = default;
		FSoftObjectPath(std::nullptr_t) {}
		explicit FSoftObjectPath(FAssetPath InPath)
			: AssetPath(std::move(InPath))
		{
		}

		COREDOBJECT_API static auto TryCreate(
			std::string_view InPath,
			FSoftObjectPath& OutPath,
			std::string* OutError = nullptr) -> bool;

		auto IsNull() const -> bool { return !AssetPath.IsValid(); }
		auto GetAssetPath() const -> const FAssetPath& { return AssetPath; }
		auto ToString() const -> const std::string& { return AssetPath.ToString(); }
		auto GetView() const -> std::string_view { return AssetPath.GetView(); }
		auto Reset() -> void { AssetPath = {}; }

		auto operator==(const FSoftObjectPath&) const -> bool = default;
		auto operator<(const FSoftObjectPath& Other) const -> bool
		{
			return GetView() < Other.GetView();
		}

	private:
		FAssetPath AssetPath;
	};

	// Combines persistent path identity with a non-owning cache of a loaded main asset.
	class FSoftObjectPtr
	{
	public:
		FSoftObjectPtr() = default;
		FSoftObjectPtr(std::nullptr_t) {}
		explicit FSoftObjectPtr(FSoftObjectPath InPath)
			: SoftObjectPath(std::move(InPath))
		{
		}
		explicit FSoftObjectPtr(FAssetPath InPath)
			: SoftObjectPath(std::move(InPath))
		{
		}

		FSoftObjectPtr(const FSoftObjectPtr&) = default;
		auto operator=(const FSoftObjectPtr&) -> FSoftObjectPtr& = default;
		COREDOBJECT_API FSoftObjectPtr(FSoftObjectPtr&& Other) noexcept;
		COREDOBJECT_API auto operator=(FSoftObjectPtr&& Other) noexcept -> FSoftObjectPtr&;

		auto operator=(std::nullptr_t) -> FSoftObjectPtr&
		{
			Reset();
			return *this;
		}

		COREDOBJECT_API auto SetPath(FSoftObjectPath InPath) -> void;
		auto SetPath(FAssetPath InPath) -> void
		{
			SetPath(FSoftObjectPath(std::move(InPath)));
		}

		// A null object resets the value. A non-null object must be a package main asset.
		COREDOBJECT_API auto TrySetObject(
			DObject* InObject,
			const DClass* ExpectedClass = nullptr,
			std::string* OutError = nullptr) -> bool;

		// Refreshes only the weak cache and requires the object to match the existing path.
		COREDOBJECT_API auto TrySetLoadedObject(
			DObject* InObject,
			const DClass* ExpectedClass = nullptr,
			std::string* OutError = nullptr) -> bool;

		COREDOBJECT_API auto Get(const DClass* ExpectedClass = nullptr) const -> DObject*;
		auto IsLoaded(const DClass* ExpectedClass = nullptr) const -> bool
		{
			return Get(ExpectedClass) != nullptr;
		}
		auto IsNull() const -> bool { return SoftObjectPath.IsNull(); }
		auto GetSoftObjectPath() const -> const FSoftObjectPath& { return SoftObjectPath; }
		auto Reset() -> void
		{
			SoftObjectPath.Reset();
			ResolvedPackagePath = {};
			WeakObject.Reset();
		}

		friend auto operator==(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> bool
		{
			return Left.SoftObjectPath == Right.SoftObjectPath;
		}
		friend auto operator<(const FSoftObjectPtr& Left, const FSoftObjectPtr& Right) -> bool
		{
			return Left.SoftObjectPath < Right.SoftObjectPath;
		}

	private:
		COREDOBJECT_API auto TrySetResolvedObject(
			DObject* InObject,
			const FAssetPath& AuthoredPath,
			const FAssetPath& ResolvedPath,
			const DClass* ExpectedClass,
			std::string* OutError) -> bool;
		auto ResetResolvedObject() -> void
		{
			ResolvedPackagePath = {};
			WeakObject.Reset();
		}

		FSoftObjectPath SoftObjectPath;
		FAssetPath ResolvedPackagePath;
		FWeakObjectPtr WeakObject;

		friend class Asset::FAssetManager;
	};

	// Provides typed access without exposing the wrapper's physical layout as reflection ABI.
	template<typename T>
	class TSoftObjectPtr
	{
	public:
		TSoftObjectPtr() = default;
		TSoftObjectPtr(std::nullptr_t) {}
		explicit TSoftObjectPtr(FSoftObjectPath InPath)
			: SoftObjectPtr(std::move(InPath))
		{
		}
		explicit TSoftObjectPtr(FAssetPath InPath)
			: SoftObjectPtr(std::move(InPath))
		{
		}

		auto operator=(std::nullptr_t) -> TSoftObjectPtr&
		{
			SoftObjectPtr.Reset();
			return *this;
		}

		auto SetPath(FSoftObjectPath InPath) -> void { SoftObjectPtr.SetPath(std::move(InPath)); }
		auto SetPath(FAssetPath InPath) -> void { SoftObjectPtr.SetPath(std::move(InPath)); }
		auto TrySetObject(DObject* InObject, std::string* OutError = nullptr) -> bool
		{
			return SoftObjectPtr.TrySetObject(InObject, GetExpectedClass(), OutError);
		}
		auto TrySetLoadedObject(DObject* InObject, std::string* OutError = nullptr) -> bool
		{
			return SoftObjectPtr.TrySetLoadedObject(InObject, GetExpectedClass(), OutError);
		}
		auto Get() const -> T*
		{
			return static_cast<T*>(SoftObjectPtr.Get(GetExpectedClass()));
		}
		auto IsLoaded() const -> bool { return Get() != nullptr; }
		auto IsNull() const -> bool { return SoftObjectPtr.IsNull(); }
		auto GetSoftObjectPath() const -> const FSoftObjectPath& { return SoftObjectPtr.GetSoftObjectPath(); }
		auto Reset() -> void { SoftObjectPtr.Reset(); }

		auto GetBase() -> FSoftObjectPtr& { return SoftObjectPtr; }
		auto GetBase() const -> const FSoftObjectPtr& { return SoftObjectPtr; }

		friend auto operator==(const TSoftObjectPtr& Left, const TSoftObjectPtr& Right) -> bool
		{
			return Left.SoftObjectPtr == Right.SoftObjectPtr;
		}
		friend auto operator<(const TSoftObjectPtr& Left, const TSoftObjectPtr& Right) -> bool
		{
			return Left.SoftObjectPtr < Right.SoftObjectPtr;
		}

	private:
		static auto GetExpectedClass() -> DClass*
		{
			static_assert(std::is_base_of_v<DObject, T>, "TSoftObjectPtr<T> requires T to derive from DObject");
			return T::StaticClass();
		}

		FSoftObjectPtr SoftObjectPtr;
	};
}

template<>
struct std::hash<Durin::FSoftObjectPath>
{
	auto operator()(const Durin::FSoftObjectPath& Value) const noexcept -> size_t
	{
		return std::hash<Durin::FAssetPath>{}(Value.GetAssetPath());
	}
};

template<>
struct std::hash<Durin::FSoftObjectPtr>
{
	auto operator()(const Durin::FSoftObjectPtr& Value) const noexcept -> size_t
	{
		return std::hash<Durin::FSoftObjectPath>{}(Value.GetSoftObjectPath());
	}
};

template<typename T>
struct std::hash<Durin::TSoftObjectPtr<T>>
{
	auto operator()(const Durin::TSoftObjectPtr<T>& Value) const noexcept -> size_t
	{
		return std::hash<Durin::FSoftObjectPath>{}(Value.GetSoftObjectPath());
	}
};
