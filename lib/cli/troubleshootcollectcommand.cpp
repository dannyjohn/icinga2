/*****************************************************************************
* Icinga 2                                                                   *
* Copyright (C) 2012-2014 Icinga Development Team (http://www.icinga.org)    *
*                                                                            *
* This program is free software; you can redistribute it and/or              *
* modify it under the terms of the GNU General Public License                *
* as published by the Free Software Foundation; either version 2             *
* of the License, or (at your option) any later version.                     *
*                                                                            *
* This program is distributed in the hope that it will be useful,            *
* but WITHOUT ANY WARRANTY; without even the implied warranty of             *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
* GNU General Public License for more details.                               *
*                                                                            *
* You should have received a copy of the GNU General Public License          *
* along with this program; if not, write to the Free Software Foundation     *
* Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
******************************************************************************/

#include "cli/troubleshootcollectcommand.hpp"
#include "cli/featureutility.hpp"
#include "cli/objectlistcommand.hpp"
#include "base/netstring.hpp"
#include "base/application.hpp"
#include "base/stdiostream.hpp"
#include "base/json.hpp"
#include "base/objectlock.hpp"
#include "base/exception.hpp"

#include <boost/filesystem.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/foreach.hpp>
#include <iomanip>
#include <iostream>
#include <time.h>
#include <fstream>

#ifdef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#endif /*_WIN32*/


using namespace icinga;
namespace po = boost::program_options;

REGISTER_CLICOMMAND("troubleshoot/collect", TroubleshootCollectCommand);

String TroubleshootCollectCommand::GetDescription(void) const
{
	return "Collect logs and other relevant information for troubleshooting purposes.";
}

String TroubleshootCollectCommand::GetShortDescription(void) const
{
	return "Collect information for troubleshooting";
}

static void getLatestReport(const String& filename, time_t& bestTimestamp, String& bestFilename)
{
#ifdef _WIN32
	struct _stat buf;
	if (_stat(filename.CStr(), &buf))
		return;
#else
	struct stat buf;
	if (stat(filename.CStr(), &buf))
	    return;
#endif /*_WIN32*/
	if (buf.st_mtime > bestTimestamp) {
		bestTimestamp = buf.st_mtime;
		bestFilename = filename;
	}
}

/*Print the latest crash report to *os* */
static void printCrashReports(std::ostream& os)
{
	String spath = Application::GetLocalStateDir() + "/log/icinga2/crash/report.*";
	time_t bestTimestamp = 0;
	String bestFilename;

	try {
		Utility::Glob(spath,
					  boost::bind(&getLatestReport, _1, boost::ref(bestTimestamp), boost::ref(bestFilename)), GlobFile);
	}
		
#ifdef _WIN32
	catch (win32_error &ex) {
		if (int const * err = boost::get_error_info<errinfo_win32_error>(ex)) {
			if (*err != 3) //Error code for path does not exist
				throw ex;
			os << Application::GetLocalStateDir() + "/log/icinga2/crash/ does not exist" << std::endl;
		} else {
			throw ex;
		}
	}
#else
	catch (...) {
		throw;
        }
#endif /*_WIN32*/

		
	if (!bestTimestamp)
		os << "\n\tNo crash logs found in " << Application::GetLocalStateDir().CStr() << "/log/icinga2/crash/\n";
	else {
		const std::tm tm = Utility::LocalTime(bestTimestamp);
                char *tmBuf = new char[200]; //Should always be enough
                const char *fmt = "%Y-%m-%d %H:%M:%S" ;
                if (!strftime(tmBuf, 199, fmt, &tm))
                    return;
		os << "\n\tLatest crash report is from " << tmBuf
			<< "\n\tFile: " << bestFilename << std::endl;
		TroubleshootCollectCommand::tail(bestFilename, 20, os);
	}
}

