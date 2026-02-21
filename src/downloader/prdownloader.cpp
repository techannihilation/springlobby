/* This file is part of the Springlobby (GPL v2 or later), see COPYING */
#include "prdownloader.h"

#include <lslutils/globalsmanager.h>

#include "lib/src/Downloader/Http/HttpDownloader.h" //FIXME
#include "lib/src/Downloader/IDownloader.h"	 //FIXME: remove this include
#include "lib/src/FileSystem/FileSystem.h"	  //FIXME
#include "lib/src/pr-downloader.h"
#include "sourcesconfig.h"
// Resolves names collision: CreateDialog from WxWidgets and CreateDialog macro from WINUSER.H
// Remove with HttpDownloader.h header inclusion
#ifdef CreateDialog
#undef CreateDialog
#endif

#include <lslunitsync/unitsync.h>
#include <lslutils/thread.h>
#include <sys/time.h>
#include <json/writer.h>
#include <wx/app.h>
#include <wx/log.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "settings.h"
#include "gui/mainwindow.h"
#include "gui/ui.h"
#include "utils/conversion.h"
#include "utils/globalevents.h"
#include "utils/slconfig.h"
#include "utils/slpaths.h"

SLCONFIG("/Spring/PortableDownload", false, "true to download portable versions of spring, if false cache/settings/etc are shared (bogous!)");
SLCONFIG("/Spring/RapidMasterUrl", "https://rapid.techa-rts.com/repos.gz", "primary master url for rapid downloads");
SLCONFIG("/Spring/RapidMasterFallbackUrl", "https://repos.springrts.com/repos.gz", "fallback master url for rapid downloads");
SLCONFIG("/Spring/MapDownloadBaseUrl", "http://www.hakora.xyz/files/springrts/maps/", "primary base URL for map downloads (.sd7/.sdz)");
SLCONFIG("/Spring/RapidRepoTimeoutSeconds", 0l, "timeout in seconds for rapid repo index/package downloads (0 disables timeout override)");
SLCONFIG("/Spring/MapDownloadTimeoutSeconds", 0l, "timeout in seconds for map index/file downloads (0 disables timeout override)");
SLCONFIG("/Spring/EngineDownloadTimeoutSeconds", 0l, "timeout in seconds for engine metadata/file downloads (0 disables timeout override)");

static PrDownloader::DownloadProgress* m_progress = nullptr;
static std::mutex dlProgressMutex;

static PrDownloader::DownloadProgress* EnsureProgressLocked()
{
	if (m_progress == nullptr) {
		m_progress = new PrDownloader::DownloadProgress();
	}
	return m_progress;
}

static void StartDownloadProgressTracking(const std::string& name)
{
	std::lock_guard<std::mutex> lock(dlProgressMutex);
	PrDownloader::DownloadProgress* progress = EnsureProgressLocked();
	progress->name = name;
	progress->downloaded = 0;
	progress->filesize = 1;
	progress->running = true;
	progress->failed = false;
}

static void FinishDownloadProgressTracking(const std::string& name, bool success)
{
	std::lock_guard<std::mutex> lock(dlProgressMutex);
	PrDownloader::DownloadProgress* progress = EnsureProgressLocked();
	progress->name = name;
	if (progress->filesize <= 0) {
		progress->filesize = 1;
	}
	if (progress->downloaded < 0) {
		progress->downloaded = 0;
	}
	if (success && progress->downloaded < progress->filesize) {
		progress->downloaded = progress->filesize;
	}
	progress->running = false;
	progress->failed = !success;
}

static std::string GetRapidMasterUrl()
{
	return STD_STRING(cfg().ReadString("/Spring/RapidMasterUrl"));
}

static std::string GetRapidMasterFallbackUrl()
{
	return STD_STRING(cfg().ReadString("/Spring/RapidMasterFallbackUrl"));
}

static std::string GetMapDownloadBaseUrl()
{
	return STD_STRING(cfg().ReadString("/Spring/MapDownloadBaseUrl"));
}

static long GetRapidRepoTimeoutSeconds()
{
	return cfg().ReadLong("/Spring/RapidRepoTimeoutSeconds");
}

