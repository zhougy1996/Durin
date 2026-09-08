#pragma once
#include "Texture/Texture.h"
#include <unordered_map>

namespace Durin::Testing
{
	// Scoped observations of admission events; getters cannot manufacture progress.
	class FTextureUpdateRequestRecorder
	{
	public:
		FTextureUpdateRequestRecorder()
		{
			Handle = OnTextureResourceChanged().AddRaw(this, &FTextureUpdateRequestRecorder::OnChange);
		}
		~FTextureUpdateRequestRecorder() { OnTextureResourceChanged().Remove(Handle); }
		auto Count(const DTexture& Texture) const -> uint64
		{
			const auto It = Requests.find(&Texture);
			return It != Requests.end() ? It->second : 0;
		}
	private:
		auto OnChange(DTexture& Texture, ETextureResourceChange Change) -> void
		{
			if (Change == ETextureResourceChange::Input) ++Requests[&Texture];
			else if (Change == ETextureResourceChange::Closed) Requests.erase(&Texture);
		}
		FDelegateHandle Handle;
		std::unordered_map<const DTexture*, uint64> Requests;
	};
}
