// ===============================
// IOCP Minimal Server Skeleton
// ===============================

#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

// ----------------------------------
// I/O operation types
// ----------------------------------
enum IO_TYPE {
	IO_ACCEPT,
	IO_RECV,
	IO_SEND
};

// ----------------------------------
// Per-I/O data
// ----------------------------------
struct PER_IO_DATA {
	OVERLAPPED ol;
	WSABUF wsaBuf;
	char buffer[1024];
	IO_TYPE type;
};

// ----------------------------------
// Per-socket data
// ----------------------------------
struct PER_SOCKET_DATA {
	SOCKET sock;
};

// ----------------------------------
// Worker thread
// ----------------------------------
DWORD WINAPI WorkerThread(LPVOID lpParam)
{
	HANDLE hIOCP = (HANDLE)lpParam;

	DWORD bytes;
	ULONG_PTR key;
	PER_IO_DATA* ioData;

	while (true) {
		BOOL ok = GetQueuedCompletionStatus(
			hIOCP,
			&bytes,
			&key,
			(LPOVERLAPPED*)&ioData,
			INFINITE
			);

		PER_SOCKET_DATA* sockData = (PER_SOCKET_DATA*)key;

		if (!ok || bytes == 0) {
			closesocket(sockData->sock);
			delete sockData;
			delete ioData;
			continue;
		}

		switch (ioData->type) {
		case IO_ACCEPT:
			// 次のAcceptExを投げる
			// 新規クライアントに対してWSARecvを投げる
			break;

		case IO_RECV:
			// 受信データ処理
			// 次のWSARecvを投げる
			break;

		case IO_SEND:
			// 送信完了処理
			break;
		}
	}
	return 0;
}

// ----------------------------------
// Main
// ----------------------------------
int main()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// 1. IOCP作成
	HANDLE hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

	// 2. リスニングソケット作成
	SOCKET listenSock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(9000);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	bind(listenSock, (sockaddr*)&addr, sizeof(addr));
	listen(listenSock, SOMAXCONN);

	// 3. IOCPにリスニングソケットを関連付け
	PER_SOCKET_DATA* listenData = new PER_SOCKET_DATA();
	listenData->sock = listenSock;

	CreateIoCompletionPort((HANDLE)listenSock, hIOCP, (ULONG_PTR)listenData, 0);

	// 4. ワーカースレッド起動
	SYSTEM_INFO si;
	GetSystemInfo(&si);

	for (DWORD i = 0; i < si.dwNumberOfProcessors * 2; i++) {
		CreateThread(NULL, 0, WorkerThread, hIOCP, 0, NULL);
	}

	// 5. AcceptEx を投げる（実際にはループで複数投げる）
	// ※ AcceptEx の取得と呼び出しは省略（MSWSOCK.DLLから関数ポインタ取得が必要）

	printf("IOCP server running...\n");

	while (true) Sleep(1000);
	return 0;
}