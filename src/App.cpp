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

#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "App.h"
#include "Platform.h"
#include "HTTP.h"
#include "Image/Decoder.h"
#include "Memory/Memory.h"
#include "Memory/MemoryLog.h"

App* App::app;
AppConfig App::config;

static void FormatByteCount(char* buffer, int bufferSize, long bytes)
{
	if (bytes < 1000)
	{
		snprintf(buffer, bufferSize, "%ld B", bytes);
	}
	else if (bytes < 1000000l)
	{
		snprintf(buffer, bufferSize, "%ld KB", bytes / 1000);
	}
	else
	{
		snprintf(buffer, bufferSize, "%ld MB", bytes / 1000000l);
	}
}

static void FormatLoadProgress(char* buffer, int bufferSize, const char* prefix, LoadTask& loadTask)
{
	char downloaded[16];
	char total[16];
	long contentSize = loadTask.GetContentSize();
	FormatByteCount(downloaded, sizeof(downloaded), loadTask.GetBytesDownloaded());
	if (contentSize > 0)
	{
		FormatByteCount(total, sizeof(total), contentSize);
		snprintf(buffer, bufferSize, "%s %s/%s", prefix, downloaded, total);
	}
	else
	{
		snprintf(buffer, bufferSize, "%s %s", prefix, downloaded);
	}
}

static void FormatDownloadResult(char* buffer, int bufferSize, long bytes, clock_t elapsedTicks)
{
	char downloaded[16];
	FormatByteCount(downloaded, sizeof(downloaded), bytes);
	if (elapsedTicks <= 0)
	{
		elapsedTicks = 1;
	}

	long elapsedMs = ((long)elapsedTicks * 1000L) / CLOCKS_PER_SEC;
	if (elapsedMs <= 0)
	{
		elapsedMs = 1;
	}
	long seconds = elapsedMs / 1000L;
	long tenths = (elapsedMs % 1000L) / 100L;
	long speedKbps = ((bytes / 1024L) * CLOCKS_PER_SEC) / (long)elapsedTicks;
	snprintf(buffer, bufferSize, "Downloaded %s in %ld.%ld sec (%ld KB/s)", downloaded, seconds, tenths, speedKbps);
}

static unsigned long ParseByteSize(const char* value)
{
	if (!value || !*value)
	{
		return 0;
	}
	char* end = NULL;
	unsigned long size = strtoul(value, &end, 10);
	if (!size)
	{
		return 0;
	}
	if (end && (*end == 'k' || *end == 'K'))
	{
		size *= 1024;
	}
	else if (size <= 64)
	{
		size *= 1024;
	}
	return size;
}

static unsigned int ParseLinearAllocatorChunkSize(const char* value)
{
	return (unsigned int)ParseByteSize(value);
}

static bool IsCommandLineArg(const char* value, const char* arg)
{
	if (!value || !arg)
	{
		return false;
	}
	if (*value == '-' || *value == '/')
	{
		value++;
	}
	if (*arg == '-' || *arg == '/')
	{
		arg++;
	}
	return !stricmp(value, arg);
}

static void NetworkStatsReset()
{
	FILE* file = fopen("NETSTAT.TXT", "w");
	if (!file)
	{
		file = fopen("C:\\NETSTAT.TXT", "w");
	}
	if (file)
	{
		fprintf(file, "NETSTAT enabled\n");
		fclose(file);
	}
}

static void NetworkStatsLog(const char* fmt, ...)
{
	FILE* file = fopen("NETSTAT.TXT", "a");
	if (!file)
	{
		file = fopen("C:\\NETSTAT.TXT", "a");
	}
	if (!file)
	{
		return;
	}

	va_list args;
	va_start(args, fmt);
	vfprintf(file, fmt, args);
	va_end(args);
	fprintf(file, "\n");
	fclose(file);
}

static const char* NetworkStatsTaskName(LoadTask* task)
{
	if (task == &App::Get().pageLoadTask)
	{
		return "page";
	}
	if (task == &App::Get().pageContentLoadTask)
	{
		return "content";
	}
	return "task";
}

static void PumpNetwork()
{
	if (!App::config.legacyNetworkPump && !App::Get().IsBulkTransferActive() && Platform::network)
	{
		Platform::network->Update();
	}
}

App::App() 
	: page(*this), pageRenderer(*this), parser(page), ui(*this)
{
	app = this;
	requestedNewPage = false;
	bulkTransferActive = false;
	bulkTransferStartTime = 0;
	bulkTransferFirstByteTime = 0;
	bulkTransferLastStatsTime = 0;
	bulkTransferLastStatsBytes = 0;
	bulkTransferReadCalls = 0;
	bulkTransferZeroReads = 0;
	bulkTransferPumpCalls = 0;
	pageLoadStatsStatus = -1;
	pageContentStatsStatus = -1;
	pageLoadStatsText[0] = '\0';
	pageContentStatsText[0] = '\0';
	pageLoadStatsBytes = -1;
	pageContentStatsBytes = -1;
	pageLoadStatsTime = 0;
	pageContentStatsTime = 0;
	loopStatsTime = 0;
	memset(pageHistoryBuffer, 0, MAX_PAGE_HISTORY_BUFFER_SIZE);
	pageHistoryPtr = pageHistoryBuffer;
}

App::~App()
{
	app = NULL;
}

void App::ResetPage()
{
	StylePool::Get().Reset();
	page.Reset();
	parser.Reset();
	pageRenderer.Reset();
	ui.Reset();
	pageRenderer.RefreshAll();
}

