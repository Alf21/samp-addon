#pragma once

#include "tcpsocket.h"


extern addonMutex* gMutex;
extern addonThread* gThread;
extern addonSocket* gSocket;


std::queue<std::string> send_to;
std::queue<std::string> rec_from;
extern std::queue<int> keyQueue;
extern std::queue<POINT> mouseQueue;




addonSocket::addonSocket()
{
	addonDebug("WSA constructor called from 'main_thread'");

	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);

	this->active = false;
	
	if(LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) 
	{
		addonDebug("Error while initializing WSA (%i)", WSAGetLastError());

		this->Close();

		return;
	}

	addonDebug("WSA library was successfuly initialized");
}



addonSocket::~addonSocket()
{
	addonDebug("WSA deconstructor called");

	WSACleanup();

	addonDebug("WSA library was successfuly cleaned up");

	if(this->active)
	{
		this->active = false;

		gThread->Stop(this->sendHandle);
		gThread->Stop(this->receiveHandle);
	}
}



int addonSocket::Start()
{
	int socket_handle = socket(AF_INET, SOCK_STREAM, NULL);

	if(socket_handle == -1)
	{
		addonDebug("Error while creating new socket (%i)", WSAGetLastError());

		this->Close();
	}

	return socket_handle;
}



void addonSocket::Close()
{
	closesocket(this->socketHandle);

	this->~addonSocket();
}



int addonSocket::set_nonblocking_socket(int sockid)
{
    DWORD flags = 1;

	return ioctlsocket(sockid, FIONBIO, &flags);
} 



void addonSocket::Connect(std::string address, int port)
{
	if(this->socketHandle == -1)
		return;

	struct sockaddr_in addr;
	struct hostent *host;

	host = gethostbyname(address.c_str());

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = *((struct in_addr *)host->h_addr);

	if(connect(this->socketHandle, (struct sockaddr *)&addr, sizeof(sockaddr)) == SOCKET_ERROR) 
	{
		addonDebug("Cannot connect to %s:%i trough TCP socket", address.c_str(), port);

		this->Close();

		return;
	}

	//this->set_nonblocking_socket(this->socket_handle);
	this->active = true;

	this->sendHandle = gThread->Start((LPTHREAD_START_ROUTINE)socket_send_thread);
	this->receiveHandle = gThread->Start((LPTHREAD_START_ROUTINE)socket_receive_thread);
}




void addonSocket::Send(std::string data)
{
	gMutex->Lock(gMutex->mutexHandle);
	send_to.push(data);
	gMutex->unLock(gMutex->mutexHandle);
}



DWORD _stdcall socket_send_thread(LPVOID lpParam)
{
	addonDebug("Thread 'socket_send_thread' succesfuly started");

	int key;
	POINT mouse;
	std::string data;
	std::stringstream format;

	while(gSocket->active)
	{
		if(!send_to.empty())
		{
			addonDebug("Send queue is not empty, processing...");

			for(unsigned int i = 0; i < send_to.size(); i++)
			{
				gMutex->Lock(gMutex->mutexHandle);
				data = send_to.front();
				send_to.pop();
				gMutex->unLock(gMutex->mutexHandle);

				format << data << '\n';
				data = format.str();
				format.clear();

				send(gSocket->socketHandle, data.c_str(), data.length(), NULL);
			}

			addonDebug("All items in send queue was processed");
		}

		if(!keyQueue.empty())
		{
			for(unsigned int i = 0; i < keyQueue.size(); i++)
			{
				gMutex->Lock(gMutex->mutexHandle);
				key = keyQueue.front();
				keyQueue.pop();
				gMutex->unLock(gMutex->mutexHandle);

				format << "TCPQUERY" << '>' << "CLIENT_CALL" << '>' << 1234 << '>' << key << '\n'; // "TCPQUERY>CLIENT_CALL>1234>%i   ---   Sends async key data (array) | %i - pressed key
				data = format.str();
				format.clear();

				send(gSocket->socketHandle, data.c_str(), data.length(), NULL);
			}
		}

		if(!mouseQueue.empty())
		{
			for(unsigned int i = 0; i < mouseQueue.size(); i++)
			{
				gMutex->Lock(gMutex->mutexHandle);
				mouse = mouseQueue.front();
				mouseQueue.pop();
				gMutex->unLock(gMutex->mutexHandle);

				format << "TCPQUERY" << '>' << "CLIENT_CALL" << '>' << 1235 << '>' << mouse.x << '_' << mouse.y << '\n'; // "TCPQUERY>CLIENT_CALL>1235>%i_%i   ---   Sends mouse cursor pos (array) | %i_%i = x_y
				data = format.str();
				format.clear();

				send(gSocket->socketHandle, data.c_str(), data.length(), NULL);
			}
		}

		Sleep(50);
	}

	return true;
}



DWORD _stdcall socket_receive_thread(LPVOID lpParam)
{
	addonDebug("Thread 'socket_receive_thread' successfuly started");

	std::string data;
	char buffer[16384];
	int bytes;

	while(gSocket->active)
	{
		bytes = recv(gSocket->socketHandle, buffer, sizeof buffer, NULL);

		if(bytes > 0)
		{
			addonDebug("There is data received, saving...");

			data.assign(buffer);

			addonDebug("New data: %s", data.c_str());

			gMutex->Lock(gMutex->mutexHandle);
			rec_from.push(data);
			gMutex->unLock(gMutex->mutexHandle);

			bytes = 0;
			memset(buffer, NULL, sizeof buffer);
		}

		Sleep(50);
	}

	return true;
}