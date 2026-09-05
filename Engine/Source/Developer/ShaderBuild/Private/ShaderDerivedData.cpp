#include "ShaderDerivedData.h"

namespace Durin::ShaderDerivedData
{
	namespace
	{
		template<typename TBuilder>
		auto UpdateString(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto IsValidRequest(const FShaderCompileOptions& Options) -> bool
		{
			if (Options.EntryPoints.empty()
				|| Options.EntryPoints.size() != Options.Frequencies.size()
				|| Options.EntryPoints.size() > MaximumEntryPoints) return false;
			std::set<std::pair<std::string_view, uint32>> Entries;
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				const std::string_view Entry = Options.EntryPoints[Index]
					? std::string_view(Options.EntryPoints[Index]) : std::string_view{};
				const uint32 Frequency =
					static_cast<uint32>(Options.Frequencies[Index]);
				if (Entry.empty()
					|| Frequency > static_cast<uint32>(EShaderFrequency::RayMiss)
					|| !Entries.emplace(Entry, Frequency).second) return false;
			}
			return true;
		}
	}

	auto BuildKey(
		const FShaderVariantKey& VariantKey,
		const FShaderCompileOptions& Options) -> DerivedData::FCacheKey
	{
		if (VariantKey.Value.IsZero() || !IsValidRequest(Options)) return {};
		FXxHash128Builder Builder;
		UpdateString(Builder, "DurinShaderCompiledOutputKey");
		Builder.UpdateValue(PayloadSchemaVersion);
		Builder.UpdateValue(BuilderVersion);
		Builder.UpdateValue(VariantKey.Value);
		Builder.UpdateValue(static_cast<uint32>(Options.EntryPoints.size()));
		for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
		{
			UpdateString(Builder, Options.EntryPoints[Index]
				? std::string_view(Options.EntryPoints[Index]) : std::string_view{});
			Builder.UpdateValue(static_cast<uint32>(Options.Frequencies[Index]));
		}
		return DerivedData::FCacheKey::FromHash(
			DerivedData::FCacheBucket::FromString("Shaders/CompiledOutput"),
			Builder.Finalize());
	}
}