void App::Run(int argc, char* argv[])
{
	running = true;
	char* targetURL = nullptr;
	config.loadImages = true;
	config.dumpPage = false;
	config.useSwap = false;
	config.useEMS = true;
	config.useXMS = true;
	config.debugMemoryLog = false;
	config.transliterateCyrillic = false;
	config.legacyNetworkPump = false;
	config.pageDrain = false;
	config.networkStats = false;
	config.bulkPumpBytes = DEFAULT_BULK_PUMP_BYTES;
	config.linearAllocatorChunkSize = LinearAllocator::DefaultChunkSize();
	if (argc > 1)
	{
		for (int n = 1; n < argc; n++)
		{
			if (IsCommandLineArg(argv[n], "netstats") || IsCommandLineArg(argv[n], "netstat") ||
				IsCommandLineArg(argv[n], "downloadstats"))
			{
				config.networkStats = true;
			}
			else if (*argv[n] != '-' && *argv[n] != '/')
			{
				if (!targetURL)
				{
					targetURL = argv[n];
				}
			}
			else if (IsCommandLineArg(argv[n], "noimages"))
			{
				config.loadImages = false;
			}
			else if (IsCommandLineArg(argv[n], "dumppage"))
			{
				config.dumpPage = true;
			}
			else if (IsCommandLineArg(argv[n], "invert"))
			{
				config.invertScreen = true;
			}
			else if (IsCommandLineArg(argv[n], "useswap"))
			{
				config.useSwap = true;
			}
			else if (IsCommandLineArg(argv[n], "noems"))
			{
				config.useEMS = false;
			}
			else if (IsCommandLineArg(argv[n], "noxms"))
			{
				config.useXMS = false;
			}
			else if (IsCommandLineArg(argv[n], "debug") || IsCommandLineArg(argv[n], "debugmem") ||
				IsCommandLineArg(argv[n], "memlog") || IsCommandLineArg(argv[n], "bootlog"))
			{
				config.debugMemoryLog = true;
			}
			else if (IsCommandLineArg(argv[n], "translit") || IsCommandLineArg(argv[n], "transliterate"))
			{
				config.transliterateCyrillic = true;
			}
			else if (IsCommandLineArg(argv[n], "netlegacy"))
			{
				config.legacyNetworkPump = true;
			}
			else if (IsCommandLineArg(argv[n], "pagedrain"))
			{
				config.pageDrain = true;
			}
			else if (strstr(argv[n], "-bulkpump=") == argv[n] || strstr(argv[n], "/bulkpump=") == argv[n])
			{
				config.bulkPumpBytes = ParseByteSize(argv[n] + 10);
			}
			else if (strstr(argv[n], "-linalloc=") == argv[n] || strstr(argv[n], "/linalloc=") == argv[n])
			{
				unsigned int chunkSize = ParseLinearAllocatorChunkSize(argv[n] + 10);
				if (chunkSize)
				{
					config.linearAllocatorChunkSize = chunkSize;
				}
			}
			else if (IsCommandLineArg(argv[n], "linalloc") && n + 1 < argc)
			{
				unsigned int chunkSize = ParseLinearAllocatorChunkSize(argv[n + 1]);
				if (chunkSize)
				{
					config.linearAllocatorChunkSize = chunkSize;
					n++;
				}
			}
		}
	}
	MemoryDebugLogSetEnabled(config.debugMemoryLog);
	if (config.networkStats)
	{
		NetworkStatsReset();
		NetworkStatsLog("config netlegacy=%d pagedrain=%d bulkpump=%lu clocksPerSec=%ld",
			config.legacyNetworkPump, config.pageDrain, config.bulkPumpBytes, (long)CLOCKS_PER_SEC);
		pageLoadStatsStatus = -1;
		pageContentStatsStatus = -1;
		pageLoadStatsText[0] = '\0';
		pageContentStatsText[0] = '\0';
		pageLoadStatsBytes = -1;
		pageContentStatsBytes = -1;
		pageLoadStatsTime = clock();
		pageContentStatsTime = pageLoadStatsTime;
		loopStatsTime = pageLoadStatsTime;
	}
	MemoryDebugLog("BOOT app config parsed images=%d ems=%d xms=%d swap=%d netlegacy=%d pagedrain=%d netstats=%d bulkpump=%lu",
		config.loadImages, config.useEMS, config.useXMS, config.useSwap, config.legacyNetworkPump,
		config.pageDrain, config.networkStats, config.bulkPumpBytes);
	MemoryManager::pageAllocator.SetChunkSize(config.linearAllocatorChunkSize);
	MemoryDebugLog("LINALLOC config chunk=%u", (unsigned)MemoryManager::pageAllocator.GetChunkSize());
	MemoryDebugLog("BOOT memblock init begin");
	MemoryManager::pageBlockAllocator.Init();
	MemoryDebugLog("BOOT memblock init done");
	if (config.loadImages)
	{
		MemoryDebugLog("BOOT image decoder allocate begin");
		ImageDecoder::Allocate();
		MemoryDebugLog("BOOT image decoder allocate done");
	}
	MemoryDebugLog("BOOT style pool init begin");
	StylePool::Get().Init();
	MemoryDebugLog("BOOT style pool init done");
	MemoryDebugLog("BOOT ui init begin");
	ui.Init();
	MemoryDebugLog("BOOT ui init done");
	MemoryDebugLog("BOOT page reset begin");
	page.Reset();
	MemoryDebugLog("BOOT page reset done");
	MemoryDebugLog("BOOT page renderer init begin");
	pageRenderer.Init();
	MemoryDebugLog("BOOT page renderer init done");
	if (targetURL)
	{
		MemoryDebugLog("BOOT initial open url begin");
		OpenURL(HTTPRequest::Get, targetURL);
		MemoryDebugLog("BOOT initial open url done");
	}
	else
	{
		MemoryDebugLog("BOOT initial focus address begin");
		ui.FocusNode(ui.addressBarNode);
		MemoryDebugLog("BOOT initial focus address done");
	}
	bool firstLoop = true;
	MemoryDebugLog("BOOT app main loop begin");
	while (running)
	{
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop platform update begin");
		}
		Platform::Update();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop platform update done");
			MemoryDebugLog("BOOT first loop mouse refresh begin");
		}
		Platform::input->RefreshMouse();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop mouse refresh done");
		}
		LogLoadTaskNetStats("page", pageLoadTask);
		LogLoadTaskNetStats("content", pageContentLoadTask);
		bool pageLoadHasContent = pageLoadTask.HasContent();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop page load begin has=%d requested=%d parserFinished=%d download=%d",
				pageLoadHasContent, requestedNewPage, parser.IsFinished(), pageLoadTask.downloadFile != NULL);
		}
		if (pageLoadHasContent)
		{
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop page load content begin");
			}
			if (requestedNewPage)
			{
				if (firstLoop)
				{
					MemoryDebugLog("BOOT first loop page load requested begin");
				}
				if (pageLoadTask.downloadFile)
				{
					ui.SetStatusMessage("Downloading content...", StatusBarNode::GeneralStatus);
				}
				else
				{
					ResetPage();
					page.pageURL = pageLoadTask.GetURL();
					ui.UpdateAddressBar(page.pageURL);
					loadTaskTargetNode = page.GetRootNode();
					ui.SetStatusMessage("Parsing page content...", StatusBarNode::GeneralStatus);
					if (!parser.SetContentType(pageLoadTask.GetContentType()))
					{
						if (pageLoadTask.type == LoadTask::RemoteFile)
						{
							ShowDownloadDialogPage();
						}
						else
						{
							ShowErrorPage("Unsupported file format");
						}
					}
				}
				requestedNewPage = false;
				if (firstLoop)
				{
					MemoryDebugLog("BOOT first loop page load requested done");
				}
			}
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop page load read begin");
			}
			bool drainPageLoad = config.pageDrain && !pageLoadTask.downloadFile && pageLoadTask.type == LoadTask::RemoteFile;
			clock_t loadEndTime = clock() + UPDATE_TIME_SLICE;
			size_t totalBytesRead = 0;
			unsigned long bulkBytesSincePump = 0;
			if (drainPageLoad)
			{
				PumpNetwork();
			}
			do 
			{
				size_t bytesRead = pageLoadTask.GetContent(loadBuffer, APP_LOAD_BUFFER_SIZE);
				if (bytesRead)
				{
					totalBytesRead += bytesRead;
					if (pageLoadTask.downloadFile)
					{
						if (!bulkTransferFirstByteTime)
						{
							bulkTransferFirstByteTime = clock();
							if (config.networkStats)
							{
								long setupMs = ((long)(bulkTransferFirstByteTime - bulkTransferStartTime) * 1000L) / CLOCKS_PER_SEC;
								NetworkStatsLog("firstByte ticks=%ld setupMs=%ld", (long)(bulkTransferFirstByteTime - bulkTransferStartTime), setupMs);
							}
						}
						fwrite(loadBuffer, 1, bytesRead, pageLoadTask.downloadFile);
						bulkTransferReadCalls++;
						bulkBytesSincePump += bytesRead;
						if (config.bulkPumpBytes && bulkBytesSincePump >= config.bulkPumpBytes)
						{
							PumpBulkTransfer();
							bulkBytesSincePump = 0;
						}
					}
					else
					{
						if (pageLoadTask.debugDumpFile)
						{
							fwrite(loadBuffer, 1, bytesRead, pageLoadTask.debugDumpFile);
						}
						parser.Parse(loadBuffer, bytesRead);
					}
				}
				else
				{
					HandleEmptyRead("page", pageLoadTask, pageLoadStatsTime);
					break;
				}
				if (!pageLoadTask.downloadFile)
				{
					PumpNetwork();
				}
				Platform::input->RefreshMouse();
			} while (clock() < loadEndTime && !Platform::input->HasInputPending());
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop page load read done bytes=%u", (unsigned)totalBytesRead);
			}
			if (totalBytesRead && pageLoadTask.type == LoadTask::RemoteFile)
			{
				char statusMessage[64];
				if (pageLoadTask.downloadFile && pageLoadTask.IsDownloadComplete())
				{
					FinishFileDownload();
				}
				else if (pageLoadTask.IsDownloadComplete())
				{
					ui.SetStatusMessage("Loading complete", StatusBarNode::GeneralStatus);
				}
				else
				{
					FormatLoadProgress(statusMessage, sizeof(statusMessage), pageLoadTask.downloadFile ? "Downloading" : "Loading", pageLoadTask);
					ui.SetStatusMessage(statusMessage, StatusBarNode::GeneralStatus);
				}
				if (pageLoadTask.downloadFile)
				{
					MaybeLogBulkTransferStats();
				}
			}
			
		}
		else
		{
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop page load empty begin requested=%d busy=%d parserFinished=%d",
					requestedNewPage, pageLoadTask.IsBusy(), parser.IsFinished());
			}
			if (requestedNewPage)
			{
				if (pageLoadTask.type == LoadTask::RemoteFile)
				{
					if (!pageLoadTask.request)
					{
						if (Platform::network->IsConnected())
						{
							ShowErrorPage("Failed to make network request");
						}
						else
						{
							ShowErrorPage("No network interface available");
						}
						requestedNewPage = false;
					}
					else if (pageLoadTask.request->GetStatus() == HTTPRequest::Error)
					{
						ShowErrorPage(pageLoadTask.request->GetStatusString());
						requestedNewPage = false;
					}
					else if (pageLoadTask.request->GetStatus() == HTTPRequest::UnsupportedHTTPS)
					{
						ShowNoHTTPSPage();
						requestedNewPage = false;
					}
				}
				else if (pageLoadTask.type == LoadTask::LocalFile)
				{
					ui.UpdateAddressBar(pageLoadTask.GetURL());
					ShowErrorPage("File not found");
					requestedNewPage = false;
				}
			}
			else if (pageLoadTask.downloadFile)
			{
				if (!pageLoadTask.IsBusy())
				{
					FinishFileDownload();
				}
			}
			else if (!parser.IsFinished())
			{
				if (firstLoop)
				{
					MemoryDebugLog("BOOT first loop parser finish call begin");
				}
				parser.Finish();
				if (firstLoop)
				{
					MemoryDebugLog("BOOT first loop parser finish call done");
				}
			}
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop page load empty done");
			}
		}
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop page load done");
		}
		if (bulkTransferActive && !pageLoadTask.downloadFile)
		{
			SetBulkTransferActive(false);
		}
		bool pageContentHasContent = pageContentLoadTask.HasContent();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop content load begin has=%d busy=%d target=%p",
				pageContentHasContent, pageContentLoadTask.IsBusy(), loadTaskTargetNode);
		}
		if (pageContentHasContent)
		{
			clock_t contentLoadEndTime = clock() + UPDATE_TIME_SLICE;
			size_t totalBytesRead = 0;
			do
			{
				size_t bytesRead = pageContentLoadTask.GetContent(loadBuffer, APP_LOAD_BUFFER_SIZE);
				if (bytesRead)
				{
					totalBytesRead += bytesRead;
					bool stillProcessing = loadTaskTargetNode->Handler().ParseContent(loadTaskTargetNode, loadBuffer, bytesRead);
					if (!stillProcessing)
					{
						pageContentLoadTask.Stop();
					}
				}
				else
				{
					HandleEmptyRead("content", pageContentLoadTask, pageContentStatsTime, true);
					break;
				}
				PumpNetwork();
				Platform::input->RefreshMouse();
			} while (clock() < contentLoadEndTime && !Platform::input->HasInputPending());
			if (totalBytesRead && pageContentLoadTask.type == LoadTask::RemoteFile)
			{
				char statusMessage[64];
				if (pageContentLoadTask.IsDownloadComplete())
				{
					ui.SetStatusMessage("Image loaded", StatusBarNode::GeneralStatus);
					if (config.networkStats)
					{
						NetworkStatsLog("content image-loaded ticks=%ld bytes=%ld target=%p",
							(long)clock(), pageContentLoadTask.GetBytesDownloaded(), loadTaskTargetNode);
					}
				}
				else
				{
					FormatLoadProgress(statusMessage, sizeof(statusMessage), "Loading image", pageContentLoadTask);
					ui.SetStatusMessage(statusMessage, StatusBarNode::GeneralStatus);
				}
			}
		}
		else if(!pageContentLoadTask.IsBusy())
		{
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop content load idle begin target=%p", loadTaskTargetNode);
			}
			if (loadTaskTargetNode)
			{
				if (config.networkStats)
				{
					NetworkStatsLog("content finish begin ticks=%ld target=%p layoutFinished=%d",
						(long)clock(), loadTaskTargetNode, page.layout.IsFinished());
				}
				loadTaskTargetNode->Handler().FinishContent(loadTaskTargetNode, pageContentLoadTask);
				loadTaskTargetNode = page.ProcessNextLoadTask(loadTaskTargetNode, pageContentLoadTask);
				if (config.networkStats)
				{
					NetworkStatsLog("content finish done ticks=%ld next=%p layoutFinished=%d pageMemErr=%d",
						(long)clock(), loadTaskTargetNode, page.layout.IsFinished(), MemoryManager::pageAllocator.GetError());
				}
				if (!loadTaskTargetNode && page.layout.IsFinished())
				{
					if (MemoryManager::pageAllocator.GetError())
					{
						page.GetApp().ui.SetStatusMessage("Out of memory when loading page", StatusBarNode::GeneralStatus);
					}
					else
					{
						page.GetApp().ui.ClearStatusMessage(StatusBarNode::GeneralStatus);
					}
				}
			}
			if (firstLoop)
			{
				MemoryDebugLog("BOOT first loop content load idle done");
			}
		}
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop content load done");
		}
		//if (loadTask.type == LoadTask::RemoteFile && loadTask.request && loadTask.request->GetStatus() == HTTPRequest::Connecting)
		//	ui.SetStatusMessage(loadTask.request->GetStatusString());
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop layout update begin");
		}
		PumpNetwork();
		page.layout.Update();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop layout update done");
			MemoryDebugLog("BOOT first loop renderer update begin");
		}
		PumpNetwork();
		pageRenderer.Update();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop renderer update done");
			MemoryDebugLog("BOOT first loop ui update begin");
		}
		PumpNetwork();
		ui.Update();

		UpdateIdleNetwork();
		MaybeLogLoopNetStats();
		if (firstLoop)
		{
			MemoryDebugLog("BOOT first loop ui update done");
			firstLoop = false;
		}
	}
}

