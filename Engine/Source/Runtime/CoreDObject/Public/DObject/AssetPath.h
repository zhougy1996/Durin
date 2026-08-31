#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	inline constexpr size_t MaximumObjectPathComponentBytes = 1024;
	inline constexpr size_t MaximumObjectPathBytes = 1024 * 1024;

	// Stores one mounted package identity without an object suffix.
	class FPackagePath
	{
	public:
		FPackagePath() = default;

		COREDOBJECT_API static auto TryCreate(
			std::string_view InPath, FPackagePath& OutPath,
			std::string* OutError = nullptr) -> bool;
		// Cook output is staged before its fixed /Game mount exists. This factory
		// admits only that deferred project-content namespace.
		COREDOBJECT_API static auto TryCreateProjectContent(
			std::string_view InPath, FPackagePath& OutPath,
			std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto IsValid(
			std::string_view InPath, std::string* OutError = nullptr) -> bool;

		auto IsValid() const -> bool { return !Path.empty(); }
		auto ToString() const -> const std::string& { return Path; }
		auto GetView() const -> std::string_view { return Path; }
		auto GetPackageName() const -> std::string_view
		{
			const size_t Slash = Path.find_last_of('/');
			return Slash == std::string::npos
				? std::string_view(Path)
				: std::string_view(Path).substr(Slash + 1);
		}
		// Temporary spelling for package-path callers awaiting migration.
		auto GetAssetName() const -> std::string_view { return GetPackageName(); }

		auto operator<=>(const FPackagePath&) const = default;

	private:
		explicit FPackagePath(std::string InPath)
			: Path(std::move(InPath))
		{
		}

		std::string Path;
	};

	// Stores one independently addressable package-outer export identity.
	class FTopLevelAssetPath
	{
	public:
		FTopLevelAssetPath() = default;

		COREDOBJECT_API static auto TryCreate(
			std::string_view InPath, FTopLevelAssetPath& OutPath,
			std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreate(
			const FPackagePath& InPackagePath, std::string_view InAssetName,
			FTopLevelAssetPath& OutPath, std::string* OutError = nullptr) -> bool;

		auto IsValid() const -> bool { return PackagePath.IsValid() && !AssetName.empty(); }
		auto GetPackagePath() const -> const FPackagePath& { return PackagePath; }
		auto GetAssetName() const -> std::string_view { return AssetName; }
		auto ToString() const -> const std::string& { return CanonicalPath; }
		auto GetView() const -> std::string_view { return CanonicalPath; }

		auto operator<=>(const FTopLevelAssetPath&) const = default;

	private:
		FPackagePath PackagePath;
		std::string AssetName;
		std::string CanonicalPath;
	};

	// Stores an exact top-level asset or one of its ordered subobjects.
	class FObjectPath
	{
	public:
		FObjectPath() = default;

		COREDOBJECT_API static auto TryCreate(
			std::string_view InPath, FObjectPath& OutPath,
			std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreate(
			const FTopLevelAssetPath& InAssetPath,
			std::span<const std::string> InSubobjectNames,
			FObjectPath& OutPath, std::string* OutError = nullptr) -> bool;

		auto IsValid() const -> bool { return AssetPath.IsValid(); }
		auto GetAssetPath() const -> const FTopLevelAssetPath& { return AssetPath; }
		auto GetPackagePath() const -> const FPackagePath& { return AssetPath.GetPackagePath(); }
		auto GetSubobjectNames() const -> std::span<const std::string> { return SubobjectNames; }
		auto IsTopLevelAsset() const -> bool { return IsValid() && SubobjectNames.empty(); }
		auto ToString() const -> const std::string& { return CanonicalPath; }
		auto GetView() const -> std::string_view { return CanonicalPath; }

		auto operator<=>(const FObjectPath&) const = default;

	private:
		FTopLevelAssetPath AssetPath;
		std::vector<std::string> SubobjectNames;
		std::string CanonicalPath;
	};

	// Temporary source adapter retained only while package-path consumers migrate.
	using FAssetPath = FPackagePath;
}

template<>
struct std::hash<Durin::FPackagePath>
{
	auto operator()(const Durin::FPackagePath& Value) const noexcept -> size_t
	{
		return std::hash<std::string_view>{}(Value.GetView());
	}
};

template<>
struct std::hash<Durin::FTopLevelAssetPath>
{
	auto operator()(const Durin::FTopLevelAssetPath& Value) const noexcept -> size_t
	{
		return std::hash<std::string_view>{}(Value.GetView());
	}
};

template<>
struct std::hash<Durin::FObjectPath>
{
	auto operator()(const Durin::FObjectPath& Value) const noexcept -> size_t
	{
		return std::hash<std::string_view>{}(Value.GetView());
	}
};