static long GetMapDownloadTimeoutSeconds()
{
	return cfg().ReadLong("/Spring/MapDownloadTimeoutSeconds");
}

static long GetEngineDownloadTimeoutSeconds()
{
	return cfg().ReadLong("/Spring/EngineDownloadTimeoutSeconds");
}

struct EffectiveSourcesConfig
{
	std::vector<std::string> rapidMasterUrls;
	std::vector<std::string> mapBaseUrls;
	std::vector<DownloaderEngineProvider> engineProviders;
	long rapidRepoTimeoutSeconds = 0;
	long mapDownloadTimeoutSeconds = 0;
	long engineDownloadTimeoutSeconds = 0;
	bool loadedFromSourcesFile = false;
	bool createdSourcesFile = false;
	bool usingSafeDefaultsBecauseFileInvalid = false;
	std::string sourcesFilePath;
	std::string sourcesFileError;
};

static constexpr const char* kDefaultRapidMasterPrimary = "https://rapid.techa-rts.com/repos.gz";
static constexpr const char* kDefaultRapidMasterSecondary = "https://repos.springrts.com/repos.gz";
static constexpr const char* kDefaultMapBase = "http://www.hakora.xyz/files/springrts/maps/";
static constexpr const char* kDefaultEngineGithubReleasesUrl = "https://api.github.com/repos/beyond-all-reason/RecoilEngine/releases?per_page=100";
static constexpr const char* kDefaultEngineSpringFilesUrl = "https://springfiles.springrts.com/json.php";

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

static void AppendUniqueUrl(std::vector<std::string>& out, std::string url)
{
	url = Trim(std::move(url));
	if (url.empty()) {
		return;
	}
	if (std::find(out.begin(), out.end(), url) == out.end()) {
		out.push_back(std::move(url));
	}
}

static std::string JoinWithNewlines(const std::vector<std::string>& list)
{
	std::string joined;
	for (size_t i = 0; i < list.size(); ++i) {
		if (i > 0) {
			joined.push_back('\n');
		}
		joined += list[i];
	}
	return joined;
}

static std::vector<DownloaderEngineProvider> MakeDefaultEngineProviders()
{
	return {
	    {"github_releases", kDefaultEngineGithubReleasesUrl, "BAR GitHub"},
	    {"springfiles", kDefaultEngineSpringFilesUrl, "SpringFiles"},
	};
}

static std::string BuildEngineProvidersJson(const std::vector<DownloaderEngineProvider>& providers)
{
	Json::Value root(Json::arrayValue);
	for (const DownloaderEngineProvider& provider : providers) {
		Json::Value item(Json::objectValue);
		item["type"] = provider.type;
		item["url"] = provider.url;
		if (!provider.name.empty()) {
			item["name"] = provider.name;
		}
		root.append(item);
	}

	Json::StreamWriterBuilder writerBuilder;
	writerBuilder["indentation"] = "";
	return Json::writeString(writerBuilder, root);
}

static EffectiveSourcesConfig MakeSafeDefaultsSourcesConfig()
{
	EffectiveSourcesConfig config;
	config.rapidMasterUrls = {kDefaultRapidMasterPrimary, kDefaultRapidMasterSecondary};
	config.mapBaseUrls = {kDefaultMapBase};
	config.engineProviders = MakeDefaultEngineProviders();
	config.rapidRepoTimeoutSeconds = 0;
	config.mapDownloadTimeoutSeconds = 0;
	config.engineDownloadTimeoutSeconds = 0;
	return config;
}