void LoadTask::Load(HTTPRequest::RequestType requestType, const char* targetURL, HTTPOptions* options)
{
	Stop();
	bytesDownloaded = 0;
	contentSize = 0;
	downloadComplete = false;
	url = targetURL;
	// Check for protocol substring
	if (strstr(url.url, "http://") == url.url)
	{
		type = LoadTask::RemoteFile;
	}
	else if (strstr(url.url, "file://") == url.url)
	{
		type = LoadTask::LocalFile;
		fs = fopen(url.url + 7, "rb");
	}
	else if (strstr(url.url, "https://") == url.url)
	{
		type = LoadTask::RemoteFile;
		// Bit of a hack: try forcing http:// first
		strcpy(url.url + 4, url.url + 5);
	}
	else if (strstr(url.url, "://"))
	{
		// Will be an unsupported protocol
		type = LoadTask::RemoteFile;
	}
	else
	{
		// User did not include protocol, first check for local file
		type = LoadTask::LocalFile;
		fs = fopen(targetURL, "rb");
		if (fs)
		{
			// Local file exists, prepend with file:// protocol
			strcpy(url.url, "file://");
			strcpy(url.url + 7, targetURL);
		}
		else
		{
			// did this start with X:\ ?
			if (targetURL[0] >= 'A' && targetURL[0] <= 'z' && targetURL[1] == ':' && targetURL[2] == '\\')
			{
				type = LoadTask::LocalFile;
				fs = nullptr;
			}
			else
			{
				// Assume this should be http://
				type = LoadTask::RemoteFile;
				strcpy(url.url, "http://");
				strcpy(url.url + 7, targetURL);
			}
		}
	}
	url.CleanUp();
	if (App::config.networkStats)
	{
		NetworkStatsLog("task-open %s ticks=%ld type=%d url=\"%.120s\"",
			NetworkStatsTaskName(this), (long)clock(), (int)type, url.url);
	}
	if (type == LoadTask::RemoteFile)
	{
		request = Platform::network->CreateRequest();
		if (request)
		{
			request->Open(requestType, url.url, options);
			if (App::config.dumpPage && this == &App::Get().pageLoadTask)
			{
				debugDumpFile = fopen("dump.htm", "wb");
			}
		}
	}
}

