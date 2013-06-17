#pragma once



#include "tcpsocket.h"



extern amxMutex *gMutex;
extern amxPool *gPool;
extern amxThread *gThread;

extern logprintf_t logprintf;


extern std::queue<amxConnect> amxConnectQueue;
extern std::queue<amxConnectError> amxConnectErrorQueue;
extern std::queue<amxDisconnect> amxDisconnectQueue;


amxSocket *gSocket;

std::queue<processStruct> sendQueue;
std::queue<processStruct> recvQueue;





amxSocket::amxSocket()
{
	this->socketID = -1;
	this->socketInfo.maxClients = -1;

	#ifdef WIN32
		WORD wVersionRequested = MAKEWORD(2, 2);
		WSADATA wsaData;

		int err = WSAStartup(wVersionRequested, &wsaData);

		if(err || LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) 
		{
			logprintf("samp-addon: Winsock failed to initialize (error code: %i)", WSAGetLastError());

			this->Close();
		}
	#endif
}



amxSocket::~amxSocket()
{
	gThread->Stop(this->connHandle);
	gThread->Stop(this->recvHandle);
	gThread->Stop(this->sendHandle);

	#ifdef WIN32
		WSACleanup();
	#endif
}



void amxSocket::Create()
{
	this->socketID = socket(AF_INET, SOCK_STREAM, NULL);

	if(this->socketID == -1)
	{
		logprintf("samp-addon: socket creation failed (function 'socket' returned -1)");

		return;
	}
}



void amxSocket::Close()
{
	for(std::map<int, sockPool>::iterator i = gPool->socketPool.begin(); i != gPool->socketPool.end(); i++) 
	{
		if(i->second.socketid != -1) 
		{
			this->CloseSocket(i->second.socketid);
		}
	}

	this->CloseSocket(this->socketID);

	this->~amxSocket();
}



void amxSocket::CloseSocket(int socketid)
{
	#ifdef WIN32
		closesocket(socketid);
	#else
		close(socketid);
	#endif
}



void amxSocket::MaxClients(int max)
{
	if(this->socketID == -1)
		return;

	this->socketInfo.maxClients = max;
}



int amxSocket::FindFreeSlot()
{
	for(unsigned int i = 0; i != this->socketInfo.maxClients; i++)
	{
		if(!(gPool->socketPool.count(i)))
			return i;
	}

	return 65535;
}



void amxSocket::Bind(std::string ip)
{
	if(this->socketID == -1)
		return;

	this->bind_ip = ip;
}



