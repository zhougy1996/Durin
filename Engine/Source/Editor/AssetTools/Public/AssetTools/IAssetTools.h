#pragma once

#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"

namespace Durin
{
	class DClass;
	class DFactory;
	class DPackage;

	struct FAssetToolsResult
	{
		DObject* Asset = nullptr;
		DPackage* Package = nullptr;
		std::string Message;

		auto Succeeded() const -> bool { return Asset != nullptr; }
		explicit operator bool() const { return Succeeded(); }
	};

	// Coordinates editor asset construction while factories own object-specific
	// initialization. Returned assets are live and unsaved; the package is kept
	// alive by Standalone residency rather than the permanent root set.
	class IAssetTools
	{
	public:
		virtual ~IAssetTools() = default;

		virtual auto CreateAsset(
			const FAssetPath& AssetPath,
			DClass* AssetClass,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public)
			-> FAssetToolsResult = 0;

		virtual auto ImportAsset(
			const FAssetPath& AssetPath,
			DClass* AssetClass,
			std::string_view Filename,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public)
			-> FAssetToolsResult = 0;

		// Discards a package created through this service. Unsaved state is
		// intentionally abandoned and the full object hierarchy is collected.
		virtual auto DiscardPackage(DPackage* Package) -> bool = 0;
	};

	ASSETTOOLS_API auto GetAssetTools() -> IAssetTools&;
}