void LoadTask::Stop()
{
	if (App::config.networkStats)
	{
		NetworkStatsLog("task-stop %s ticks=%ld type=%d bytes=%ld size=%ld status=\"%s\"",
			NetworkStatsTaskName(this), (long)clock(), (int)type, GetBytesDownloaded(), GetContentSize(),
			(type == LoadTask::RemoteFile && request) ? request->GetStatusString() : "local");
	}
	if (type == LoadTask::RemoteFile && request)
	{
		bytesDownloaded = request->GetBytesDownloaded();
		contentSize = request->GetContentSize();
		downloadComplete = request->GetStatus() == HTTPRequest::Finished || (contentSize > 0 && bytesDownloaded >= contentSize);
	}
	if (debugDumpFile)
	{
		fclose(debugDumpFile);
		debugDumpFile = NULL;
	}
	if (downloadFile)
	{
		fclose(downloadFile);
		downloadFile = NULL;
	}
	switch (type)
	{
	case LoadTask::LocalFile:
		if (fs)
		{
			fclose(fs);
			fs = nullptr;
		}
		break;
	case LoadTask::RemoteFile:
		if (request)
		{
			request->Stop();
			Platform::network->DestroyRequest(request);
			request = nullptr;
		}
		break;
	}
}

const char* LoadTask::GetURL()
{
	if (type == LoadTask::RemoteFile && request)
	{
		return request->GetURL();
	}
	return url.url;
}

