#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

// ------------------------------
// 設定
// ------------------------------
const int PORT = 9000;
const int BUFFER_SIZE = 1024;
const int ACCEPT_EX_COUNT = 4;

// ------------------------------
// IOの種類
// ------------------------------
enum IO_TYPE {
    IO_ACCEPT,
    IO_RECV,
    IO_SEND
};

// ------------------------------
// ソケットごとの情報
// ------------------------------
struct PER_SOCKET_CONTEXT {
    SOCKET sock;
};

// ------------------------------
// IOごとの情報
// ------------------------------
struct PER_IO_CONTEXT {
    OVERLAPPED ol;
    WSABUF wsaBuf;
    char buffer[BUFFER_SIZE];
    IO_TYPE type;

    // AcceptEx用: 新規クライアントソケット
    SOCKET acceptSock;
};

// AcceptEx関数ポインタ
LPFN_ACCEPTEX lpfnAcceptEx = nullptr;

// グローバルハンドル（簡略化のため）
HANDLE g_hIOCP = nullptr;
SOCKET g_listenSock = INVALID_SOCKET;

// ------------------------------
// エラー表示ヘルパー
// ------------------------------
void PrintLastError(const char* msg)
{
    printf("%s: %d\n", msg, WSAGetLastError());
}

// ------------------------------
// AcceptEx を投げる
// ------------------------------
bool PostAccept()
{
    PER_IO_CONTEXT* ioCtx = new PER_IO_CONTEXT();
    ZeroMemory(ioCtx, sizeof(PER_IO_CONTEXT));

    ioCtx->type = IO_ACCEPT;
    ioCtx->acceptSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (ioCtx->acceptSock == INVALID_SOCKET) {
        PrintLastError("WSASocket (acceptSock)");
        delete ioCtx;
        return false;
    }

    ioCtx->wsaBuf.buf = ioCtx->buffer;
    ioCtx->wsaBuf.len = BUFFER_SIZE;

    DWORD bytes = 0;

    BOOL ret = lpfnAcceptEx(
        g_listenSock,
        ioCtx->acceptSock,
        ioCtx->buffer,
        0,  // 最初のデータは受け取らない
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytes,
        &ioCtx->ol
    );

    if (!ret) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            PrintLastError("AcceptEx");
            closesocket(ioCtx->acceptSock);
            delete ioCtx;
            return false;
        }
    }

    return true;
}

// ------------------------------
// クライアントソケットに対して WSARecv を投げる
// ------------------------------
bool PostRecv(PER_SOCKET_CONTEXT* sockCtx)
{
    PER_IO_CONTEXT* ioCtx = new PER_IO_CONTEXT();
    ZeroMemory(ioCtx, sizeof(PER_IO_CONTEXT));

    ioCtx->type = IO_RECV;
    ioCtx->wsaBuf.buf = ioCtx->buffer;
    ioCtx->wsaBuf.len = BUFFER_SIZE;

    DWORD flags = 0;
    DWORD bytes = 0;

    int ret = WSARecv(
        sockCtx->sock,
        &ioCtx->wsaBuf,
        1,
        &bytes,
        &flags,
        &ioCtx->ol,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            PrintLastError("WSARecv");
            delete ioCtx;
            return false;
        }
    }

    return true;
}

// ------------------------------
// 送信（echo用）
// ------------------------------
bool PostSend(PER_SOCKET_CONTEXT* sockCtx, const char* data, DWORD len)
{
    PER_IO_CONTEXT* ioCtx = new PER_IO_CONTEXT();
    ZeroMemory(ioCtx, sizeof(PER_IO_CONTEXT));

    ioCtx->type = IO_SEND;
    memcpy(ioCtx->buffer, data, len);
    ioCtx->wsaBuf.buf = ioCtx->buffer;
    ioCtx->wsaBuf.len = len;

    DWORD bytes = 0;

    int ret = WSASend(
        sockCtx->sock,
        &ioCtx->wsaBuf,
        1,
        &bytes,
        0,
        &ioCtx->ol,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            PrintLastError("WSASend");
            delete ioCtx;
            return false;
        }
    }

    return true;
}