static EffectiveSourcesConfig MakeLegacySourcesConfig()
{
	EffectiveSourcesConfig legacyConfig;
	AppendUniqueUrl(legacyConfig.rapidMasterUrls, GetRapidMasterUrl());
	AppendUniqueUrl(legacyConfig.rapidMasterUrls, GetRapidMasterFallbackUrl());
	AppendUniqueUrl(legacyConfig.mapBaseUrls, NormalizeMapBaseUrl(GetMapDownloadBaseUrl()));
	legacyConfig.engineProviders = MakeDefaultEngineProviders();
	legacyConfig.rapidRepoTimeoutSeconds = GetRapidRepoTimeoutSeconds();
	legacyConfig.mapDownloadTimeoutSeconds = GetMapDownloadTimeoutSeconds();
	legacyConfig.engineDownloadTimeoutSeconds = GetEngineDownloadTimeoutSeconds();

	if (legacyConfig.rapidMasterUrls.empty()) {
		legacyConfig.rapidMasterUrls.push_back(kDefaultRapidMasterPrimary);
	}
	if (legacyConfig.mapBaseUrls.empty()) {
		legacyConfig.mapBaseUrls.push_back(kDefaultMapBase);
	}

	return legacyConfig;
}

static std::string BuildSourcesConfigJson(const EffectiveSourcesConfig& config)
{
	Json::Value root(Json::objectValue);
	root["version"] = 1;

	Json::Value rapid(Json::objectValue);
	Json::Value rapidMasterUrls(Json::arrayValue);
	for (const std::string& url : config.rapidMasterUrls) {
		rapidMasterUrls.append(url);
	}
	rapid["master_urls"] = rapidMasterUrls;
	rapid["repo_timeout_seconds"] = static_cast<Json::Int64>(config.rapidRepoTimeoutSeconds);

	Json::Value maps(Json::objectValue);
	Json::Value mapBaseUrls(Json::arrayValue);
	for (const std::string& url : config.mapBaseUrls) {
		mapBaseUrls.append(url);
	}
	maps["base_urls"] = mapBaseUrls;
	maps["download_timeout_seconds"] = static_cast<Json::Int64>(config.mapDownloadTimeoutSeconds);

	Json::Value engine(Json::objectValue);
	Json::Value providers(Json::arrayValue);
	for (const DownloaderEngineProvider& provider : config.engineProviders) {
		Json::Value item(Json::objectValue);
		item["type"] = provider.type;
		item["url"] = provider.url;
		if (!provider.name.empty()) {
			item["name"] = provider.name;
		}
		providers.append(item);
	}
	engine["providers"] = providers;
	engine["download_timeout_seconds"] =
	    static_cast<Json::Int64>(config.engineDownloadTimeoutSeconds);

	root["rapid"] = rapid;
	root["maps"] = maps;
	root["engine"] = engine;

	Json::StreamWriterBuilder writerBuilder;
	writerBuilder["indentation"] = "  ";
	return Json::writeString(writerBuilder, root);
}

static bool WriteFileAtomically(const std::string& path, const std::string& content,
				std::string& error)
{
	const std::string tempPath = path + ".tmp";

	FILE* tempFile = std::fopen(tempPath.c_str(), "wb");
	if (tempFile == nullptr) {
		error = "cannot open temp file: " + std::string(std::strerror(errno));
		return false;
	}

	const size_t written = std::fwrite(content.data(), 1, content.size(), tempFile);
	if (written != content.size()) {
		error = "cannot write temp file";
		std::fclose(tempFile);
		std::remove(tempPath.c_str());
		return false;
	}
	if (std::fflush(tempFile) != 0) {
		error = "cannot flush temp file: " + std::string(std::strerror(errno));
		std::fclose(tempFile);
		std::remove(tempPath.c_str());
		return false;
	}
	if (std::fclose(tempFile) != 0) {
		error = "cannot close temp file: " + std::string(std::strerror(errno));
		std::remove(tempPath.c_str());
		return false;
	}
	if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
		error = "cannot replace target file: " + std::string(std::strerror(errno));
		std::remove(tempPath.c_str());
		return false;
	}

	return true;
}