bool LoadTask::IsBusy()
{
	return NeedsNetworkIdle() || HasContent();
}

bool LoadTask::NeedsNetworkIdle()
{
	return type == LoadTask::RemoteFile && request && request->GetStatus() == HTTPRequest::Connecting;
}

bool LoadTask::HasContent()
{
	if (type == LoadTask::LocalFile)
	{
		return (fs && !feof(fs));
	}
	else if (type == LoadTask::RemoteFile)
	{
		return (request && request->GetStatus() == HTTPRequest::Downloading);
	}
	return false;
}

size_t LoadTask::GetContent(char* buffer, size_t count)
{
	if (type == LoadTask::LocalFile)
	{
		if (fs)
		{
			if (!feof(fs))
			{
				return fread(buffer, 1, count, fs);
			}
			fclose(fs);
		}
	}
	else if (type == LoadTask::RemoteFile)
	{
		if (request)
		{ 
			switch (request->GetStatus())
			{
			case HTTPRequest::Downloading:
			{
				size_t bytesRead = request->ReadData(buffer, count);
				bytesDownloaded = request->GetBytesDownloaded();
				contentSize = request->GetContentSize();
				downloadComplete = request->GetStatus() == HTTPRequest::Finished || (contentSize > 0 && bytesDownloaded >= contentSize);
				return bytesRead;
			}
			case HTTPRequest::Error:
				Stop();
				break;
			case HTTPRequest::Finished:
				downloadComplete = true;
				Stop();
				break;
			case HTTPRequest::Stopped:
				Stop();
				break;
			}
		}
	}
	return 0;
}

const char* LoadTask::GetContentType()
{
	if (type == LoadTask::RemoteFile)
	{
		return request->GetContentType();
	}
	else
	{
		const char* extension = url.url + strlen(url.url);
		while (extension > url.url)
		{
			extension--;
			if (*extension == '.')
			{
				extension++;
				if (!stricmp(extension, "gif"))
				{
					return "image/gif";
				}
				else if (!stricmp(extension, "png"))
				{
					return "image/png";
				}
				else if (!stricmp(extension, "jpeg") || !stricmp(extension, "jpg"))
				{
					return "image/jpeg";
				}
				else if (!stricmp(extension, "htm") || !stricmp(extension, "html"))
				{
					return "text/html";
				}
				return "text/plain";
			}
		}
		return "text/html";
	}
}

long LoadTask::GetContentSize()
{
	if (type == LoadTask::RemoteFile && request)
	{
		return request->GetContentSize();
	}
	return contentSize;
}

long LoadTask::GetBytesDownloaded()
{
	if (type == LoadTask::RemoteFile && request)
	{
		return request->GetBytesDownloaded();
	}
	return bytesDownloaded;
}

bool LoadTask::IsDownloadComplete()
{
	if (type == LoadTask::RemoteFile && request)
	{
		long requestContentSize = request->GetContentSize();
		long requestBytesDownloaded = request->GetBytesDownloaded();
		return request->GetStatus() == HTTPRequest::Finished || (requestContentSize > 0 && requestBytesDownloaded >= requestContentSize);
	}
	return downloadComplete;
}

