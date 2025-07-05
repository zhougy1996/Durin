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
	DHTFile MetaFile = Parser_->ParseHeaderFile(Filename);
	Generator_->GenerateDHTHeaderFile(MetaFile);
	return true;
}
} // namespace DHT

