#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <minwinbase.h>
#include <mswsockdef.h>
#include <string>
#include <vector>
#include <cassert>
#pragma comment(lib, "ws2_32.lib")

// 缓冲区长度
#define MAX_BUFFER_LEN 8192

// 默认端口
#define DEFAULT_PORT 12345
// 默认IP地址
#define DEFAULT_IP "127.0.0.1"

// 在完成端口上投递的I/O操作的类型
typedef enum _OPERATION_TYPE {
  ACCEPT_POSTED, 
  SEND_POSTED,
  RECV_POSTED,
  NULL_POSTED
}OPERATION_TYPE;

typedef struct _PER_IO_CONTEXT {
  OVERLAPPED m_overlapped;            // 每一个重叠网络操作的重叠结构 
  SOCKET m_sockAccept;                // 这个网络操作的Socket
  WSABUF m_wsaBuf;                    // WSA类型的缓冲区，用于给重叠操作传参数
  char m_szBuffer[MAX_BUFFER_LEN];    // 这个是WSABUF里具体存字符的缓冲区
  OPERATION_TYPE m_opType;            // 表示网络操作的类型

  _PER_IO_CONTEXT() {
    ZeroMemory(&m_overlapped, sizeof(m_overlapped));
    ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
    m_sockAccept = INVALID_SOCKET;
    m_wsaBuf.buf = m_szBuffer;
    m_wsaBuf.len = MAX_BUFFER_LEN;
    m_opType = NULL_POSTED;
  }

  ~_PER_IO_CONTEXT() {
    if(m_sockAccept != INVALID_SOCKET) {
      closesocket(m_sockAccept);
      m_sockAccept = INVALID_SOCKET;
    }
  }

  void ResetBuffer() {
    ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
  }
} PER_IO_CONTEXT, *PPER_IO_CONTEXT;

typedef struct _PER_SOCKET_CONTEXT {
  SOCKET m_socket;                                // 每一个客户端的socket
  SOCKADDR_IN m_clientAddr;                       // 客户端的地址
  std::vector<_PER_IO_CONTEXT*> m_arrayIoContext;      // 客户端网络操作的上下文数据

  _PER_SOCKET_CONTEXT() {
    m_socket = INVALID_SOCKET;
    memset(&m_clientAddr, 0, sizeof(m_clientAddr));
  }

  ~_PER_SOCKET_CONTEXT(){
    if (m_socket != INVALID_SOCKET) {
      closesocket(m_socket);
      m_socket = INVALID_SOCKET;
    }
    for (int i = 0; i < m_arrayIoContext.size(); ++i) {
      delete m_arrayIoContext.at(i);
    }
    m_arrayIoContext.clear();
  }

  _PER_IO_CONTEXT* GetNewIoContext() {
    _PER_IO_CONTEXT* p = new _PER_IO_CONTEXT;
    m_arrayIoContext.push_back(p);
    return p;
  }

  void RemoveContext(_PER_IO_CONTEXT* pContext) {
    assert(pContext != NULL);

    for (int i = 0; i < m_arrayIoContext.size(); ++i) {
      if (pContext == m_arrayIoContext.at(i)) {
        delete pContext;
        pContext = NULL;
        m_arrayIoContext.erase(m_arrayIoContext.begin() + i);
        break;
      }
    }
  }
} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;

class CIocp;
typedef struct _tagThreadParams_WORKER {
  CIocp* pIocp;
  int nThreadNo;
}THREADPARAMS_WORKER,*PTHREADPARAM_WORKER;

class CIocp {
public:
  CIocp();
  ~CIocp();

  bool Start();

  void Stop();

  bool LoadSocketLib();

  void UnloadSocketLib() {WSACleanup();}

  std::string GetLocalIP();

  void SetPort(const int nPort) { m_nPort = nPort; }

protected:
  bool _InitializeIOCP();

  bool _InitializeListenSocket();

  void _DeInitialize();

  bool _PostAccept(PER_IO_CONTEXT* pAcceptIoContext);

  bool _PostRecv(PER_IO_CONTEXT* pIoContext);

  bool _DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

  bool _DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

  void _AddToContextList(PER_SOCKET_CONTEXT* pSocketContext);

  void _RemoveContext(PER_SOCKET_CONTEXT* pSocketContext);

  void _ClearContextList();

  bool _AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext);

  bool HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD dwErr);

  static DWORD WINAPI _WorkerThread(LPVOID lpParam);

  int _GetNoOfProcessors();

  bool _IsSocketAlive(SOCKET s);

private:
  int m_nThreads;             // 生成的线程数量
  HANDLE m_hShutDownEvent;    // 用来通知线程系统退出的事件，为了能够更好的退出线程
  HANDLE m_hIOCompletionPort; // 完成端口的句柄
  HANDLE* m_phWorkerThreads;   // 工作者线程的句柄
  std::string m_strIP;            // 服务器的IP地址
  int m_nPort;                // 服务器监听端口
  CRITICAL_SECTION m_csContextList; // 用于Worker线程同步的互斥量
  std::vector<PER_SOCKET_CONTEXT*> m_arrayClientContext; // 客户端socket的context信息
  PER_SOCKET_CONTEXT* m_pListenContext; // 用于监听的Socket的Context信息
  LPFN_ACCEPTEX m_lpfnAcceptEx;
  LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockAddrs;
};