void App::RequestNewPage(HTTPRequest::RequestType requestType, const char* url, HTTPOptions* options)
{
	if (!url || !*url)
		return;
	StopLoad();
	pageLoadTask.Load(requestType, url, options);
	requestedNewPage = true;
	loadTaskTargetNode = nullptr;
	//ui.UpdateAddressBar(loadTask.url);
	if (pageLoadTask.type == LoadTask::RemoteFile && pageLoadTask.request)
	{
		ui.SetStatusMessage("Connecting to server...", StatusBarNode::GeneralStatus);
	}
}

void App::OpenURL(HTTPRequest::RequestType requestType, const char* url, HTTPOptions* options)
{
	RequestNewPage(requestType, url, options);
	size_t urlStringLength = strlen(url) + 1;
	if (*pageHistoryPtr)
	{
		pageHistoryPtr += strlen(pageHistoryPtr) + 1;
	}
	// If not enough space in the buffer then move to previous space
	while (pageHistoryPtr + urlStringLength > pageHistoryBuffer + MAX_PAGE_HISTORY_BUFFER_SIZE)
	{
		size_t firstLength = strlen(pageHistoryBuffer);
		if (!firstLength)
		{
			// This shouldn't happen
			pageHistoryPtr = pageHistoryBuffer;
			break;
		}
		memmove(pageHistoryBuffer, pageHistoryBuffer + firstLength + 1, MAX_PAGE_HISTORY_BUFFER_SIZE - firstLength - 1);
		pageHistoryPtr -= firstLength + 1;
	}
	strcpy(pageHistoryPtr, url);
	memset(pageHistoryPtr + urlStringLength, 0, MAX_PAGE_HISTORY_BUFFER_SIZE - (pageHistoryPtr + urlStringLength - pageHistoryBuffer));
}

void App::StopLoad()
{
	SetBulkTransferActive(false);
	pageLoadTask.Stop();
	pageContentLoadTask.Stop();
}

void App::FinishFileDownload()
{
	if (!pageLoadTask.downloadFile)
	{
		return;
	}

	char downloadResult[96];
	long downloadedBytes = pageLoadTask.GetBytesDownloaded();
	clock_t now = clock();
	clock_t startTicks = bulkTransferFirstByteTime ? bulkTransferFirstByteTime : bulkTransferStartTime;
	clock_t downloadTicks = startTicks ? (now - startTicks) : 0;
	FormatDownloadResult(downloadResult, sizeof(downloadResult), downloadedBytes, downloadTicks);
	if (config.networkStats)
	{
		long elapsedMs = ((long)downloadTicks * 1000L) / CLOCKS_PER_SEC;
		NetworkStatsLog("complete bytes=%ld ticks=%ld ms=%ld", downloadedBytes, (long)downloadTicks, elapsedMs);
	}

	SetBulkTransferActive(false);
	FILE* completedFile = pageLoadTask.downloadFile;
	pageLoadTask.downloadFile = NULL;
	fclose(completedFile);
	pageLoadTask.Stop();
	ShowDownloadEndedPage(downloadResult);
}

void App::SetBulkTransferActive(bool active)
{
	if (bulkTransferActive == active)
	{
		return;
	}
	bulkTransferActive = active;
	if (active)
	{
		bulkTransferStartTime = clock();
		bulkTransferFirstByteTime = 0;
		bulkTransferLastStatsTime = bulkTransferStartTime;
		bulkTransferLastStatsBytes = pageLoadTask.GetBytesDownloaded();
		bulkTransferReadCalls = 0;
		bulkTransferZeroReads = 0;
		bulkTransferPumpCalls = 0;
		if (config.networkStats)
		{
			NetworkStatsLog("begin bytes=%ld bulkpump=%lu", bulkTransferLastStatsBytes, config.bulkPumpBytes);
		}
	}
	else
	{
		MaybeLogBulkTransferStats(true);
		if (config.networkStats)
		{
			NetworkStatsLog("end bytes=%ld", pageLoadTask.GetBytesDownloaded());
		}
	}
	if (Platform::network)
	{
		Platform::network->SetBulkTransferMode(active);
	}
}

void App::PumpBulkTransfer()
{
	if (Platform::network)
	{
		Platform::network->Update();
		bulkTransferPumpCalls++;
	}
}

void App::LogZeroReadNetStats(const char* name, LoadTask& task, clock_t& lastLogTime, bool includeTarget)
{
	if (!config.networkStats)
	{
		return;
	}

	clock_t now = clock();
	if ((now - lastLogTime) < (CLOCKS_PER_SEC / 2))
	{
		return;
	}

	const char* status = "local";
	if (task.type == LoadTask::RemoteFile && task.request)
	{
		status = task.request->GetStatusString();
	}

	if (includeTarget)
	{
		NetworkStatsLog("read-zero task=%s ticks=%ld bytes=%ld status=\"%s\" target=%p",
			name, (long)now, task.GetBytesDownloaded(), status, loadTaskTargetNode);
	}
	else
	{
		NetworkStatsLog("read-zero task=%s ticks=%ld bytes=%ld status=\"%s\"",
			name, (long)now, task.GetBytesDownloaded(), status);
	}
	lastLogTime = now;
}

void App::HandleEmptyRead(const char* name, LoadTask& task, clock_t& lastLogTime, bool includeTarget)
{
	if (!task.HasContent() || task.IsDownloadComplete())
	{
		return;
	}

	if (task.downloadFile)
	{
		bulkTransferZeroReads++;
		return;
	}

	if (Platform::network)
	{
		Platform::network->UpdateIdle();
	}
	LogZeroReadNetStats(name, task, lastLogTime, includeTarget);
}

