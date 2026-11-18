#include "Toolkits/AssetEditorToolkit.h"

#include "DObject/ObjectGraphReplacement.h"

namespace Durin
{
	class FToolkitAssetReloadParticipant final : public IObjectReplacementParticipant
	{
	public:
		explicit FToolkitAssetReloadParticipant(std::shared_ptr<FAssetEditorToolkit> InOwner) : Owner(std::move(InOwner)) {}
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Bindings.clear();
			for (size_t Index = 0; Index < Owner->EditingAssets.size(); ++Index)
				if (const auto* Entry = Map.Find(Owner->EditingAssets[Index].Get()))
				{
					if (!Entry->Replacement)
						return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference});
					Bindings.push_back({Index, FObjectKey(Entry->Previous), FStrongObjectPtr(Entry->Replacement)});
				}
			return {};
		}
		auto Validate() const -> bool override
		{
			return Owner->IsEditorOpen() && std::ranges::all_of(Bindings, [&](const auto& Binding) {
				return Binding.Index < Owner->EditingAssets.size()
					&& Owner->EditingAssets[Binding.Index].GetKey() == Binding.Previous;
			});
		}
		auto CoversNativeReferences(const DObject&) const -> bool override { return false; }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
		{ return static_cast<uint32>(std::ranges::count(Bindings, FObjectKey(&Object), &FBinding::Previous)); }
		auto Commit() noexcept -> void override
		{
			for (auto& Binding : Bindings)
			{
				if (Owner->ActiveAsset == Binding.Previous)
				{
					Owner->ActiveAsset = Binding.Replacement.GetKey();
					Owner->Tabs.SelectAsset(Owner->ActiveAsset);
				}
				Owner->EditingAssets[Binding.Index] = std::move(Binding.Replacement);
			}
		}
		auto Abort() noexcept -> void override { Bindings.clear(); }
		auto CanRetire() const -> bool override { return true; }
	private:
		struct FBinding
		{
			size_t Index;
			FObjectKey Previous;
			FStrongObjectPtr Replacement;
		};
		std::shared_ptr<FAssetEditorToolkit> Owner;
		std::vector<FBinding> Bindings;
	};

	auto FAssetEditorToolkit::CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant>
	{
		const auto Owner = weak_from_this().lock();
		return Owner ? std::make_shared<FToolkitAssetReloadParticipant>(Owner) : nullptr;
	}
}
