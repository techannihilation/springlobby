/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#ifndef SPRINGLOBBY_HEADERGUARD_GLOBALEVENTS_H
#define SPRINGLOBBY_HEADERGUARD_GLOBALEVENTS_H

#include <wx/event.h>
#include <map>
#include <set>
#include "utils/conversion.h"
class GlobalEventManager
{
private:
	GlobalEventManager();
	~GlobalEventManager();

public:
	static GlobalEventManager* Instance();
	static void Release();

public:
	void Subscribe(wxEvtHandler* evh, wxEventType id, wxObjectEventFunction func, const std::string& debuginfo);
	void UnSubscribe(wxEvtHandler* evh, wxEventType id = 0);
	void UnSubscribeAll(wxEvtHandler* evh);

	void Send(wxEventType type);
	void Send(wxCommandEvent event);
	void Send(wxEventType type, void* clientData);

private:
	void _Connect(wxEvtHandler* evthandler, wxEventType id, wxObjectEventFunction func, const std::string& debuginfo);
	void _Disconnect(wxEvtHandler* evthandler, wxEventType id = 0);

public:
	static const wxEventType OnDownloadStarted;
	static const wxEventType OnDownloadComplete;
	static const wxEventType OnDownloadFailed;
	static const wxEventType OnDownloadProgress;
	static const wxEventType OnRapidValidateComplete;
	static const wxEventType OnRapidValidateFailed;
	static const wxEventType OnUnitsyncFirstTimeLoad;
	static const wxEventType OnUnitsyncReloaded;
	static const wxEventType OnUnitsyncReloadRequest;
	static const wxEventType OnUnitsyncReloadRequestPostDownload;
	static const wxEventType OnUnitsyncReloadFailed;
	static const wxEventType OnSpringTerminated;
	static const wxEventType OnSpringStarted;
	static const wxEventType UpdateFinished;
	static const wxEventType OnQuit;
	static const wxEventType OnLogin;
	static const wxEventType OnUpdateFinished;
	static const wxEventType OnLobbyDownloaded;

	static const wxEventType BattleSyncReload;
	static const wxEventType BattleStartedEvent;
	static const wxEventType ApplicationSettingsChangedEvent;

private:
	static GlobalEventManager* m_Instance;

private:
	bool m_eventsDisabled;
	std::map<wxEventType, std::map<wxEvtHandler*, const std::string> > m_eventsTable;
	const int ANY_EVENT = 0;
};

template <typename T>
static inline wxObjectEventFunction SlWxObjectEventFunction(T func)
{
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
	wxObjectEventFunction result = wxObjectEventFunction(func);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
	return result;
}

#define SUBSCRIBE_GLOBAL_EVENT(event, callbackfunc) \
	GlobalEventManager::Instance()->Subscribe(this, event, SlWxObjectEventFunction(&callbackfunc), stdprintf("%s:%d %s()", __FILE__, __LINE__, __func__))


#endif // SPRINGLOBBY_HEADERGUARD_GLOBALEVENTS_H
