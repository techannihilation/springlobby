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
#include <wx/log.h>
#include <algorithm>
#include <cctype>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "settings.h"
#include "utils/conversion.h"
#include "utils/globalevents.h"
#include "utils/slconfig.h"
#include "utils/slpaths.h"

SLCONFIG("/Spring/PortableDownload", false, "true to download portable versions of spring, if false cache/settings/etc are shared (bogous!)");
SLCONFIG("/Spring/RapidMasterUrl", "https://rapid.techa-rts.com/repos.gz", "primary master url for rapid downloads");
SLCONFIG("/Spring/RapidMasterFallbackUrl", "https://repos.springrts.com/repos.gz", "fallback master url for rapid downloads");
SLCONFIG("/Spring/MapDownloadBaseUrl", "http://www.hakora.xyz/files/springrts/maps/", "primary base URL for map downloads (.sd7/.sdz)");
SLCONFIG("/Spring/RapidGitEnabled", true, "enable git-first rapid tag resolution before repos.gz");
SLCONFIG("/Spring/RapidGitManifestUrl", "https://rapid.techa-rts.com/git-manifest.json", "manifest URL for rapid git tag mapping");
SLCONFIG("/Spring/RapidGitManifestTtlSeconds", 300l, "cache ttl for rapid git manifest in seconds");
SLCONFIG("/Spring/RapidGitApiTimeoutSeconds", 20l, "timeout for rapid git API calls in seconds");

static PrDownloader::DownloadProgress* m_progress = nullptr;
static std::mutex dlProgressMutex;

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

static bool GetRapidGitEnabled()
{
	return cfg().ReadBool("/Spring/RapidGitEnabled");
}

static std::string GetRapidGitManifestUrl()
{
	return STD_STRING(cfg().ReadString("/Spring/RapidGitManifestUrl"));
}

static long GetRapidGitManifestTtlSeconds()
{
	return cfg().ReadLong("/Spring/RapidGitManifestTtlSeconds");
}

static long GetRapidGitApiTimeoutSeconds()
{
	return cfg().ReadLong("/Spring/RapidGitApiTimeoutSeconds");
}

struct EffectiveSourcesConfig
{
	std::vector<std::string> rapidMasterUrls;
	std::vector<std::string> mapBaseUrls;
	bool rapidGitEnabled = true;
	std::string rapidGitManifestUrl;
	long rapidGitManifestTtlSeconds = 300;
	long rapidGitApiTimeoutSeconds = 20;
	bool loadedFromSourcesFile = false;
	bool usingSafeDefaultsBecauseFileInvalid = false;
	std::string sourcesFilePath;
	std::string sourcesFileError;
};

static constexpr const char* kDefaultRapidMasterPrimary = "https://rapid.techa-rts.com/repos.gz";
static constexpr const char* kDefaultRapidMasterSecondary = "https://repos.springrts.com/repos.gz";
static constexpr const char* kDefaultMapBase = "http://www.hakora.xyz/files/springrts/maps/";
static constexpr const char* kDefaultRapidGitManifestUrl = "https://rapid.techa-rts.com/git-manifest.json";

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

