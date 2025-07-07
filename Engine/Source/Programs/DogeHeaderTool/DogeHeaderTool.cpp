#include "DogeHeaderTool.h"

#include "DHTCodeGenerator.h"
#include "DHTParser.h"
#include "DHTMetaData.h"

namespace DHT
{
DogeHeaderTool::DogeHeaderTool()
{
	Parser_ = new DHTParser();
	Generator_ = new DHTCodeGenerator();
}

DogeHeaderTool::~DogeHeaderTool()
{
	delete Parser_;
	delete Generator_;
}

bool DogeHeaderTool::Process(const std::string& Filename)
{
	std::optional<DHTFile> MetaFile = Parser_->ParseHeaderFile(Filename);
	if (!MetaFile.has_value())
	{
		std::cerr << "Parse failed: " << Filename << std::endl;
		return false;
	}

	Generator_->GenerateDHTHeaderFile(MetaFile.value());
	return true;
}
} // namespace DHT

