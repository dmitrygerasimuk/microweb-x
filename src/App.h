//
// Copyright (C) 2021 James Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#ifndef _APP_H_
#define _APP_H_

#include <stdio.h>
#include <time.h>
#include "Parser.h"
#include "Page.h"
#include "URL.h"
#include "Interface.h"
#include "Render.h"
#include "HTTP.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define PATH_MAX MAX_PATH
#endif

#define MAX_PAGE_HISTORY_BUFFER_SIZE MAX_URL_LENGTH
#define APP_LOAD_BUFFER_SIZE 512
#define UPDATE_TIME_SLICE (CLOCKS_PER_SEC / 5)		// 200ms time slices for rendering / parsing content buffers
#define DEFAULT_BULK_PUMP_BYTES 0UL

class HTTPRequest;
struct HTTPOptions;

struct LoadTask
{
	LoadTask() : type(LocalFile), fs(NULL), debugDumpFile(NULL), downloadFile(NULL), bytesDownloaded(0), contentSize(0), downloadComplete(false) {}

	void Load(HTTPRequest::RequestType requestType, const char* url, HTTPOptions* options = NULL);
	void Stop();
	bool HasContent();
	bool IsBusy();
	bool NeedsNetworkIdle();
	size_t GetContent(char* buffer, size_t count);
	const char* GetURL();
	const char* GetContentType();
	long GetContentSize();
	long GetBytesDownloaded();
	bool IsDownloadComplete();

	enum Type
	{
		LocalFile,
		RemoteFile,
	};

	URL url;
	Type type;

	union
	{
		FILE* fs;
		HTTPRequest* request;
	};

	FILE* debugDumpFile;
	FILE* downloadFile;
	long bytesDownloaded;
	long contentSize;
	bool downloadComplete;
};

struct Widget;

struct AppConfig
{
	bool loadImages : 1;
	bool dumpPage : 1;
	bool invertScreen : 1;
	bool useSwap : 1;
	bool useEMS : 1;
	bool useXMS : 1;
	bool debugMemoryLog : 1;
	bool transliterateCyrillic : 1;
	bool legacyNetworkPump : 1;
	bool pageDrain : 1;
	bool networkStats : 1;
	unsigned int linearAllocatorChunkSize;
	unsigned long bulkPumpBytes;
};

class App
{
public:
	App();
	~App();

	void Run(int argc, char* argv[]);
	void Close() { running = false; }
	void OpenURL(HTTPRequest::RequestType requestType, const char* url, HTTPOptions* options = NULL);

	void PreviousPage();
	void NextPage();

	void StopLoad();
	void ReloadPage();

	void BeginFileDownload(const char* savePath);
	void CancelFileDownload();
	void ShowErrorPage(const char* message);
	bool IsBulkTransferActive() const { return bulkTransferActive; }

	static App& Get() { return *app; }

	Page page;
	PageRenderer pageRenderer;
	HTMLParser parser;
	AppInterface ui;
	static AppConfig config;

	LoadTask pageLoadTask;
	LoadTask pageContentLoadTask;

	void LoadImageNodeContent(Node* node);

private:
	void ResetPage();
	void RequestNewPage(HTTPRequest::RequestType requestType, const char* url, HTTPOptions* options = NULL);

	void ShowNoHTTPSPage();
	void ShowDownloadDialogPage();
	void ShowDownloadProgressPage(const char* savePath);
	void ShowDownloadEndedPage(const char* message);
	void FinishFileDownload();
	void SetBulkTransferActive(bool active);
	void PumpBulkTransfer();
	void MaybeLogBulkTransferStats(bool force = false);
	void LogLoadTaskNetStats(const char* name, LoadTask& task, bool force = false);
	void MaybeLogLoopNetStats(bool force = false);
	void LogZeroReadNetStats(const char* name, LoadTask& task, clock_t& lastLogTime, bool includeTarget = false);
	void HandleEmptyRead(const char* name, LoadTask& task, clock_t& lastLogTime, bool includeTarget = false);
	void UpdateIdleNetwork();

	bool requestedNewPage;
	bool bulkTransferActive;
	Node* loadTaskTargetNode;
	bool running;

	char pageHistoryBuffer[MAX_PAGE_HISTORY_BUFFER_SIZE];
	char* pageHistoryPtr;

	static App* app;

	clock_t bulkTransferStartTime;
	clock_t bulkTransferFirstByteTime;
	clock_t bulkTransferLastStatsTime;
	long bulkTransferLastStatsBytes;
	unsigned long bulkTransferReadCalls;
	unsigned long bulkTransferZeroReads;
	unsigned long bulkTransferPumpCalls;

	int pageLoadStatsStatus;
	int pageContentStatsStatus;
	char pageLoadStatsText[48];
	char pageContentStatsText[48];
	long pageLoadStatsBytes;
	long pageContentStatsBytes;
	clock_t pageLoadStatsTime;
	clock_t pageContentStatsTime;
	clock_t loopStatsTime;

	char loadBuffer[APP_LOAD_BUFFER_SIZE];
};


#endif