// ------------------------------
// IOCPワーカースレッド
// ------------------------------
DWORD WINAPI WorkerThread(LPVOID lpParam)
{
    while (true) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        PER_IO_CONTEXT* ioCtx = nullptr;

        BOOL ok = GetQueuedCompletionStatus(
            g_hIOCP,
            &bytes,
            &key,
            (LPOVERLAPPED*)&ioCtx,
            INFINITE
        );

        PER_SOCKET_CONTEXT* sockCtx = (PER_SOCKET_CONTEXT*)key;

        if (!ok) {
            int err = GetLastError();
            if (ioCtx == nullptr) {
                // IOCP自体のエラー
                printf("GQCS failed: %d\n", err);
                continue;
            }
        }

        if (ioCtx == nullptr) {
            // 理論上あまり来ないケース
            continue;
        }

        if (bytes == 0 && ioCtx->type != IO_ACCEPT) {
            // クライアント切断
            printf("Client disconnected.\n");
            if (sockCtx) {
                closesocket(sockCtx->sock);
                delete sockCtx;
            }
            delete ioCtx;
            continue;
        }

        switch (ioCtx->type) {
        case IO_ACCEPT:
        {
            // Accept完了
            printf("Client accepted.\n");

            // 新しいソケットコンテキスト作成
            PER_SOCKET_CONTEXT* newSockCtx = new PER_SOCKET_CONTEXT();
            newSockCtx->sock = ioCtx->acceptSock;

            // SO_UPDATE_ACCEPT_CONTEXT を呼ぶ
            setsockopt(
                newSockCtx->sock,
                SOL_SOCKET,
                SO_UPDATE_ACCEPT_CONTEXT,
                (char*)&g_listenSock,
                sizeof(g_listenSock)
            );

            // 新しいソケットをIOCPに関連付け
            CreateIoCompletionPort(
                (HANDLE)newSockCtx->sock,
                g_hIOCP,
                (ULONG_PTR)newSockCtx,
                0
            );

            // このクライアントに対して最初のRecvを投げる
            PostRecv(newSockCtx);

            // AcceptEx を次のためにもう一回投げる
            PostAccept();

            // AcceptEx用IOコンテキストはここで解放
            delete ioCtx;
            break;
        }

        case IO_RECV:
        {
            // 受信データをそのままecho
            printf("Received %lu bytes: %.*s\n", bytes, (int)bytes, ioCtx->buffer);

            // echo送信
            PostSend(sockCtx, ioCtx->buffer, bytes);

            // 次の受信を投げる
            PostRecv(sockCtx);

            delete ioCtx;
            break;
        }

        case IO_SEND:
        {
            // 送信完了
            // 特にやることはないので解放
            delete ioCtx;
            break;
        }
        }
    }

    return 0;
}

// ------------------------------
// AcceptEx関数ポインタ取得
// ------------------------------
bool LoadAcceptEx()
{
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD bytes = 0;

    int ret = WSAIoctl(
        g_listenSock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx,
        sizeof(guidAcceptEx),
        &lpfnAcceptEx,
        sizeof(lpfnAcceptEx),
        &bytes,
        NULL,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        PrintLastError("WSAIoctl (AcceptEx)");
        return false;
    }

    return true;
}

// ------------------------------
// main
// ------------------------------
int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        PrintLastError("WSAStartup");
        return 1;
    }

    // リスニングソケット作成
    g_listenSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (g_listenSock == INVALID_SOCKET) {
        PrintLastError("WSASocket (listen)");
        return 1;
    }

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        PrintLastError("bind");
        return 1;
    }

    if (listen(g_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        PrintLastError("listen");
        return 1;
    }

    // IOCP作成
    g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_hIOCP == NULL) {
        PrintLastError("CreateIoCompletionPort (base)");
        return 1;
    }

    // リスニングソケットをIOCPに関連付け
    PER_SOCKET_CONTEXT* listenCtx = new PER_SOCKET_CONTEXT();
    listenCtx->sock = g_listenSock;

    if (CreateIoCompletionPort(
        (HANDLE)g_listenSock,
        g_hIOCP,
        (ULONG_PTR)listenCtx,
        0
    ) == NULL) {
        PrintLastError("CreateIoCompletionPort (listen)");
        return 1;
    }

    // AcceptExロード
    if (!LoadAcceptEx()) {
        return 1;
    }

    // AcceptExを複数本投げておく
    for (int i = 0; i < ACCEPT_EX_COUNT; ++i) {
        if (!PostAccept()) {
            return 1;
        }
    }

    // ワーカースレッド起動
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threadCount = si.dwNumberOfProcessors * 2;

    for (int i = 0; i < threadCount; ++i) {
        HANDLE hThread = CreateThread(
            NULL,
            0,
            WorkerThread,
            NULL,
            0,
            NULL
        );
        CloseHandle(hThread);
    }

    printf("IOCP echo server running on port %d...\n", PORT);

    // メインスレッドは何もしない
    while (true) {
        Sleep(1000);
    }

    WSACleanup();
    return 0;
}