static EffectiveSourcesConfig LoadEffectiveSourcesConfig()
{
	const DownloaderSourcesConfig fileConfig =
	    LoadDownloaderSourcesConfig(SlPaths::GetLobbyWriteDir());

	switch (fileConfig.loadState) {
		case DownloaderSourcesLoadState::LoadedFromFile: {
			EffectiveSourcesConfig config;
			config.loadedFromSourcesFile = true;
			config.rapidMasterUrls = fileConfig.rapidMasterUrls;
			config.mapBaseUrls = fileConfig.mapBaseUrls;
			config.engineProviders = fileConfig.engineProviders;
			config.rapidRepoTimeoutSeconds = fileConfig.rapidRepoTimeoutSeconds;
			config.mapDownloadTimeoutSeconds = fileConfig.mapDownloadTimeoutSeconds;
			config.engineDownloadTimeoutSeconds = fileConfig.engineDownloadTimeoutSeconds;
			config.sourcesFilePath = fileConfig.path;
			if (config.engineProviders.empty()) {
				config.engineProviders = MakeDefaultEngineProviders();
			}
			return config;
		}
		case DownloaderSourcesLoadState::InvalidFile: {
			EffectiveSourcesConfig config = MakeSafeDefaultsSourcesConfig();
			config.usingSafeDefaultsBecauseFileInvalid = true;
			config.sourcesFilePath = fileConfig.path;
			config.sourcesFileError = fileConfig.error;
			return config;
		}
		case DownloaderSourcesLoadState::Missing:
		default:
			break;
	}

	EffectiveSourcesConfig legacyConfig = MakeLegacySourcesConfig();
	legacyConfig.sourcesFilePath = fileConfig.path;

	const std::string content = BuildSourcesConfigJson(legacyConfig);
	std::string writeError;
	if (!WriteFileAtomically(fileConfig.path, content, writeError)) {
		wxLogWarning("Could not create downloader sources file '%s': %s. Using legacy /Spring/* settings.",
			     fileConfig.path.c_str(), writeError.c_str());
		return legacyConfig;
	}

	const DownloaderSourcesConfig reloaded =
	    LoadDownloaderSourcesConfig(SlPaths::GetLobbyWriteDir());
	if (reloaded.loadState == DownloaderSourcesLoadState::LoadedFromFile) {
		EffectiveSourcesConfig config;
		config.loadedFromSourcesFile = true;
		config.createdSourcesFile = true;
		config.rapidMasterUrls = reloaded.rapidMasterUrls;
		config.mapBaseUrls = reloaded.mapBaseUrls;
		config.engineProviders = reloaded.engineProviders;
		config.rapidRepoTimeoutSeconds = reloaded.rapidRepoTimeoutSeconds;
		config.mapDownloadTimeoutSeconds = reloaded.mapDownloadTimeoutSeconds;
		config.engineDownloadTimeoutSeconds = reloaded.engineDownloadTimeoutSeconds;
		config.sourcesFilePath = reloaded.path;
		if (config.engineProviders.empty()) {
			config.engineProviders = MakeDefaultEngineProviders();
		}
		return config;
	}

	wxLogWarning("Created downloader sources file '%s', but could not reload it (%s). Using legacy /Spring/* settings.",
		     fileConfig.path.c_str(), reloaded.error.c_str());
	return legacyConfig;
}

static void MaybeLogSourcesConfigWarning(const EffectiveSourcesConfig& config)
{
	if (!config.usingSafeDefaultsBecauseFileInvalid) {
		return;
	}

	static std::mutex warningMutex;
	static bool warningShown = false;
	static std::string warningSignature;

	const std::string signature = config.sourcesFilePath + "|" + config.sourcesFileError;
	{
		std::lock_guard<std::mutex> lock(warningMutex);
		if (warningShown && warningSignature == signature) {
			return;
		}
		warningShown = true;
		warningSignature = signature;
	}

	wxLogWarning("Invalid downloader source config '%s': %s. Using safe built-in defaults.",
		     config.sourcesFilePath.c_str(), config.sourcesFileError.c_str());
}

