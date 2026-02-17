/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#include "sourcesconfig.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <json/reader.h>

namespace {

static std::string Trim(std::string value)
{
	while (!value.empty() &&
	       std::isspace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() &&
	       std::isspace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

static std::string NormalizeMapBaseUrl(std::string url)
{
	url = Trim(std::move(url));
	if (!url.empty() && url.back() != '/') {
		url.push_back('/');
	}
	return url;
}

static void AppendUnique(std::vector<std::string>& list, std::string value)
{
	value = Trim(std::move(value));
	if (value.empty()) {
		return;
	}
	if (std::find(list.begin(), list.end(), value) == list.end()) {
		list.push_back(std::move(value));
	}
}

static bool ParseUrlArray(const Json::Value& node, std::vector<std::string>& out,
			  bool normalizeMapBaseUrls, std::string& error)
{
	if (!node.isArray()) {
		error = "expected array of URLs";
		return false;
	}

	for (Json::Value::ArrayIndex i = 0; i < node.size(); ++i) {
		if (!node[i].isString()) {
			error = "array contains non-string URL value";
			return false;
		}
		std::string url = node[i].asString();
		if (normalizeMapBaseUrls) {
			url = NormalizeMapBaseUrl(std::move(url));
		}
		AppendUnique(out, std::move(url));
	}

	if (out.empty()) {
		error = "URL array is empty";
		return false;
	}
	return true;
}

static bool ParsePositiveLong(const Json::Value& node, const char* key,
			      long& out, std::string& error)
{
	if (!node.isMember(key)) {
		return true;
	}
	if (!node[key].isInt64() && !node[key].isInt()) {
		error = std::string(key) + " must be an integer";
		return false;
	}

	const long value = static_cast<long>(node[key].asInt64());
	if (value <= 0) {
		error = std::string(key) + " must be > 0";
		return false;
	}
	out = value;
	return true;
}

static std::string BuildSourcesConfigPath(const std::string& lobbyWriteDir)
{
	std::string dir = lobbyWriteDir;
	if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
		dir.push_back('/');
	}
	return dir + "sources.json";
}

static bool ReadFile(const std::string& path, std::string& out, std::string& error)
{
	FILE* fp = std::fopen(path.c_str(), "rb");
	if (fp == nullptr) {
		if (errno == ENOENT) {
			error.clear();
		} else {
			error = std::strerror(errno);
		}
		return false;
	}

	char buffer[4096];
	out.clear();
	while (!std::feof(fp)) {
		const size_t nread = std::fread(buffer, 1, sizeof(buffer), fp);
		if (nread > 0) {
			out.append(buffer, nread);
		}
		if (std::ferror(fp)) {
			error = "failed while reading file";
			std::fclose(fp);
			return false;
		}
	}
	std::fclose(fp);
	return true;
}

} // namespace

DownloaderSourcesConfig LoadDownloaderSourcesConfig(const std::string& lobbyWriteDir)
{
	DownloaderSourcesConfig config;
	config.path = BuildSourcesConfigPath(lobbyWriteDir);

	std::string json;
	std::string fileReadError;
	if (!ReadFile(config.path, json, fileReadError)) {
		if (fileReadError.empty()) {
			config.loadState = DownloaderSourcesLoadState::Missing;
			return config;
		}
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "failed to open file: " + fileReadError;
		return config;
	}

	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(json, root)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "invalid JSON: " + reader.getFormattedErrorMessages();
		return config;
	}
	if (!root.isObject()) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "root must be a JSON object";
		return config;
	}

	if (root.isMember("version")) {
		if (!root["version"].isInt() || root["version"].asInt() != 1) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "unsupported or invalid version (expected 1)";
			return config;
		}
	}

	const Json::Value rapid = root["rapid"];
	const Json::Value maps = root["maps"];
	if (!rapid.isObject() || !maps.isObject()) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "missing or invalid rapid/maps sections";
		return config;
	}

	std::string parseError;
	if (!ParseUrlArray(rapid["master_urls"], config.rapidMasterUrls, false, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "rapid.master_urls: " + parseError;
		return config;
	}
	if (!ParseUrlArray(maps["base_urls"], config.mapBaseUrls, true, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "maps.base_urls: " + parseError;
		return config;
	}

	if (rapid.isMember("git_enabled")) {
		if (!rapid["git_enabled"].isBool()) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "rapid.git_enabled must be a bool";
			return config;
		}
		config.rapidGitEnabled = rapid["git_enabled"].asBool();
	}
	if (rapid.isMember("git_manifest_url")) {
		if (!rapid["git_manifest_url"].isString()) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "rapid.git_manifest_url must be a string";
			return config;
		}
		config.rapidGitManifestUrl = Trim(rapid["git_manifest_url"].asString());
	}
	if (!ParsePositiveLong(rapid, "git_manifest_ttl_seconds",
			       config.rapidGitManifestTtlSeconds, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "rapid." + parseError;
		return config;
	}
	if (!ParsePositiveLong(rapid, "git_api_timeout_seconds",
			       config.rapidGitApiTimeoutSeconds, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "rapid." + parseError;
		return config;
	}

	config.loadState = DownloaderSourcesLoadState::LoadedFromFile;
	return config;
}
