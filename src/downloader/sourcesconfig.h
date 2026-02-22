/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#ifndef SPRINGLOBBY_SOURCESCONFIG_H
#define SPRINGLOBBY_SOURCESCONFIG_H

#include <string>
#include <vector>

enum class DownloaderSourcesLoadState {
	Missing,
	LoadedFromFile,
	InvalidFile,
};

struct DownloaderEngineProvider
{
	std::string type;
	std::string url;
	std::string name;
};

struct DownloaderSourcesConfig
{
	DownloaderSourcesLoadState loadState = DownloaderSourcesLoadState::Missing;
	std::vector<std::string> rapidMasterUrls;
	std::vector<std::string> mapBaseUrls;
	std::vector<DownloaderEngineProvider> engineProviders;
	long rapidRepoTimeoutSeconds = 20;
	long mapDownloadTimeoutSeconds = 20;
	long engineDownloadTimeoutSeconds = 20;
	std::string error;
	std::string path;
};

DownloaderSourcesConfig LoadDownloaderSourcesConfig(const std::string& lobbyWriteDir);

#endif