static void MaybeShowSourcesConfigInfoPopup(const EffectiveSourcesConfig& config)
{
	std::string message;
	if (config.createdSourcesFile) {
		message = "Created and loaded downloader sources from: " + config.sourcesFilePath;
	} else if (config.loadedFromSourcesFile) {
		message = "Loaded downloader sources from: " + config.sourcesFilePath;
	} else if (config.usingSafeDefaultsBecauseFileInvalid) {
		message = "Could not load downloader sources file:\n" + config.sourcesFilePath +
			  "\n\nReason: " + config.sourcesFileError +
			  "\n\nUsing built-in defaults for this session.";
	} else {
		message = "Downloader sources file not found:\n" + SlPaths::GetLobbyWriteDir() +
			  "sources.json\n\nUsing legacy /Spring/* settings.";
	}

	static std::mutex popupMutex;
	static std::string shownSignature;
	{
		std::lock_guard<std::mutex> lock(popupMutex);
		if (shownSignature == message) {
			return;
		}
		shownSignature = message;
	}

	wxApp* app = wxTheApp;
	if (app == nullptr) {
		return;
	}

	const wxString wxMessage = TowxString(message);
	app->CallAfter([wxMessage]() {
		if (!ui().IsMainWindowCreated()) {
			return;
		}
		wxCommandEvent* event =
		    new wxCommandEvent(wxEVT_SHOW, MainWindow::mySHOW_INFO_MESSAGE);
		event->SetString(wxMessage);
		ui().mw().GetEventHandler()->QueueEvent(event);
	});
}

static void ApplyEffectiveSourcesConfig(const EffectiveSourcesConfig& config)
{
	if (!config.rapidMasterUrls.empty()) {
		rapidDownload->setOption("masterurl", config.rapidMasterUrls.front());
	}
	rapidDownload->setOption("repo_timeout_seconds",
				 std::to_string(config.rapidRepoTimeoutSeconds));
	httpDownload->setOption("map_download_timeout_seconds",
				std::to_string(config.mapDownloadTimeoutSeconds));
	httpDownload->setOption("engine_download_timeout_seconds",
				std::to_string(config.engineDownloadTimeoutSeconds));
	httpDownload->setOption("engine_providers", BuildEngineProvidersJson(config.engineProviders));

	if (config.mapBaseUrls.empty()) {
		httpDownload->setOption("map_base_url", "");
		httpDownload->setOption("map_base_urls", "");
		return;
	}

	httpDownload->setOption("map_base_url", config.mapBaseUrls.front());
	httpDownload->setOption("map_base_urls", JoinWithNewlines(config.mapBaseUrls));
}

static int SearchOnRapidSource(const DownloadEnum::Category category,
			       const std::string& name,
			       const std::string& rapidMasterUrl)
{
	rapidDownload->setOption("masterurl", rapidMasterUrl);
	return DownloadSearch(category, name.c_str());
}

class DownloadItem : public LSL::WorkItem
{
private:
	DownloadEnum::Category m_category;
	std::string m_name;
	std::string m_filename;

public:
	DownloadItem(const DownloadEnum::Category cat, const std::string& name, const std::string& filename)
	    : m_category(cat)
	    , m_name(name)
	    , m_filename(filename)
	{
		slLogDebugFunc("");
	}

	void Run()
	{
		slLogDebugFunc("");
		wxLogInfo("Starting download of filename: %s, name: %s, category: %s", m_filename.c_str(), m_name.c_str(), DownloadEnum::getCat(m_category).c_str());

		StartDownloadProgressTracking(m_name);
		bool progressFinalized = false;
		bool downloadStartedEventSent = false;
		auto finalizeProgress = [&](bool success) {
			if (progressFinalized) {
				return;
			}
			FinishDownloadProgressTracking(m_name, success);
			if (downloadStartedEventSent) {
				GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadProgress);
			}
			progressFinalized = true;
		};

