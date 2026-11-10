#pragma once

#include "CoreMinimal.h"
#include "Misc/Name.h"

namespace Durin
{
	struct FPrimaryAssetType
	{
		FName Name;
		FPrimaryAssetType() = default;
		explicit FPrimaryAssetType(FName InName) : Name(InName) {}
		auto IsValid() const -> bool { return !Name.IsNone(); }
		auto ToString() const -> std::string { return Name.ToString(); }
		auto operator==(const FPrimaryAssetType&) const -> bool = default;
	};

	struct FPrimaryAssetId
	{
		FPrimaryAssetType PrimaryAssetType;
		FName PrimaryAssetName;
		auto IsValid() const -> bool { return PrimaryAssetType.IsValid() && !PrimaryAssetName.IsNone(); }
		auto ToString() const -> std::string
		{ return IsValid() ? PrimaryAssetType.ToString() + ":" + PrimaryAssetName.ToString() : std::string{}; }
		static auto TryParse(std::string_view Text, FPrimaryAssetId& Out) -> bool
		{
			const auto Colon = Text.find(':');
			if (Colon == 0 || Colon == std::string_view::npos || Colon + 1 == Text.size()
				|| Text.find(':', Colon + 1) != std::string_view::npos) return false;
			FPrimaryAssetId Candidate{FPrimaryAssetType(FName(Text.substr(0, Colon))), FName(Text.substr(Colon + 1))};
			if (!Candidate.IsValid()) return false;
			Out = Candidate;
			return true;
		}
		auto operator==(const FPrimaryAssetId&) const -> bool = default;
	};
}

template<> struct std::hash<Durin::FPrimaryAssetType>
{
	auto operator()(const Durin::FPrimaryAssetType& Value) const -> size_t { return std::hash<Durin::FName>{}(Value.Name); }
};
template<> struct std::hash<Durin::FPrimaryAssetId>
{
	auto operator()(const Durin::FPrimaryAssetId& Value) const -> size_t
	{ return std::hash<Durin::FPrimaryAssetType>{}(Value.PrimaryAssetType) ^ (std::hash<Durin::FName>{}(Value.PrimaryAssetName) << 1); }
};
