/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#include <iostream>
#include <string>
#include <cstdlib>
#include <clocale>
#include <locale>
#include <cstdio>

#include "lslunitsync/c_api.h"
#include "lslunitsync/springbundle.h"

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::cerr << "Usage: " << (argc > 0 ? argv[0] : "unitsync_smoke") << " /path/to/libunitsync.so\n";
		return 2;
	}

	std::setbuf(stdout, nullptr);
	std::setbuf(stderr, nullptr);

	const std::string unitsyncPath = argv[1];

	if (const char* useEnvLocale = std::getenv("UNITSYNC_SMOKE_USE_ENV_LOCALE"); useEnvLocale && std::string(useEnvLocale) == "1") {
		std::setlocale(LC_ALL, "");
		std::locale::global(std::locale(""));
	}

	LSL::UnitsyncLib usync;

	// Reproduce SpringLobby startup sequence:
	// 1) SpringBundle probes libunitsync to read GetSpringVersion (no Init()).
	// 2) UnitsyncLib loads the same library and calls Init().
	{
		LSL::SpringBundle bundle;
		bundle.unitsync = unitsyncPath;
		bundle.GetBundleVersion();
	}

	if (!usync.Load(unitsyncPath)) {
		std::cerr << "Failed to load unitsync: " << unitsyncPath << "\n";
		return 1;
	}

	try {
		std::cout << "SpringVersion: " << usync.GetSpringVersion() << "\n";
		std::cout << "SpringDataDir: " << usync.GetSpringDataDir() << "\n";
		std::cout << "MapCount: " << usync.GetMapCount() << "\n";
		std::cout << "PrimaryModCount: " << usync.GetPrimaryModCount() << "\n";
	} catch (const std::exception& e) {
		std::cerr << "Exception: " << e.what() << "\n";
		return 1;
	}

	usync.Unload();
	return 0;
}