		try {
			const bool force = true;
			DownloadSetConfig(CONFIG_RAPID_FORCEUPDATE, &force);
			const EffectiveSourcesConfig sourceConfig = LoadEffectiveSourcesConfig();
			MaybeLogSourcesConfigWarning(sourceConfig);
			ApplyEffectiveSourcesConfig(sourceConfig);

			if (m_category == DownloadEnum::CAT_SPRINGLOBBY ||
			    m_category == DownloadEnum::CAT_HTTP) {
				const int results = DownloadAddByUrl(m_category, m_filename.c_str(), m_name.c_str());
				if (results <= 0) {
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
					wxLogInfo("Nothing found to download");
					finalizeProgress(false);
					return;
				}

				downloadInfo info;
				const bool hasdlinfo = DownloadGetInfo(0, info);
				if (!hasdlinfo) {
					wxLogWarning("Download has no downloadinfo: %s", m_name.c_str());
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
					finalizeProgress(false);
					return;
				}

				downloadStartedEventSent = true;
				GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadStarted);
				const bool downloadFailed = DownloadStart();
				if (downloadFailed) {
					wxLogWarning("Download failed: %s", m_name.c_str());
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
					finalizeProgress(false);
				} else {
					wxLogInfo("Download finished: %s", m_name.c_str());
					DownloadFinished(m_category, info);
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadComplete);
					finalizeProgress(true);
				}
				return;
			}

			// For rapid categories, retry full search+download on each configured source.
			// This covers failures during both metadata lookup and the actual download start.
			std::vector<std::string> rapidUrls = sourceConfig.rapidMasterUrls;
			if (rapidUrls.empty()) {
				rapidUrls.push_back(GetRapidMasterUrl());
				const std::string fallback = GetRapidMasterFallbackUrl();
				if (!fallback.empty() && fallback != rapidUrls.front()) {
					rapidUrls.push_back(fallback);
				}
			}

			for (size_t idx = 0; idx < rapidUrls.size(); ++idx) {
				const std::string& rapidUrl = rapidUrls[idx];
				const int results = SearchOnRapidSource(m_category, m_name, rapidUrl);
				if (results <= 0) {
					if (idx + 1 < rapidUrls.size()) {
						wxLogInfo("No rapid matches on %s for '%s', retrying with %s",
							  rapidUrl.c_str(), m_name.c_str(),
							  rapidUrls[idx + 1].c_str());
					}
					continue;
				}

				DownloadAdd(0); // add first result only
				downloadInfo info;
				if (!DownloadGetInfo(0, info)) {
					wxLogWarning("Download has no downloadinfo on %s: %s",
						     rapidUrl.c_str(), m_name.c_str());
					continue;
				}

				if (!downloadStartedEventSent) {
					downloadStartedEventSent = true;
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadStarted);
				}

				if (!DownloadStart()) {
					wxLogInfo("Download finished: %s (source %s)", m_name.c_str(),
						  rapidUrl.c_str());
					DownloadFinished(m_category, info);
					GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadComplete);
					finalizeProgress(true);
					return;
				}

				if (idx + 1 < rapidUrls.size()) {
					wxLogWarning("Download failed on %s for '%s', retrying with %s",
						     rapidUrl.c_str(), m_name.c_str(),
						     rapidUrls[idx + 1].c_str());
				} else {
					wxLogWarning("Download failed on %s for '%s'",
						     rapidUrl.c_str(), m_name.c_str());
				}
			}

			GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
			finalizeProgress(false);
		} catch (...) {
			finalizeProgress(false);
			throw;
		}
	}

	DownloadEnum::Category getCategory() const
	{
		return m_category;
	}
	const std::string& getFilename() const
	{
		return m_filename;
	}
	const std::string& getName() const
	{
		return m_name;
	}

