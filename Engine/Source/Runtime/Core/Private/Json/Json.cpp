#include "Json/Json.h"

#include "Misc/FileHelper.h"

#include "yyjson.h"

namespace Durin
{
	namespace
	{
		static auto MakeJsonValue(void* ValuePtr) -> yyjson_val*
		{
			return static_cast<yyjson_val*>(ValuePtr);
		}

		static auto ToJsonValueView(yyjson_val* Value) -> FJsonValueView
		{
			return FJsonValueView::FromOpaque(static_cast<void*>(Value));
		}

		static auto PopulateParseError(FJsonParseError* OutError, const yyjson_read_err& ReadError, std::string_view JsonText) -> void
		{
			if (!OutError)
			{
				return;
			}

			OutError->Code = static_cast<int32>(ReadError.code);
			OutError->BytePosition = ReadError.pos;
			OutError->Message = ReadError.msg ? ReadError.msg : "";

			size_t Line = 0;
			size_t Column = 0;
			size_t Character = 0;
			if (yyjson_locate_pos(JsonText.data(), JsonText.size(), ReadError.pos, &Line, &Column, &Character))
			{
				OutError->Line = Line;
				OutError->Column = Column;
				OutError->Character = Character;
			}
		}
	} // namespace

	struct FJsonDocument::FImpl
	{
		yyjson_doc* Document = nullptr;

		~FImpl()
		{
			Reset();
		}

		auto Reset() -> void
		{
			if (Document)
			{
				yyjson_doc_free(Document);
				Document = nullptr;
			}
		}
	};

	auto FJsonValueView::IsValid() const -> bool
	{
		return ValuePtr != nullptr;
	}

	auto FJsonValueView::IsNull() const -> bool
	{
		return yyjson_is_null(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsObject() const -> bool
	{
		return yyjson_is_obj(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsArray() const -> bool
	{
		return yyjson_is_arr(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsString() const -> bool
	{
		return yyjson_is_str(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsBool() const -> bool
	{
		return yyjson_is_bool(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsInt() const -> bool
	{
		return yyjson_is_int(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsUInt() const -> bool
	{
		return yyjson_is_uint(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::IsNumber() const -> bool
	{
		return yyjson_is_num(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::Num() const -> size_t
	{
		return yyjson_get_len(MakeJsonValue(ValuePtr));
	}

	auto FJsonValueView::GetView(std::string_view InKey) const -> FJsonValueView
	{
		return ToJsonValueView(yyjson_obj_getn(MakeJsonValue(ValuePtr), InKey.data(), InKey.size()));
	}

	auto FJsonValueView::GetView(size_t Index) const -> FJsonValueView
	{
		return ToJsonValueView(yyjson_arr_get(MakeJsonValue(ValuePtr), Index));
	}

	auto FJsonValueView::GetString(std::string DefaultValue) const -> std::string
	{
		if (const char* Value = yyjson_get_str(MakeJsonValue(ValuePtr)))
		{
			return Value;
		}
		return DefaultValue;
	}

	auto FJsonValueView::GetBool(bool DefaultValue) const -> bool
	{
		return IsBool() ? yyjson_get_bool(MakeJsonValue(ValuePtr)) : DefaultValue;
	}

	auto FJsonValueView::GetInt(int64 DefaultValue) const -> int64
	{
		return IsInt() ? yyjson_get_sint(MakeJsonValue(ValuePtr)) : DefaultValue;
	}

	auto FJsonValueView::GetUInt(uint64 DefaultValue) const -> uint64
	{
		return IsUInt() ? yyjson_get_uint(MakeJsonValue(ValuePtr)) : DefaultValue;
	}

	auto FJsonValueView::GetDouble(double DefaultValue) const -> double
	{
		return IsNumber() ? yyjson_get_num(MakeJsonValue(ValuePtr)) : DefaultValue;
	}

	auto FJsonValueView::GetStringValue(std::string_view InKey, std::string DefaultValue) const -> std::string
	{
		return GetView(InKey).GetString(std::move(DefaultValue));
	}

	auto FJsonValueView::GetBoolValue(std::string_view InKey, bool DefaultValue) const -> bool
	{
		return GetView(InKey).GetBool(DefaultValue);
	}

	auto FJsonValueView::GetIntValue(std::string_view InKey, int64 DefaultValue) const -> int64
	{
		return GetView(InKey).GetInt(DefaultValue);
	}

	auto FJsonValueView::GetUIntValue(std::string_view InKey, uint64 DefaultValue) const -> uint64
	{
		return GetView(InKey).GetUInt(DefaultValue);
	}

	auto FJsonValueView::GetDoubleValue(std::string_view InKey, double DefaultValue) const -> double
	{
		return GetView(InKey).GetDouble(DefaultValue);
	}

	FJsonDocument::FJsonDocument()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FJsonDocument::~FJsonDocument() = default;

	FJsonDocument::FJsonDocument(FJsonDocument&& Other) noexcept = default;

	auto FJsonDocument::operator=(FJsonDocument&& Other) noexcept -> FJsonDocument& = default;

	auto FJsonDocument::Parse(std::string_view JsonText, FJsonParseError* OutError) -> bool
	{
		Reset();

		if (OutError)
		{
			*OutError = {};
		}

		yyjson_read_err ReadError{};
		Impl->Document = yyjson_read_opts(const_cast<char*>(JsonText.data()), JsonText.size(), YYJSON_READ_NOFLAG, nullptr, &ReadError);
		if (!Impl->Document)
		{
			PopulateParseError(OutError, ReadError, JsonText);
			return false;
		}

		return true;
	}

	auto FJsonDocument::LoadFromFile(std::string_view FileName, FJsonParseError* OutError) -> bool
	{
		std::string JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, FileName))
		{
			if (OutError)
			{
				*OutError = {};
				OutError->Message = std::format("Failed to load JSON file: {}", FileName);
			}
			return false;
		}

		return Parse(JsonText, OutError);
	}

	auto FJsonDocument::Reset() -> void
	{
		Impl->Reset();
	}

	auto FJsonDocument::IsValid() const -> bool
	{
		return Impl->Document != nullptr;
	}

	auto FJsonDocument::GetRootView() const -> FJsonValueView
	{
		return ToJsonValueView(yyjson_doc_get_root(Impl->Document));
	}

}
