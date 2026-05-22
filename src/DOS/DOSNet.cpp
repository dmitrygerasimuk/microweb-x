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

#include "DOSNet.h"
#include "../HTTP.h"
#include "../Memory/MemoryLog.h"

#include <bios.h>
#include <io.h>
#include <fcntl.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "types.h"

#include "timer.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "udp.h"
#include "dns.h"

// Ctrl-Break and Ctrl-C handler.  Check the flag once in a while to see if
// the user wants out.
volatile uint8_t CtrlBreakDetected = 0;
void __interrupt __far ctrlBreakHandler() {
	CtrlBreakDetected = 1;
}

DOSNetworkDriver::DOSNetworkDriver()
{
	isConnected = false;
	legacyPump = false;
	bulkTransferMode = false;
	conservativeDns = false;
	for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
	{
		requests[n] = NULL;
		sockets[n] = NULL;
	}
}

void DOSNetworkDriver::Init()
{
	isConnected = false;

	//printf("Init network driver\n");
	MemoryDebugLog("BOOT net parseEnv begin");
	int parseResult = Utils::parseEnv();
	MemoryDebugLog("BOOT net parseEnv done result=%d", parseResult);
	if (parseResult != 0) {
		printf("Failed in parseEnv()\n");
		return;
	}

	//printf("Init network stack\n");
	MemoryDebugLog("BOOT net initStack begin req=%d ring=%d", MAX_CONCURRENT_HTTP_REQUESTS, TCP_SOCKET_RING_SIZE);
	int initResult = Utils::initStack(MAX_CONCURRENT_HTTP_REQUESTS, TCP_SOCKET_RING_SIZE, ctrlBreakHandler, ctrlBreakHandler);
	MemoryDebugLog("BOOT net initStack done result=%d", initResult);
	if (initResult) {
		printf("Failed to initialize TCP/IP\n");
		return;
	}
#ifdef SLEEP_CALLS
	mTCP_releaseTimesliceEnabled = 0;
	MemoryDebugLog("BOOT net sleep=%d releaseTimeslice disabled legacy=%d", mTCP_sleepCallEnabled, legacyPump);
#endif
	Dns::setInitialSendSpin(conservativeDns ? 0 : 1);
	MemoryDebugLog("BOOT net dns conservative=%d", conservativeDns);

	for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
	{
		requests[n] = new HTTPRequest();
		MemoryDebugLog("BOOT net request alloc begin index=%d", n);
		if (!requests[n])
		{
			Platform::FatalError("Could not allocate memory for HTTP request");
		}

		sockets[n] = new DOSTCPSocket();
		MemoryDebugLog("BOOT net socket alloc begin index=%d", n);
		if (!sockets[n])
		{
			Platform::FatalError("Could not allocate memory for TCP socket");
		}
	}

	printf("Network interface initialised\n");
	isConnected = true;
	bulkTransferMode = false;
	MemoryDebugLog("BOOT net init complete");
}

void DOSNetworkDriver::Shutdown()
{
	MemoryDebugLog("BOOT net driver shutdown begin connected=%d", isConnected);
	for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
	{
		if (requests[n])
		{
			MemoryDebugLog("BOOT net request stop index=%d", n);
			requests[n]->Stop();
		}
		if (sockets[n] && sockets[n]->GetSock())
		{
			MemoryDebugLog("BOOT net socket close index=%d", n);
			sockets[n]->Close();
		}
	}

	if (isConnected)
	{
		MemoryDebugLog("BOOT net endStack begin");
		Utils::endStack();
		MemoryDebugLog("BOOT net endStack done");
		isConnected = false;
	}
	MemoryDebugLog("BOOT net driver shutdown done");
}

void DOSNetworkDriver::Update()
{
	if (isConnected)
	{
		if (legacyPump || bulkTransferMode)
		{
			DriveLegacyNetwork();
		}
		else
		{
			DriveNetwork(false);
		}
	}
}

