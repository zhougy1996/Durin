#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class FAssetPath
	{
	public:
		FAssetPath() = default;

		COREDOBJECT_API static auto TryCreate(std::string_view InPath, FAssetPath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto IsValid(std::string_view InPath, std::string* OutError = nullptr) -> bool;

		auto IsValid() const -> bool { return !Path.empty(); }
		auto ToString() const -> const std::string& { return Path; }
		auto GetView() const -> std::string_view { return Path; }
		auto GetAssetName() const -> std::string_view
		{
			const size_t Slash = Path.find_last_of('/');
			return Slash == std::string::npos ? std::string_view(Path) : std::string_view(Path).substr(Slash + 1);
		}

		auto operator==(const FAssetPath&) const -> bool = default;

	private:
		explicit FAssetPath(std::string InPath)
			: Path(std::move(InPath))
		{
		}

		std::string Path;
	};
}

template<>
struct std::hash<Durin::FAssetPath>
{
	auto operator()(const Durin::FAssetPath& Value) const noexcept -> size_t
	{
		return std::hash<std::string_view>{}(Value.GetView());
	}
};