/*Print the last *numLines* of *file* to *os* */
int TroubleshootCollectCommand::tail(const String& file, int numLines, std::ostream& os)
{
	boost::circular_buffer<std::string> ringBuf(numLines);
	std::ifstream text;
	text.open(file.CStr(), std::ifstream::in);
	if (!text.good())
		return 0;

	std::string line;
	int lines = 0;

	while (std::getline(text, line)) {
		ringBuf.push_back(line);
		lines++;
	}

	if (lines < numLines)
		numLines = lines;

	for (int k = 0; k < numLines; k++)
		os << ringBuf[k] << std::endl;

	os << std::endl;
	text.close();
	return numLines;
}

/*Find all FileLoggers and print out their last 10 lines*/
static void printLogTail(const String& objectfile, std::ostream& os)
{
	std::fstream fp;
	fp.open(objectfile.CStr(), std::ios_base::in);

	StdioStream::Ptr sfp = new StdioStream(&fp, false);
	String message;

	while (NetString::ReadStringFromStream(sfp, &message)) {
		Dictionary::Ptr object = JsonDecode(message);
		Dictionary::Ptr properties = object->Get("properties");

		String name = object->Get("name");
		String type = object->Get("type");

		if (Utility::Match(type, "FileLogger")) {
			Dictionary::Ptr debug_hints = object->Get("debug_hints");
			Dictionary::Ptr properties = object->Get("properties");
			Dictionary::Ptr debug_hint_props;

			if (debug_hints)
				debug_hint_props = debug_hints->Get("properties");

			ObjectLock olock(properties);
			BOOST_FOREACH(const Dictionary::Pair& kv, properties)
			{
				String key = kv.first;
				Value val = kv.second;
				if (Utility::Match(key, "path")) {
					os << "\n\tFound Log " << object->Get("name") << " at path: " << val << std::endl;
					String valS = val;
					if (!TroubleshootCollectCommand::tail(val, 10, os))
						os << val << " either does not exist or is empty" << std::endl;
				}
			}
		}
	}

	printCrashReports(os);
}

void TroubleshootCollectCommand::InitParameters(boost::program_options::options_description& visibleDesc,
									 boost::program_options::options_description& hiddenDesc) const
{
	visibleDesc.add_options()
		("console,c", "print to console instead of file")
		;
}

int TroubleshootCollectCommand::Run(const boost::program_options::variables_map& vm, const std::vector<std::string>& ap) const
{
	std::ofstream os;
	if (vm.count("console")) {
		os.copyfmt(std::cout);
		os.clear(std::cout.rdstate());
		os.basic_ios<char>::rdbuf(std::cout.rdbuf());
	} else {
		os.open((Application::GetLocalStateDir() + "/log/icinga2/troubleshooting.log").CStr(),
				std::ios::out | std::ios::trunc);
		if (!os.is_open()) {
			std::cout << "Could not open " << (Application::GetLocalStateDir() + "/log/icinga2/troubleshooting.log") << " to write"
				<< std::endl;
			return 3;
		}
	}

	String appName = Utility::BaseName(Application::GetArgV()[0]);

	os << appName << " -- Troubleshooting help:" << std::endl
		<< "Should you run in problems with icinga please add this file to your help request\n";

	if (appName.GetLength() > 3 && appName.SubStr(0, 3) == "lt-")
		appName = appName.SubStr(3, appName.GetLength() - 3);

	os << std::endl;
	Application::DisplayInfoMessage(os);
	os << std::endl;
	FeatureUtility::ListFeatures(os);
	os << std::endl;

	String objectfile = Application::GetObjectsPath();

	if (!Utility::PathExists(objectfile)) {
		os << "Cannot open objects file '" << Application::GetObjectsPath() << "'."
			<< "Run 'icinga2 daemon -C' to validate config and generate the cache file.\n";
	} else
		printLogTail(objectfile, os);

	std::cout << "Finished collection";
	if (!vm.count("console")) {
		os.close();
		std::cout << ", printed to \"" << Application::GetLocalStateDir() + "/log/icinga2/troubleshooting.log\"";
	}
	std::cout << std::endl;

	return 0;
}