static EffectiveSourcesConfig MakeSafeDefaultsSourcesConfig()
{
	EffectiveSourcesConfig config;
	config.rapidMasterUrls = {kDefaultRapidMasterPrimary, kDefaultRapidMasterSecondary};
	config.mapBaseUrls = {kDefaultMapBase};
	config.rapidGitEnabled = true;
	config.rapidGitManifestUrl = kDefaultRapidGitManifestUrl;
	config.rapidGitManifestTtlSeconds = 300;
	config.rapidGitApiTimeoutSeconds = 20;
	return config;
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
			config.rapidGitEnabled = fileConfig.rapidGitEnabled;
			config.rapidGitManifestUrl = fileConfig.rapidGitManifestUrl;
			config.rapidGitManifestTtlSeconds =
			    fileConfig.rapidGitManifestTtlSeconds;
			config.rapidGitApiTimeoutSeconds =
			    fileConfig.rapidGitApiTimeoutSeconds;
			config.sourcesFilePath = fileConfig.path;
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

	EffectiveSourcesConfig legacyConfig;
	AppendUniqueUrl(legacyConfig.rapidMasterUrls, GetRapidMasterUrl());
	AppendUniqueUrl(legacyConfig.rapidMasterUrls, GetRapidMasterFallbackUrl());
	AppendUniqueUrl(legacyConfig.mapBaseUrls, NormalizeMapBaseUrl(GetMapDownloadBaseUrl()));
	legacyConfig.rapidGitEnabled = GetRapidGitEnabled();
	legacyConfig.rapidGitManifestUrl = GetRapidGitManifestUrl();
	legacyConfig.rapidGitManifestTtlSeconds = GetRapidGitManifestTtlSeconds();
	legacyConfig.rapidGitApiTimeoutSeconds = GetRapidGitApiTimeoutSeconds();

	if (legacyConfig.rapidMasterUrls.empty()) {
		legacyConfig.rapidMasterUrls.push_back(kDefaultRapidMasterPrimary);
	}
	if (legacyConfig.mapBaseUrls.empty()) {
		legacyConfig.mapBaseUrls.push_back(kDefaultMapBase);
	}
	if (legacyConfig.rapidGitManifestUrl.empty()) {
		legacyConfig.rapidGitManifestUrl = kDefaultRapidGitManifestUrl;
	}

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

static void ApplyEffectiveSourcesConfig(const EffectiveSourcesConfig& config)
{
	if (!config.rapidMasterUrls.empty()) {
		rapidDownload->setOption("masterurl", config.rapidMasterUrls.front());
	}
	rapidDownload->setOption("git_enabled", config.rapidGitEnabled ? "1" : "0");
	rapidDownload->setOption("git_manifest_url", config.rapidGitManifestUrl);
	rapidDownload->setOption("git_manifest_ttl",
				 std::to_string(config.rapidGitManifestTtlSeconds));
	rapidDownload->setOption("git_api_timeout",
				 std::to_string(config.rapidGitApiTimeoutSeconds));

	if (config.mapBaseUrls.empty()) {
		httpDownload->setOption("map_base_url", "");
		httpDownload->setOption("map_base_urls", "");
		return;
	}

	httpDownload->setOption("map_base_url", config.mapBaseUrls.front());
	httpDownload->setOption("map_base_urls", JoinWithNewlines(config.mapBaseUrls));
}

static bool IsRapidSearchCategory(const DownloadEnum::Category category)
{
	return category == DownloadEnum::CAT_GAME || category == DownloadEnum::CAT_COUNT || category == DownloadEnum::CAT_NONE;
}

static int SearchWithRapidFallback(const DownloadEnum::Category category,
				   const std::string& name,
				   const std::vector<std::string>& rapidMasterUrls)
{
	if (!IsRapidSearchCategory(category)) {
		return DownloadSearch(category, name.c_str());
	}
	if (rapidMasterUrls.empty()) {
		return DownloadSearch(category, name.c_str());
	}

	for (size_t i = 0; i < rapidMasterUrls.size(); ++i) {
		const std::string& currentUrl = rapidMasterUrls[i];
		rapidDownload->setOption("masterurl", currentUrl);
		const int results = DownloadSearch(category, name.c_str());
		if (results > 0) {
			return results;
		}
		if (i + 1 < rapidMasterUrls.size()) {
			wxLogInfo("No rapid matches on %s for '%s', retrying with %s",
				  currentUrl.c_str(), name.c_str(),
				  rapidMasterUrls[i + 1].c_str());
		}
	}

	return 0;
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

		{
			std::lock_guard<std::mutex> lock(dlProgressMutex);

			if (m_progress == nullptr)
				m_progress = new PrDownloader::DownloadProgress();
			m_progress->name = m_name;
		}

		const bool force = true;
		DownloadSetConfig(CONFIG_RAPID_FORCEUPDATE, &force);
		const EffectiveSourcesConfig sourceConfig = LoadEffectiveSourcesConfig();
		MaybeLogSourcesConfigWarning(sourceConfig);
		ApplyEffectiveSourcesConfig(sourceConfig);

		int results = 0;

		switch (m_category) {
			case DownloadEnum::CAT_SPRINGLOBBY:
			case DownloadEnum::CAT_HTTP:
				results = DownloadAddByUrl(m_category, m_filename.c_str(), m_name.c_str());
				break;
			default:
				results = SearchWithRapidFallback(m_category, m_name, sourceConfig.rapidMasterUrls);
				break;
		}
		if (results <= 0) {
			GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
			wxLogInfo("Nothing found to download");
			return;
		}

		for (int i = 0; i < results; i++) {
			DownloadAdd(i);
			break; //only add one result
		}

		downloadInfo info;
		const bool hasdlinfo = DownloadGetInfo(0, info);
		//In case if something gone wrong
		if (!hasdlinfo) {
			wxLogWarning("Download has no downloadinfo: %s", m_name.c_str());
			GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
			return;
		}

		GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadStarted);

		const bool downloadFailed = DownloadStart();

		if (downloadFailed) {
			wxLogWarning("Download failed: %s", m_name.c_str());
			GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadFailed);
		} else {
			wxLogInfo("Download finished: %s", m_name.c_str());
			DownloadFinished(m_category, info);
			GlobalEventManager::Instance()->Send(GlobalEventManager::OnDownloadComplete);
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

	return m_progress != nullptr && !m_progress->IsFinished();
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
	{
		std::lock_guard<std::mutex> lock(dlProgressMutex);
		if (m_progress == nullptr)
			m_progress = new PrDownloader::DownloadProgress();
		m_progress->name = httpurl;
	}
	return CHttpDownloader::DownloadUrl(httpurl, res);
}