void DOSNetworkDriver::UpdateIdle()
{
	if (isConnected)
	{
		if (legacyPump || bulkTransferMode)
		{
			DriveLegacyNetwork();
		}
		else
		{
			DriveNetwork(true);
		}
	}
}

void DOSNetworkDriver::DriveLegacyNetwork()
{
	PACKET_PROCESS_MULT(5);
	Arp::driveArp();
	Tcp::drivePackets();
	Dns::drivePendingQuery();
	DriveRequests();
}

void DOSNetworkDriver::DriveNetwork(bool allowSleep)
{
	ProcessPackets(allowSleep);
	Arp::driveArp();
	Tcp::drivePackets();
	Dns::drivePendingQuery();
	DriveRequests();
}

void DOSNetworkDriver::DriveRequests()
{
	for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
	{
		requests[n]->Update();
	}
}

void DOSNetworkDriver::ProcessPackets(bool allowSleep)
{
	if (allowSleep)
	{
		PACKET_PROCESS_MULT(5);
	}
	else
	{
		uint8_t i = 0;
		while (i < 5)
		{
			if (Buffer_first != Buffer_next)
			{
				Packet_process_internal();
			}
			else
			{
				break;
			}
			i++;
		}
		IP_FRAGS_CHECK_OVERDUE();
	}
}

HTTPRequest* DOSNetworkDriver::CreateRequest()
{
	if (isConnected)
	{
		for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
		{
			if (requests[n]->GetStatus() == HTTPRequest::Stopped)
			{
				return requests[n];
			}
		}
	}

	return NULL;
}

void DOSNetworkDriver::DestroyRequest(HTTPRequest* request)
{
	if (request)
	{
		request->Stop();
	}
}

// Returns zero on success, negative number is error
int DOSNetworkDriver::ResolveAddress(const char* name, NetworkAddress address, bool sendRequest)
{
	return Dns::resolve(name, address, sendRequest ? 1 : 0);
}

NetworkTCPSocket* DOSNetworkDriver::CreateSocket()
{
	for (int n = 0; n < MAX_CONCURRENT_HTTP_REQUESTS; n++)
	{
		if (sockets[n]->GetSock() == NULL)
		{
			TcpSocket* sock = TcpSocketMgr::getSocket();
			if (sock)
			{
				sockets[n]->SetSock(sock);
				return sockets[n];
			}
			return NULL;
		}
	}

	return NULL;
}

void DOSNetworkDriver::DestroySocket(NetworkTCPSocket* socket)
{
	socket->Close();
}

DOSTCPSocket::DOSTCPSocket()
{
	sock = NULL;
}

void DOSTCPSocket::SetSock(TcpSocket* inSock)
{
	sock = inSock;
	if (sock)
	{
		sock->rcvBuffer = recvBuffer;
		sock->rcvBufSize = TCP_RECV_BUFFER_SIZE;
	}
}

int DOSTCPSocket::Send(uint8_t* data, int length)
{
	if (!sock)
	{
		return -1;
	}
	return sock->send(data, length);
}

int DOSTCPSocket::Receive(uint8_t* buffer, int length)
{
	if (!sock)
	{
		return -1;
	}
	return sock->recv(buffer, length);
}

int DOSTCPSocket::Connect(NetworkAddress address, int port)
{
	uint16_t localport = 2048 + rand();
	return sock->connectNonBlocking(localport, address, port);
}

bool DOSTCPSocket::IsConnectComplete()
{
	return sock && sock->isConnectComplete();
}

bool DOSTCPSocket::IsClosed() 
{
	return !sock || sock->isClosed();
}

void DOSTCPSocket::Close()
{
	if (sock)
	{
		TcpSocket* oldSock = sock;
		sock = NULL;
		oldSock->closeNonblocking();
		TcpSocketMgr::freeSocket(oldSock);
	}
}