private:
	void DownloadFinished(DownloadEnum::Category cat, const downloadInfo& info)
	{
		slLogDebugFunc("");

		switch (cat) {
			case DownloadEnum::CAT_ENGINE:
			case DownloadEnum::CAT_ENGINE_LINUX:
			case DownloadEnum::CAT_ENGINE_WINDOWS:
			case DownloadEnum::CAT_ENGINE_WINDOWS64:
			case DownloadEnum::CAT_ENGINE_LINUX64:
			case DownloadEnum::CAT_ENGINE_MACOSX: {
				SlPaths::RefreshSpringVersionList(); //FIXME: maybe not thread-save!
				std::string version;
				const std::string prefix = "spring ";
				if (m_name.compare(0, prefix.size(), prefix) == 0)
					version = m_name.substr(prefix.size()); //FIXME: hack
				else
					version = m_name;

				SlPaths::SetUsedSpringIndex(version);
				// Reload unitsync on the GUI thread (some engine bundles crash when initialized from the downloader worker thread).
				GlobalEventManager::Instance()->Send(GlobalEventManager::OnUnitsyncReloadRequest);
				break;
			}
			case DownloadEnum::CAT_SPRINGLOBBY: {
				const std::string& updatedir = SlPaths::GetUpdateDir();
				const std::string& zipfile = info.filename;
				if (!fileSystem->extract(zipfile, updatedir)) {
					wxLogError("Couldn't extract %s to %s", zipfile.c_str(), updatedir.c_str());
					break;
				}
				GlobalEventManager::Instance()->Send(GlobalEventManager::OnLobbyDownloaded);
				break;
			}
			case DownloadEnum::CAT_MAP:
			case DownloadEnum::CAT_GAME:
				if (!LSL::usync().ReloadUnitSyncLib()) {
					wxLogWarning("Couldn't reload unitsync!");
					break;
				}
				if (cat == DownloadEnum::CAT_MAP) {
					LSL::usync().PrefetchMap(m_name);
				} else {
					LSL::usync().PrefetchGame(m_name);
				}
				GlobalEventManager::Instance()->Send(GlobalEventManager::OnUnitsyncReloaded);
				break;
			default:
				wxLogError("Unknown category: %d", cat);
				assert(false);
				break;
		}
	}
};

void PrDownloader::GetProgress(DownloadProgress& progress)
{
	std::lock_guard<std::mutex> lock(dlProgressMutex);

	if (m_progress == nullptr) {
		assert(false);
		return;
	}

	progress.name = m_progress->name;
	progress.downloaded = m_progress->downloaded;
	progress.filesize = m_progress->filesize;
	progress.running = m_progress->running;
	progress.failed = m_progress->failed;

	wxLogDebug("%s %d %d", progress.name.c_str(), progress.downloaded, progress.filesize);
}

#ifdef _WIN32
#define BILLION (1E9)

static BOOL g_first_time = 1;
static LARGE_INTEGER g_counts_per_sec;

int clock_gettime(int dummy, struct timespec* ct)
{
	LARGE_INTEGER count;
	if (g_first_time) {
		g_first_time = 0;
		if (0 == QueryPerformanceFrequency(&g_counts_per_sec)) {
			g_counts_per_sec.QuadPart = 0;
		}
	}

	if ((NULL == ct) || (g_counts_per_sec.QuadPart <= 0) || (0 == QueryPerformanceCounter(&count))) {
		return -1;
	}
	ct->tv_sec = count.QuadPart / g_counts_per_sec.QuadPart;
	ct->tv_nsec = ((count.QuadPart % g_counts_per_sec.QuadPart) * BILLION) / g_counts_per_sec.QuadPart;
	return 0;
}
#endif

void updatelistener(int downloaded, int filesize)
{
	std::lock_guard<std::mutex> lock(dlProgressMutex);

	if (m_progress == nullptr)
		m_progress = new PrDownloader::DownloadProgress();

	static struct timespec lastupdate = {0, 0};
	struct timespec now = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &now);

	// rate limit
	if (((double)now.tv_sec + 1.0e-9 * now.tv_nsec) - ((double)lastupdate.tv_sec + 1.0e-9 * lastupdate.tv_nsec) < 0.2f) {
		// always log 100%
		if (downloaded != filesize) {
			return;
		}
	}

	lastupdate.tv_nsec = now.tv_nsec;
	lastupdate.tv_sec = now.tv_sec;

	m_progress->filesize = filesize;
	m_progress->downloaded = downloaded;
	m_progress->running = true;
	m_progress->failed = false;

	GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadProgress);
}

PrDownloader::PrDownloader()
    : wxEvtHandler()
    , m_dl_thread(new LSL::WorkerThread())
{
	slLogDebugFunc("");

	UpdateSettings();
	IDownloader::Initialize();
	IDownloader::setProcessUpdateListener(updatelistener);
	SUBSCRIBE_GLOBAL_EVENT(GlobalEventManager::OnSpringStarted, PrDownloader::OnSpringStarted);
	SUBSCRIBE_GLOBAL_EVENT(GlobalEventManager::OnSpringTerminated, PrDownloader::OnSpringTerminated);
}

