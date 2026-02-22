/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#include "sourcesconfig.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
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

static std::string ToLowerAscii(std::string value)
{
	for (char& c : value) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
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

static bool ParseEngineProviders(const Json::Value& node,
				 std::vector<DownloaderEngineProvider>& out,
				 std::string& error)
{
	if (!node.isArray()) {
		error = "expected array of providers";
		return false;
	}

	std::set<std::string> seen;
	for (Json::Value::ArrayIndex i = 0; i < node.size(); ++i) {
		const Json::Value provider = node[i];
		if (!provider.isObject()) {
			error = "provider entry must be an object";
			return false;
		}

		if (!provider["type"].isString() || !provider["url"].isString()) {
			error = "provider entry requires string fields type and url";
			return false;
		}

		DownloaderEngineProvider parsed;
		parsed.type = ToLowerAscii(Trim(provider["type"].asString()));
		parsed.url = Trim(provider["url"].asString());
		parsed.name = provider["name"].isString()
				  ? Trim(provider["name"].asString())
				  : "";

		if (parsed.type != "github_releases" && parsed.type != "springfiles") {
			error = "unsupported provider type: " + parsed.type;
			return false;
		}
		if (parsed.url.empty()) {
			error = "provider url cannot be empty";
			return false;
		}

		const std::string dedupeKey = parsed.type + "\n" + parsed.url;
		if (seen.insert(dedupeKey).second) {
			out.push_back(std::move(parsed));
		}
	}

	if (out.empty()) {
		error = "providers array is empty";
		return false;
	}
	return true;
}

static bool ParseNonNegativeLong(const Json::Value& node, const char* key,
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
	if (value < 0) {
		error = std::string(key) + " must be >= 0";
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
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errs;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader || !reader->parse(json.data(), json.data() + json.size(), &root, &errs)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "invalid JSON: " + errs;
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
	if (!ParseNonNegativeLong(rapid, "repo_timeout_seconds",
				  config.rapidRepoTimeoutSeconds, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "rapid." + parseError;
		return config;
	}
	if (!ParseNonNegativeLong(maps, "download_timeout_seconds",
				  config.mapDownloadTimeoutSeconds, parseError)) {
		config.loadState = DownloaderSourcesLoadState::InvalidFile;
		config.error = "maps." + parseError;
		return config;
	}

	if (root.isMember("engine")) {
		const Json::Value engine = root["engine"];
		if (!engine.isObject()) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "engine section must be a JSON object";
			return config;
		}

		if (!ParseEngineProviders(engine["providers"], config.engineProviders, parseError)) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "engine.providers: " + parseError;
			return config;
		}
		if (!ParseNonNegativeLong(engine, "download_timeout_seconds",
					  config.engineDownloadTimeoutSeconds, parseError)) {
			config.loadState = DownloaderSourcesLoadState::InvalidFile;
			config.error = "engine." + parseError;
			return config;
		}
	}

	config.loadState = DownloaderSourcesLoadState::LoadedFromFile;
	return config;
}