void App::UpdateIdleNetwork()
{
	if (config.legacyNetworkPump || !Platform::network)
	{
		return;
	}

	if ((pageLoadTask.NeedsNetworkIdle() || pageContentLoadTask.NeedsNetworkIdle()) && !pageRenderer.IsRendering())
	{
		Platform::network->UpdateIdle();
		return;
	}

	if (!pageLoadTask.IsBusy() && !pageContentLoadTask.IsBusy()
		&& parser.IsFinished() && page.layout.IsFinished() && !pageRenderer.IsRendering())
	{
		Platform::network->UpdateIdle();
	}
}

void App::MaybeLogBulkTransferStats(bool force)
{
	if (!config.networkStats)
	{
		return;
	}

	clock_t now = clock();
	clock_t sampleTicks = now - bulkTransferLastStatsTime;
	if (!force && sampleTicks < (CLOCKS_PER_SEC / 2))
	{
		return;
	}

	long totalBytes = pageLoadTask.GetBytesDownloaded();
	long sampleBytes = totalBytes - bulkTransferLastStatsBytes;
	long elapsedMs = 0;
	long sampleMs = 0;
	if (CLOCKS_PER_SEC)
	{
		elapsedMs = ((long)(now - bulkTransferStartTime) * 1000L) / CLOCKS_PER_SEC;
		sampleMs = ((long)sampleTicks * 1000L) / CLOCKS_PER_SEC;
	}
	NetworkStatsLog("download ticks=%ld ms=%ld sampleTicks=%ld sampleMs=%ld bytes=%ld sample=%ld reads=%lu zero=%lu pumps=%lu bulkpump=%lu",
		(long)(now - bulkTransferStartTime), elapsedMs, (long)sampleTicks, sampleMs, totalBytes, sampleBytes,
		bulkTransferReadCalls, bulkTransferZeroReads, bulkTransferPumpCalls, config.bulkPumpBytes);

	bulkTransferLastStatsTime = now;
	bulkTransferLastStatsBytes = totalBytes;
	bulkTransferReadCalls = 0;
	bulkTransferZeroReads = 0;
	bulkTransferPumpCalls = 0;
}

void App::LogLoadTaskNetStats(const char* name, LoadTask& task, bool force)
{
	if (!config.networkStats)
	{
		return;
	}

	int* lastStatus = &pageLoadStatsStatus;
	char* lastText = pageLoadStatsText;
	long* lastBytes = &pageLoadStatsBytes;
	clock_t* lastTime = &pageLoadStatsTime;
	if (&task == &pageContentLoadTask)
	{
		lastStatus = &pageContentStatsStatus;
		lastText = pageContentStatsText;
		lastBytes = &pageContentStatsBytes;
		lastTime = &pageContentStatsTime;
	}

	int status = -1;
	const char* statusText = "local";
	const char* url = task.GetURL();
	if (task.type == LoadTask::RemoteFile)
	{
		if (task.request)
		{
			status = (int)task.request->GetStatus();
			statusText = task.request->GetStatusString();
			url = task.request->GetURL();
		}
		else
		{
			statusText = "no-request";
		}
	}
	else if (!task.fs)
	{
		statusText = "idle";
	}

	long bytes = task.GetBytesDownloaded();
	clock_t now = clock();
	bool changed = status != *lastStatus || strcmp(statusText, lastText);
	bool progressed = bytes != *lastBytes;
	if (!force && !changed && (!progressed || (now - *lastTime) < (CLOCKS_PER_SEC / 2)))
	{
		return;
	}

	NetworkStatsLog("task %s ticks=%ld status=%d text=\"%s\" bytes=%ld size=%ld has=%d busy=%d done=%d requested=%d target=%p url=\"%.96s\"",
		name, (long)now, status, statusText, bytes, task.GetContentSize(), task.HasContent(), task.IsBusy(),
		task.IsDownloadComplete(), requestedNewPage, loadTaskTargetNode, url ? url : "");

	*lastStatus = status;
	strncpy(lastText, statusText, 47);
	lastText[47] = '\0';
	*lastBytes = bytes;
	*lastTime = now;
}

void App::MaybeLogLoopNetStats(bool force)
{
	if (!config.networkStats)
	{
		return;
	}

	clock_t now = clock();
	if (!force && (now - loopStatsTime) < (CLOCKS_PER_SEC / 2))
	{
		return;
	}
	loopStatsTime = now;
	NetworkStatsLog("loop ticks=%ld pageBusy=%d pageHas=%d contentBusy=%d contentHas=%d requested=%d parserFinished=%d layoutFinished=%d rendering=%d target=%p bulk=%d",
		(long)now, pageLoadTask.IsBusy(), pageLoadTask.HasContent(), pageContentLoadTask.IsBusy(),
		pageContentLoadTask.HasContent(), requestedNewPage, parser.IsFinished(), page.layout.IsFinished(),
		pageRenderer.IsRendering(), loadTaskTargetNode, bulkTransferActive);
}

void App::ShowErrorPage(const char* message)
{
	StopLoad();
	ResetPage();
	page.SetTitle("Error");
	page.pageURL = pageLoadTask.GetURL();
	ui.UpdateAddressBar(page.pageURL);
	parser.Write("<html>");
	parser.Write("<h1>Error loading page</h1>");
	parser.Write("<hr>");
	parser.Write(message);
	parser.Write("</html>");
	parser.Finish();
}

static const char* frogFindURL = "http://frogfind.com/read.php?a=";
#define FROG_FIND_URL_LENGTH 31

void App::ShowNoHTTPSPage()
{
	ResetPage();
	page.SetTitle("HTTPS unsupported");
	page.pageURL = pageLoadTask.GetURL();
	ui.UpdateAddressBar(page.pageURL);
	StopLoad();
	page.pageURL = frogFindURL;
	strncpy(page.pageURL.url + FROG_FIND_URL_LENGTH, pageLoadTask.GetURL(), MAX_URL_LENGTH - FROG_FIND_URL_LENGTH);
	parser.Write("<html>");
	parser.Write("<h1>HTTPS unsupported</h1>");
	parser.Write("<hr>");
	parser.Write("Sorry this browser does not support HTTPS!<br>");
	parser.Write("<a href=\"");
	parser.Write(page.pageURL.url);
	parser.Write("\">Visit this site via FrogFind</a>");
	parser.Write("</html>");
	parser.Finish();
}

