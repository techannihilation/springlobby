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

struct DownloaderSourcesConfig
{
	DownloaderSourcesLoadState loadState = DownloaderSourcesLoadState::Missing;
	std::vector<std::string> rapidMasterUrls;
	std::vector<std::string> mapBaseUrls;
	bool rapidGitEnabled = true;
	std::string rapidGitManifestUrl;
	long rapidGitManifestTtlSeconds = 300;
	long rapidGitApiTimeoutSeconds = 20;
	std::string error;
	std::string path;
};

DownloaderSourcesConfig LoadDownloaderSourcesConfig(const std::string& lobbyWriteDir);

#endif