PrDownloader::~PrDownloader()
{
	slLogDebugFunc("");

	GlobalEventManager::Instance()->UnSubscribeAll(this);

	if (!!m_dl_thread) {
		m_dl_thread->Wait();
		delete m_dl_thread;
		m_dl_thread = nullptr;
	}
	IDownloader::Shutdown();

	if (!!m_progress) {
		delete m_progress;
		m_progress = nullptr;
	}
}

void PrDownloader::ClearFinished()
{
	slLogDebugFunc("");
}

void PrDownloader::UpdateSettings()
{
	slLogDebugFunc("");

	DownloadSetConfig(CONFIG_FILESYSTEM_WRITEPATH, SlPaths::GetDownloadDir().c_str());
	//FIXME: fileSystem->setEnginePortableDownload(cfg().ReadBool(_T("/Spring/PortableDownload")));
	const EffectiveSourcesConfig sourceConfig = LoadEffectiveSourcesConfig();
	MaybeLogSourcesConfigWarning(sourceConfig);
	MaybeShowSourcesConfigInfoPopup(sourceConfig);
	ApplyEffectiveSourcesConfig(sourceConfig);
}

void PrDownloader::RemoveTorrentByName(const std::string& /*name*/)
{
	slLogDebugFunc("");
}

void PrDownloader::Download(DownloadEnum::Category cat, const std::string& filename, const std::string& url)
{
	slLogDebugFunc("");

	wxLogDebug("Starting download of %s, %s %d", filename.c_str(), url.c_str(), cat);
	DownloadItem* dl_item = new DownloadItem(cat, filename, url);
	m_dl_thread->DoWork(dl_item);
}


void PrDownloader::OnSpringStarted(wxCommandEvent& /*data*/)
{
	slLogDebugFunc("");
	//FIXME: pause downloads
}

void PrDownloader::OnSpringTerminated(wxCommandEvent& /*data*/)
{
	slLogDebugFunc("");
	//FIXME: resume downloads
}

PrDownloader& prDownloader()
{
	static LSL::Util::LineInfo<PrDownloader> m(AT);
	static LSL::Util::GlobalObjectHolder<PrDownloader, LSL::Util::LineInfo<PrDownloader> > s_PrDownloader(m);
	return s_PrDownloader;
}

bool PrDownloader::IsRunning()
{
	slLogDebugFunc("");
	std::lock_guard<std::mutex> lock(dlProgressMutex);
	return m_progress != nullptr && m_progress->running;
}

void PrDownloader::UpdateApplication(const std::string& updateurl)
{
	slLogDebugFunc("");

	const std::string updatedir = SlPaths::GetUpdateDir();
	const size_t mindirlen = 9; // safety, minimal is/should be: C:\update
	if ((updatedir.size() <= mindirlen)) {
		wxLogError(_T("Invalid update dir: ") + TowxString(updatedir));
		return;
	}
	if (wxDirExists(updatedir)) {
		if (!SlPaths::RmDir(updatedir)) {
			wxLogError(_T("Couldn't cleanup ") + TowxString(updatedir));
			return;
		}
	}
	if (!SlPaths::mkDir(updatedir)) {
		wxLogError(_T("couldn't create update directory") + TowxString(updatedir));
		return;
	}

	if (!wxFileName::IsDirWritable(updatedir)) {
		wxLogError(_T("dir not writable: ") + TowxString(updatedir));
		return;
	}

	const std::string dlfilepath = SlPaths::GetLobbyWriteDir() + "springlobby-latest.zip";
	if (wxFileExists(dlfilepath) && !(wxRemoveFile(dlfilepath))) {
		wxLogError(_T("couldn't delete: ") + dlfilepath);
		return;
	}
	Download(DownloadEnum::CAT_SPRINGLOBBY, updateurl, dlfilepath);
}

bool PrDownloader::DownloadUrl(const std::string& httpurl, std::string& res)
{
	UpdateSettings();
	StartDownloadProgressTracking(httpurl);
	const bool ok = CHttpDownloader::DownloadUrl(httpurl, res);
	FinishDownloadProgressTracking(httpurl, ok);
	GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadProgress);
	return ok;
}
