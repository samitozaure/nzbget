/*
 *  This file if part of nzbget
 *
 *  Copyright (C) 2005  Bo Cordes Petersen <placebodk@sourceforge.net>
 *  Copyright (C) 2007  Andrei Prygounkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Revision$
 * $Date$
 *
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#include "win32.h"
#endif

#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "nzbget.h"
#include "RemoteServer.h"
#include "Log.h"
#include "Options.h"
#include "QueueCoordinator.h"
#include "Util.h"

extern Options* g_pOptions;
extern QueueCoordinator* g_pQueueCoordinator;
extern void ExitProc();

const char* g_szMessageRequests[] =
    { "N/A", "Download", "Pause/Unpause", "List",
      "Set download rate", "Dump debug", "Edit queue", "Log", "Quit"
    };

//*****************************************************************
// RemoteServer

RemoteServer::RemoteServer()
{
	debug("Creating RemoteServer");

	m_pNetAddress = new NetAddress(g_pOptions->GetServerIP(), g_pOptions->GetServerPort());
	m_pConnection = new Connection(m_pNetAddress);
	m_pConnection->SetTimeout(g_pOptions->GetConnectionTimeout());
}

RemoteServer::~RemoteServer()
{
	debug("Destroying RemoteServer");

	delete m_pNetAddress;
	delete m_pConnection;
}

void RemoteServer::Run()
{
	debug("Entering RemoteServer-loop");

	m_pConnection->Bind();

	while (!IsStopped())
	{
		// Accept connections and store the "new" socket value
		SOCKET iSocket = m_pConnection->Accept();
		if (iSocket == INVALID_SOCKET)
		{
			if (IsStopped())
			{
				break; 
			}
			// error binding on port. wait 0.5 sec. and retry
			usleep(500 * 1000);
			delete m_pConnection;
			m_pConnection = new Connection(m_pNetAddress);
			m_pConnection->SetTimeout(g_pOptions->GetConnectionTimeout());
			m_pConnection->Bind();
			continue;
		}

		MessageCommand* commandThread = new MessageCommand();
		commandThread->SetAutoDestroy(true);
		commandThread->SetSocket(iSocket);
		commandThread->Start();
	}
	m_pConnection->Disconnect();

	debug("Exiting RemoteServer-loop");
}

void RemoteServer::Stop()
{
	Thread::Stop();
	m_pConnection->Cancel();
#ifdef WIN32
	m_pConnection->Disconnect();
#endif
}

//*****************************************************************
// MessageCommand

void MessageCommand::SendResponse(char* szAnswer)
{
	send(m_iSocket, szAnswer, strlen(szAnswer), 0);
}

void MessageCommand::ProcessRequest()
{
	SNZBMessageBase* pMessageBase = (SNZBMessageBase*) & m_RequestBuffer;

	switch (pMessageBase->m_iType)
	{
		case NZBMessageRequest::eRequestDownload:
			{
				RequestDownload();
				break;
			}

		case NZBMessageRequest::eRequestList:
			{
				RequestList();
				break;
			}

		case NZBMessageRequest::eRequestLog:
			{
				RequestLog();
				break;
			}

		case NZBMessageRequest::eRequestPauseUnpause:
			{
				SNZBPauseUnpauseRequest* pPauseUnpauseRequest = (SNZBPauseUnpauseRequest*) & m_RequestBuffer;
				g_pOptions->SetPause(pPauseUnpauseRequest->m_bPause);
				SendResponse("Pause-/Unpause-Command completed successfully");
				break;
			}

		case NZBMessageRequest::eRequestEditQueue:
			{
				RequestEditQueue();
				break;
			}

		case NZBMessageRequest::eRequestSetDownloadRate:
			{
				SNZBSetDownloadRateRequest* pSetDownloadRequest = (SNZBSetDownloadRateRequest*) & m_RequestBuffer;
				g_pOptions->SetDownloadRate(pSetDownloadRequest->m_fDownloadRate);
				SendResponse("Rate-Command completed successfully");
				break;
			}

		case NZBMessageRequest::eRequestDumpDebug:
			{
				g_pQueueCoordinator->LogDebugInfo();
				SendResponse("Debug-Command completed successfully");
				break;
			}

		case NZBMessageRequest::eRequestShutdown:
			{
				SendResponse("Stopping server");
				ExitProc();
				break;
			}

		default:
			error("NZB-Request not yet supported");
			break;
	}
}

void MessageCommand::RequestDownload()
{
	SNZBDownloadRequest* pDownloadRequest = (SNZBDownloadRequest*) & m_RequestBuffer;
	const char* pExtraData = (m_iExtraDataLength > 0) ? ((char*)pDownloadRequest + pDownloadRequest->m_MessageBase.m_iSize) : NULL;
	int NeedBytes = pDownloadRequest->m_iTrailingDataLength - m_iExtraDataLength;
	char* pRecvBuffer = (char*)malloc(pDownloadRequest->m_iTrailingDataLength + 1);
	memcpy(pRecvBuffer, pExtraData, m_iExtraDataLength);
	char* pBufPtr = pRecvBuffer + m_iExtraDataLength;

	// Read from the socket until nothing remains
	int iResult = 0;
	while (NeedBytes > 0)
	{
		iResult = recv(m_iSocket, pBufPtr, NeedBytes, 0);
		// Did the recv succeed?
		if (iResult <= 0)
		{
			error("invalid request");
			break;
		}
		pBufPtr += iResult;
		NeedBytes -= iResult;
	}

	if (NeedBytes == 0)
	{
		NZBFile* pNZBFile = NZBFile::CreateFromBuffer(pDownloadRequest->m_szFilename, pRecvBuffer, pDownloadRequest->m_iTrailingDataLength);

		if (pNZBFile)
		{
			info("Request: Queue collection %s", pDownloadRequest->m_szFilename);
			g_pQueueCoordinator->AddNZBFileToQueue(pNZBFile, pDownloadRequest->m_bAddFirst);
			delete pNZBFile;

			char tmp[1024];
			snprintf(tmp, 1024, "Collection %s added to queue", BaseFileName(pDownloadRequest->m_szFilename));
			tmp[1024-1] = '\0';
			SendResponse(tmp);
		}
		else
		{
			char tmp[1024];
			snprintf(tmp, 1024, "Download Request failed for %s", BaseFileName(pDownloadRequest->m_szFilename));
			tmp[1024-1] = '\0';
			SendResponse(tmp);
		}
	}

	free(pRecvBuffer);
}

void MessageCommand::RequestList()
{
	SNZBListRequest* pListRequest = (SNZBListRequest*) & m_RequestBuffer;

	SNZBListRequestAnswer ListRequestAnswer;
	memset(&ListRequestAnswer, 0, sizeof(ListRequestAnswer));
	ListRequestAnswer.m_iSize = sizeof(ListRequestAnswer);
	ListRequestAnswer.m_iEntrySize = sizeof(SNZBListRequestAnswerEntry);

	char* buf = NULL;
	int bufsize = 0;

	if (pListRequest->m_bFileList)
	{
		// Make a data structure and copy all the elements of the list into it
		DownloadQueue* pDownloadQueue = g_pQueueCoordinator->LockQueue();

		int NrEntries = pDownloadQueue->size();

		// calculate required buffer size
		bufsize = NrEntries * sizeof(SNZBListRequestAnswerEntry);
		for (DownloadQueue::iterator it = pDownloadQueue->begin(); it != pDownloadQueue->end(); it++)
		{
			FileInfo* pFileInfo = *it;
			bufsize += strlen(pFileInfo->GetNZBFilename()) + 1;
			bufsize += strlen(pFileInfo->GetSubject()) + 1;
			bufsize += strlen(pFileInfo->GetFilename()) + 1;
			bufsize += strlen(pFileInfo->GetDestDir()) + 1;
		}

		buf = (char*) malloc(bufsize);
		char* bufptr = buf;
		for (DownloadQueue::iterator it = pDownloadQueue->begin(); it != pDownloadQueue->end(); it++)
		{
			FileInfo* pFileInfo = *it;
			SNZBListRequestAnswerEntry* pListAnswer = (SNZBListRequestAnswerEntry*) bufptr;
			pListAnswer->m_iID				= pFileInfo->GetID();
			pListAnswer->m_iFileSize		= (int)pFileInfo->GetSize();
			pListAnswer->m_bFilenameConfirmed = pFileInfo->GetFilenameConfirmed();
			pListAnswer->m_iRemainingSize	= (int)pFileInfo->GetRemainingSize();
			pListAnswer->m_bPaused			= pFileInfo->GetPaused();
			pListAnswer->m_iNZBFilenameLen	= strlen(pFileInfo->GetNZBFilename()) + 1;
			pListAnswer->m_iSubjectLen		= strlen(pFileInfo->GetSubject()) + 1;
			pListAnswer->m_iFilenameLen		= strlen(pFileInfo->GetFilename()) + 1;
			pListAnswer->m_iDestDirLen		= strlen(pFileInfo->GetDestDir()) + 1;
			bufptr += sizeof(SNZBListRequestAnswerEntry);
			strcpy(bufptr, pFileInfo->GetNZBFilename());
			bufptr += pListAnswer->m_iNZBFilenameLen;
			strcpy(bufptr, pFileInfo->GetSubject());
			bufptr += pListAnswer->m_iSubjectLen;
			strcpy(bufptr, pFileInfo->GetFilename());
			bufptr += pListAnswer->m_iFilenameLen;
			strcpy(bufptr, pFileInfo->GetDestDir());
			bufptr += pListAnswer->m_iDestDirLen;
		}

		g_pQueueCoordinator->UnlockQueue();

		ListRequestAnswer.m_iNrTrailingEntries = NrEntries;
		ListRequestAnswer.m_iTrailingDataLength = bufsize;
	}

	if (pListRequest->m_bServerState)
	{
		ListRequestAnswer.m_fDownloadRate = g_pQueueCoordinator->CalcCurrentDownloadSpeed();
		ListRequestAnswer.m_lRemainingSize = g_pQueueCoordinator->CalcRemainingSize();
		ListRequestAnswer.m_fDownloadLimit = g_pOptions->GetDownloadRate();
		ListRequestAnswer.m_bServerPaused = g_pOptions->GetPause();
		ListRequestAnswer.m_iThreadCount = Thread::GetThreadCount() - 1; // not counting itself
	}

	// Send the request answer
	send(m_iSocket, (char*) &ListRequestAnswer, sizeof(ListRequestAnswer), 0);

	// Send the data
	if (bufsize > 0)
	{
		send(m_iSocket, buf, bufsize, 0);
	}

	if (buf)
	{
		free(buf);
	}
}

void MessageCommand::RequestLog()
{
	SNZBLogRequest* pLogRequest = (SNZBLogRequest*) & m_RequestBuffer;

	Log::Messages* pMessages = g_pLog->LockMessages();

	int iNrEntries = pLogRequest->m_iLines;
	unsigned int iIDFrom = pLogRequest->m_iIDFrom;
	int iStart = pMessages->size();
	if (iNrEntries > 0)
	{
		if (iNrEntries > (int)pMessages->size())
		{
			iNrEntries = pMessages->size();
		}
		iStart = pMessages->size() - iNrEntries;
	}
	if (iIDFrom > 0 && !pMessages->empty())
	{
		iStart = iIDFrom - pMessages->front()->GetID();
		if (iStart < 0)
		{
			iStart = 0;
		}
		iNrEntries = pMessages->size() - iStart;
		if (iNrEntries < 0)
		{
			iNrEntries = 0;
		}
	}

	// calculate required buffer size
	int bufsize = iNrEntries * sizeof(SNZBLogRequestAnswerEntry);
	for (unsigned int i = (unsigned int)iStart; i < pMessages->size(); i++)
	{
		Message* pMessage = (*pMessages)[i];
		bufsize += strlen(pMessage->GetText()) + 1;
	}

	char* buf = (char*) malloc(bufsize);
	char* bufptr = buf;
	for (unsigned int i = (unsigned int)iStart; i < pMessages->size(); i++)
	{
		Message* pMessage = (*pMessages)[i];
		SNZBLogRequestAnswerEntry* pLogAnswer = (SNZBLogRequestAnswerEntry*) bufptr;
		pLogAnswer->m_iID = pMessage->GetID();
		pLogAnswer->m_iKind = pMessage->GetKind();
		pLogAnswer->m_tTime = pMessage->GetTime();
		pLogAnswer->m_iTextLen = strlen(pMessage->GetText()) + 1;
		bufptr += sizeof(SNZBLogRequestAnswerEntry);
		strcpy(bufptr, pMessage->GetText());
		bufptr += pLogAnswer->m_iTextLen;
	}

	g_pLog->UnlockMessages();

	SNZBLogRequestAnswer LogRequestAnswer;
	LogRequestAnswer.m_iSize = sizeof(LogRequestAnswer);
	LogRequestAnswer.m_iEntrySize = sizeof(SNZBLogRequestAnswerEntry);
	LogRequestAnswer.m_iNrTrailingEntries = iNrEntries;
	LogRequestAnswer.m_iTrailingDataLength = bufsize;

	// Send the request answer
	send(m_iSocket, (char*) &LogRequestAnswer, sizeof(LogRequestAnswer), 0);

	// Send the data
	if (bufsize > 0)
	{
		send(m_iSocket, buf, bufsize, 0);
	}

	free(buf);
}

void MessageCommand::RequestEditQueue()
{
	SNZBEditQueueRequest* pEditQueueRequest = (SNZBEditQueueRequest*) & m_RequestBuffer;

	int From = pEditQueueRequest->m_iIDFrom;
	int To = pEditQueueRequest->m_iIDTo;
	int Step = 1;
	if ((pEditQueueRequest->m_iAction == NZBMessageRequest::eActionMoveTop) ||
	        ((pEditQueueRequest->m_iAction == NZBMessageRequest::eActionMoveOffset) &&
	         (pEditQueueRequest->m_iOffset < 0)))
	{
		Step = -1;
		int tmp = From; From = To; To = tmp;
	}

	for (int ID = From; ID != To + Step; ID += Step)
	{
		switch (pEditQueueRequest->m_iAction)
		{
			case NZBMessageRequest::eActionPause:
			case NZBMessageRequest::eActionResume:
				{
					g_pQueueCoordinator->EditQueuePauseUnpauseEntry(ID, pEditQueueRequest->m_iAction == NZBMessageRequest::eActionPause);
					break;
				}

			case NZBMessageRequest::eActionMoveOffset:
				{
					g_pQueueCoordinator->EditQueueMoveEntry(ID, pEditQueueRequest->m_iOffset, true);
					break;
				}

			case NZBMessageRequest::eActionMoveTop:
				{
					g_pQueueCoordinator->EditQueueMoveEntry(ID, -1000000, true);
					break;
				}

			case NZBMessageRequest::eActionMoveBottom:
				{
					g_pQueueCoordinator->EditQueueMoveEntry(ID, + 1000000, true);
					break;
				}

			case NZBMessageRequest::eActionDelete:
				{
					g_pQueueCoordinator->EditQueueDeleteEntry(ID);
					break;
				}
		}
	}
	SendResponse("Edit-Command completed successfully");
}

void MessageCommand::SetSocket(SOCKET iSocket)
{
	m_iSocket = iSocket;
}

void MessageCommand::Run()
{
	int iRequestReceived = 0;

	// Read the first package which needs to be a request

	iRequestReceived = recv(m_iSocket, (char*) & m_RequestBuffer, sizeof(m_RequestBuffer), 0);
	if (iRequestReceived < 0)
	{
		return;
	}

	SNZBMessageBase* pMessageBase = (SNZBMessageBase*) & m_RequestBuffer;

	// Make sure this is a nzbget request from a client
	if (pMessageBase->m_iId != NZBMESSAGE_SIGNATURE)
	{
		warn("Non-nzbget request received on port %i", g_pOptions->GetServerPort());

		if (m_iSocket > -1)
		{
			closesocket(m_iSocket);
		}

		return;
	}

	if (strcmp(pMessageBase->m_szPassword, g_pOptions->GetServerPassword()))
	{
		warn("nzbget request received on port %i, but password invalid", g_pOptions->GetServerPort());

		if (m_iSocket > -1)
		{
			closesocket(m_iSocket);
		}

		return;
	}

	// Info - connection received
	struct sockaddr_in PeerName;
	int iPeerNameLength = sizeof(PeerName);
	if (getpeername(m_iSocket, (struct sockaddr*)&PeerName, (socklen_t*) &iPeerNameLength) >= 0)
	{
#ifdef WIN32
		char* ip = inet_ntoa(PeerName.sin_addr);
#else
		char ip[20];
		inet_ntop(AF_INET, &PeerName.sin_addr, ip, sizeof(ip));
#endif
		debug("%s request received from %s", g_szMessageRequests[pMessageBase->m_iType], ip);
	}

	m_iExtraDataLength = iRequestReceived - pMessageBase->m_iSize;

	ProcessRequest();

	// Close the socket
	closesocket(m_iSocket);
}