void App::PreviousPage()
{
	if (pageHistoryPtr > pageHistoryBuffer)
	{
		do
		{
			pageHistoryPtr--;
		} while (pageHistoryPtr > pageHistoryBuffer && pageHistoryPtr[-1]);
		RequestNewPage(HTTPRequest::Get, pageHistoryPtr);
	}
}

void App::NextPage()
{
	if (*pageHistoryPtr)
	{
		char* next = pageHistoryPtr + strlen(pageHistoryPtr) + 1;
		if (next < pageHistoryBuffer + MAX_PAGE_HISTORY_BUFFER_SIZE && *next)
		{
			pageHistoryPtr = next;
			RequestNewPage(HTTPRequest::Get, pageHistoryPtr);
		}
	}
}

void App::ReloadPage()
{
	if (*pageHistoryPtr)
	{
		RequestNewPage(HTTPRequest::Get, pageHistoryPtr);
	}
}

void App::LoadImageNodeContent(Node* node)
{
	loadTaskTargetNode = node;
	node->Handler().LoadContent(node, pageContentLoadTask);
}

void VideoDriver::InvertVideoOutput()
{
	if (drawSurface->format == DrawSurface::Format_1BPP)
	{
		App::config.invertScreen = !App::config.invertScreen;
		DrawContext context(drawSurface, 0, 0, screenWidth, screenHeight);
		Platform::input->HideMouse();
		context.surface->InvertRect(context, 0, 0, screenWidth, screenHeight);
		Platform::input->ShowMouse();
	}
}

void App::ShowDownloadProgressPage(const char* savePath)
{
	ResetPage();
	parser.Write("<html><body><center>");
	parser.Write("<h1>Downloading</h1>");
	parser.Write("<hr>");
	parser.Write(page.pageURL.url);
	parser.Write("<br> to ");
	parser.Write(savePath);
	parser.Write("<hr>");
	parser.Write("<form action=\"cancel://\">");
	parser.Write("<input type=\"button\" value=\"Cancel\"/>");
	parser.Write("</form>");
	parser.Write("</center></body></html>");
	parser.Finish();
}

void App::ShowDownloadEndedPage(const char* message)
{
	ResetPage();
	parser.Write("<html><body><center><h1>");
	parser.Write(message);
	parser.Write("</h1></center></body></html>");
	parser.Finish();
}

void App::ShowDownloadDialogPage()
{
	char temp[32];
	char filename[14];
	parser.Write("<html><body><center>");
	parser.Write("<h1>Do you want to download this file?</h1>");
	parser.Write("<hr>");
	parser.Write(pageLoadTask.url.url);
	parser.Write("<br><b>Content type: ");
	parser.Write(pageLoadTask.GetContentType());
	long contentSize = pageLoadTask.request->GetContentSize();
	if (contentSize != 0)
	{
		parser.Write("<br>Size: ");
			
		if (contentSize < 1000)
		{
			snprintf(temp, 32, "%d bytes", contentSize);
		}
		else if (contentSize < 1000000l)
		{
			snprintf(temp, 32, "%d KB", contentSize / 1000);
		}
		else
		{
			snprintf(temp, 32, "%d MB", contentSize / 1000000l);
		}
		parser.Write(temp);
	}
	const char* srcFilename = pageLoadTask.url.url;
	const char* ptr;
	while (ptr = strchr(srcFilename, '/'))
	{
		srcFilename = ptr + 1;
	}
	int filenameLength = 0;
	for (int n = 0; n < 8; n++)
	{
		if (srcFilename[n] == '\0' || srcFilename[n] == '.')
		{
			break;
		}
		filename[n] = srcFilename[n];
		filenameLength++;
	}
	if (ptr = strchr(srcFilename, '.'))
	{
		for (int n = 0; n < 4; n++)
		{
			filename[filenameLength++] = ptr[n];
		}
	}
	filename[filenameLength] = '\0';
	parser.Write("</b><hr>");
	parser.Write("<form action=\"download://\">");
	parser.Write("<input name=\"path\" type=\"text\" width=\"75%\" value=\"");
	char currentDirectory[PATH_MAX + 1];
	if (getcwd(currentDirectory, (PATH_MAX + 1)) == NULL)
	{
		parser.Write("Can't get current path: ");
		parser.Write(strerror(errno));
	}
	else
	{
		parser.Write(currentDirectory);
		// Non-root folders has no \ at the end - let's add it!
		if (currentDirectory[strlen(currentDirectory) - 1] != '\\')
		{
			parser.Write("\\");
		}
		parser.Write(filename);
	}
	parser.Write("\"/><br>");
	parser.Write("<br>");
	parser.Write("<input type=\"button\" value=\"Download\"/>");
	parser.Write("</form>");
	parser.Write("</center></body></html>");
	parser.Finish();
}

void App::BeginFileDownload(const char* savePath)
{
	FILE* downloadFile = fopen(savePath, "wb");
	if (downloadFile)
	{
		ShowDownloadProgressPage(savePath);
		pageLoadTask.Load(HTTPRequest::Get, pageLoadTask.url.url);
		requestedNewPage = true;
		pageLoadTask.downloadFile = downloadFile;
		SetBulkTransferActive(true);
		ui.SetStatusMessage("Connecting to server...", StatusBarNode::GeneralStatus);
	}
}

void App::CancelFileDownload()
{
	if (pageLoadTask.downloadFile)
	{
		StopLoad();
		ShowDownloadEndedPage("Download cancelled");
	}
}