void amxSocket::Listen(int port)
{
	if(this->socketID == -1)
		return;

	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	memset(&(addr.sin_zero), NULL, 8);

	if(!this->bind_ip.length()) 
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else 
		addr.sin_addr.s_addr = inet_addr(this->bind_ip.c_str());

	if(bind(this->socketID, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) 
	{
		logprintf("samp-addon: socket has failed to bind on port %i", port);

		return;
	}

	if(listen(this->socketID, 10) == SOCKET_ERROR) 
	{
		logprintf("samp-addon: socket has failed to listen on port %i", port);

		return;
	}

	logprintf("\nsamp-addon: started TCP server on port %i\n", port);

	this->socketInfo.active = true;

	this->connHandle = gThread->Start(socket_connection_thread, (void *)this->socketID);
	this->recvHandle = gThread->Start(socket_receive_thread, (void *)this->socketID);
	this->sendHandle = gThread->Start(socket_send_thread, (void *)this->socketID);
}



void amxSocket::KickClient(int clientid)
{
	if(this->socketID == -1)
		return;

	int client = gPool->socketPool[clientid].socketid;

	if(client != -1)
		this->CloseSocket(client);
}



bool amxSocket::IsClientConnected(int clientid)
{
	if(this->socketID == -1)
		return false;

	int socketid = gPool->socketPool[clientid].socketid;

	if((clientid <= this->socketInfo.maxClients) && (socketid != -1) && gPool->clientPool[clientid].auth)
	{
		char buffer[32768];

		if(recv(socketid, buffer, sizeof buffer, NULL) != 0)
			return true;
	}

	return false;
}



std::string amxSocket::GetClientIP(int clientid)
{
	if(!this->IsClientConnected(clientid))
		return std::string("0.0.0.0");

	struct sockaddr_in peer_addr;

	#ifdef WIN32
		int cLen = sizeof(peer_addr);
	#else
		std::size_t cLen = sizeof(peer_addr);
	#endif
		
	getpeername(gPool->socketPool[clientid].socketid, (struct sockaddr *)&peer_addr, &cLen);

	return std::string(inet_ntoa(peer_addr.sin_addr));
}



void amxSocket::Send(int clientid, std::string data)
{
	if(this->socketID == -1)
		return;

	if(this->IsClientConnected(clientid))
	{
		processStruct pushme;

		pushme.clientID = clientid;
		pushme.data = data;

		gMutex->Lock();
		sendQueue.push(pushme);
		gMutex->unLock();
	}
}



int amxSocket::SetNonblock(int socketid)
{
	if(socketid == -1)
		return NULL;

    DWORD flags;

	#ifdef WIN32
		flags = 1;

		return ioctlsocket(socketid, FIONBIO, &flags);
	#else
		/* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
		if((flags = fcntl(socketid, F_GETFL, 0)) == -1)
        {
			flags = 0;
		}

		return fcntl(socketid, F_SETFL, (flags | O_NONBLOCK));
		fcntl(socketid, F_SETFL, O_NONBLOCK);
	#endif
}



#ifdef WIN32
	DWORD socket_connection_thread(void *lpParam)
#else
	void *socket_connection_thread(void *lpParam)
#endif
{
	int socketid = (int)lpParam;
	sockaddr_in remote_client;

	#ifdef WIN32
		int cLen = sizeof(remote_client);
	#else
		size_t cLen = sizeof(remote_client);
	#endif

	do
	{
		int sockID = accept(socketid, (sockaddr *)&remote_client, &cLen);
		int clientid = gSocket->FindFreeSlot();

		if((sockID != NULL) && (sockID != SOCKET_ERROR) && (clientid != 65535)) 
		{
			amxConnect pushme;
			sockPool pool;

			logprintf("Connect: socketid: %i clientid: %i", sockID, clientid);

			gSocket->SetNonblock(sockID);

			pool.ip.assign(inet_ntoa(remote_client.sin_addr));
			pool.socketid = sockID;

			gPool->socketPool[clientid] = pool;

			pushme.ip = pool.ip;
			pushme.clientID = clientid;

			gMutex->Lock();
			amxConnectQueue.push(pushme);
			gMutex->unLock();
		} 
		else 
		{
			amxConnectError pushme;

			pushme.ip.assign(inet_ntoa(remote_client.sin_addr));

			if(clientid == 65535)
			{
				send(sockID, "TCPQUERY SERVER_CALL 1001 0", 27, NULL);

				pushme.error.assign("Server is full");
				pushme.errorCode = 1;

				gMutex->Lock();
				amxConnectErrorQueue.push(pushme);
				gMutex->unLock();
			}

			gSocket->KickClient(sockID);

			if(pushme.errorCode != 1)
			{
				pushme.error.assign("Unknown error");

				gMutex->Lock();
				amxConnectErrorQueue.push(pushme);
				gMutex->unLock();
			}
		}

		SLEEP(1);
	} 
	while((gSocket->socketInfo.active) && (gPool->pluginInit));

	#ifdef WIN32
		return true;
	#endif
}



#ifdef WIN32
	DWORD socket_receive_thread(void *lpParam)
#else
	void *socket_receive_thread(void *lpParam)
#endif
{
	int socketid = (int)lpParam;
	int sockID;
	char szRecBuffer[32768];

	memset(szRecBuffer, NULL, sizeof szRecBuffer);

	do 
	{
		for(std::map<int, sockPool>::iterator i = gPool->socketPool.begin(); i != gPool->socketPool.end(); i++) 
		{
			sockID = i->second.socketid;

			if(sockID != -1) 
			{
				int byte_len = recv(sockID, (char *)&szRecBuffer, sizeof szRecBuffer, NULL);

				if(byte_len > 0) 
				{
					processStruct pushme;

					szRecBuffer[byte_len] = NULL;

					pushme.clientID = i->first;
					pushme.data.assign(szRecBuffer);

					memset(szRecBuffer, NULL, sizeof szRecBuffer);

					logprintf("Recieved data from %i: %s", i->first, pushme.data.c_str());

					gMutex->Lock();
					recvQueue.push(pushme);
					gMutex->unLock();
				}
				else if(!byte_len) 
				{
					amxDisconnect pushme;

					pushme.clientID = i->first;

					gSocket->CloseSocket(sockID);
					gPool->socketPool.erase(i);

					gMutex->Lock();
					amxDisconnectQueue.push(pushme);
					gMutex->unLock();
				}
			}
		}

		SLEEP(1);
	} 
	while((gSocket->socketInfo.active) && (gPool->pluginInit));

	#ifdef WIN32
		return true;
	#endif
}



#ifdef WIN32
	DWORD socket_send_thread(void *lpParam)
#else
	void *socket_send_thread(void *lpParam)
#endif
{
	processStruct data;

	do
	{
		if(!sendQueue.empty())
		{
			for(unsigned int i = 0; i < sendQueue.size(); i++)
			{
				gMutex->Lock();
				data = sendQueue.front();
				sendQueue.pop();
				gMutex->unLock();

				logprintf("Send data to %i: %s", data.clientID, data.data.c_str());

				send(gPool->socketPool[data.clientID].socketid, data.data.c_str(), data.data.length(), NULL);

				/*if(send(data.clientid, data.data.c_str(), data.data.length(), NULL) == SOCKET_ERROR)
				{
					logprintf("Error while sending data %s", data.data.c_str());

					continue;
				}*/
			}
		}

		SLEEP(1);
	}
	while((gSocket->socketInfo.active) && (gPool->pluginInit));

	#ifdef WIN32
		return true;
	#endif
}