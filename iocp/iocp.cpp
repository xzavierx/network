#include "iocp.h"
#include <MSWSock.h>
#include <WinSock2.h>
#include <basetsd.h>
#include <errhandlingapi.h>
#include <handleapi.h>
#include <inaddr.h>
#include <ioapiset.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <string>
#include <synchapi.h>
#include <sysinfoapi.h>
#include <vcruntime.h>
#include <vcruntime_string.h>
#include <winbase.h>
#include <winerror.h>
#include <winnt.h>
#include <winuser.h>
#include <iostream>

#define WORKER_THREADS_PER_PROCESSOR 2
#define MAX_POST_ACCEPT 10
#define EXIT_CODE NULL

#define RELEASE(x) do{if (x != NULL) {delete x; x = NULL;}}while(0) 
#define RELEASE_HANDLE(x) do{if (x != NULL && x != INVALID_HANDLE_VALUE){CloseHandle(x); x = NULL;}}while(0)
#define RELEASE_SOCKET(x) do{if (x != INVALID_SOCKET){ closesocket(x); x = INVALID_SOCKET; }}while(0)

CIocp::CIocp()
  : m_nThreads(0)
  , m_hShutDownEvent(NULL)
  , m_hIOCompletionPort(NULL)
  , m_phWorkerThreads(NULL)
  , m_strIP(DEFAULT_IP)
  , m_nPort(DEFAULT_PORT)
  , m_lpfnAcceptEx(NULL)
  , m_pListenContext(NULL) {}

CIocp::~CIocp() {
}

DWORD WINAPI CIocp::_WorkerThread(LPVOID lpParam) {
  THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
  CIocp* pIocp = (CIocp*)pParam->pIocp;
  int nThreadNo = (int)pParam->nThreadNo;

  printf("工作者线程启动ID:%d.\n", nThreadNo);

  OVERLAPPED *pOverlapped = NULL;
  PER_SOCKET_CONTEXT *pSocketCotext = NULL;
  DWORD dwBytesTransfered = 0;

  while (WAIT_OBJECT_0 != WaitForSingleObject(pIocp->m_hShutDownEvent, 0)) {
    BOOL bReturn = GetQueuedCompletionStatus(
      pIocp->m_hIOCompletionPort,
      &dwBytesTransfered, 
      (PULONG_PTR)&pSocketCotext, 
      &pOverlapped, 
      INFINITE);
    if (EXIT_CODE == (DWORD)pSocketCotext) {
      break;
    }
    if (!bReturn) {
      DWORD dwErr = GetLastError();
      if (!pIocp->HandleError(pSocketCotext, dwErr)) {
        break;
      }
      continue;
    } else {
      PER_IO_CONTEXT* pIoContext = 
        CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_overlapped);
      if ((0 == dwBytesTransfered) 
          && (RECV_POSTED == pIoContext->m_opType 
            || SEND_POSTED == pIoContext->m_opType)) {
        printf("客户端：%s：%d断开连接.\n", 
          inet_ntoa(pSocketCotext->m_clientAddr.sin_addr),
          ntohs(pSocketCotext->m_clientAddr.sin_port));
        pIocp->_RemoveContext(pSocketCotext);
        continue;
      } else {
        switch(pIoContext->m_opType) {
          case ACCEPT_POSTED:
            pIocp->_DoAccept(pSocketCotext, pIoContext);
            break;
          case RECV_POSTED:
            pIocp->_DoRecv(pSocketCotext, pIoContext);
            break;
          case SEND_POSTED:
            break;
          default:
            printf("_WorkThread中的pIoctext->m_opType %d参数异常.", pIoContext->m_opType);
            break;
        }

      }
    }
  }
  printf("工作者线程%d号退出.\n", nThreadNo);
  RELEASE(lpParam);
  return 0;
}

bool CIocp::LoadSocketLib() {
  WSADATA wsaData;
  int nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (NO_ERROR != nResult) {
    printf("初始化Winsock 2.2失败!\n");
    return false;
  }
  return true;
}

bool CIocp::Start() {
  InitializeCriticalSection(&m_csContextList);

  m_hShutDownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

  if (!_InitializeIOCP()) {
    printf("初始化IOCP失败!\n");
    return false;
  } else {
    printf("\nIOCP初始化完毕\n");
  }
  if (!_InitializeListenSocket()) {
    printf("Listen Socket初始化失败!\n");
    _DeInitialize();
    return false;
  } else {
    printf("Listen Socket初始化完毕.\n");
  }
  printf("系统准备就绪，等待连接...\n");
  return true;
}

void CIocp::Stop() {
  if (m_pListenContext != NULL && m_pListenContext->m_socket != INVALID_SOCKET) {
    SetEvent(m_hShutDownEvent);
    for (int i = 0; i < m_nThreads; i++) {
      PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL); 
    }
    WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);
    _ClearContextList();
    _DeInitialize();
    printf("停止监听\n");
  }
}

bool CIocp::_InitializeIOCP() {
  m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  if (NULL == m_hIOCompletionPort) {
    printf("建立完成端口失败! 错误代码:%d!\n", WSAGetLastError());
    return false;
  }
  m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();

  m_phWorkerThreads = new HANDLE[m_nThreads];
  
  DWORD nThreadID; 
  for (int i = 0; i < m_nThreads; ++i) {
    THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
    pThreadParams->pIocp = this;
    pThreadParams->nThreadNo = i + 1;
    m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void*)pThreadParams, 0, &nThreadID);
  }
  printf("建立_WorkerThread %d个\n", m_nThreads);
  return true;
}

bool CIocp::_InitializeListenSocket() {
  GUID GuidAcceptEx = WSAID_ACCEPTEX;
  GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

  struct sockaddr_in ServerAddress;

  m_pListenContext = new PER_SOCKET_CONTEXT;

  m_pListenContext->m_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (INVALID_SOCKET == m_pListenContext->m_socket) {
    printf("初始化Socket失败，错误代码:%d\n", WSAGetLastError());
    return false;
  }

  printf("WSASocket() 完成\n");
  if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_socket, m_hIOCompletionPort, (DWORD)m_pListenContext, 0)) {
    printf("绑定ListenSocket至完成端口失败！错误代码：%d\n", WSAGetLastError());
    RELEASE_SOCKET(m_pListenContext->m_socket);
    return false;
  }

  printf("Listen Socket绑定完成端口 完成\n");

  ZeroMemory((char*)&ServerAddress, sizeof(ServerAddress));
  ServerAddress.sin_family = AF_INET;
  ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  ServerAddress.sin_port = htons(m_nPort);

  if (SOCKET_ERROR == bind(m_pListenContext->m_socket, (struct sockaddr*)&ServerAddress, sizeof(ServerAddress))) {
    printf("bind() 函数执行错误.\n");
    return false;
  }

  printf("bind() 完成.\n");
  if (SOCKET_ERROR == listen(m_pListenContext->m_socket, SOMAXCONN)) {
    printf("listen()函数执行错误.\n");
    return false;
  }
  printf("listen()完成.\n");
  DWORD dwBytes = 0;
  if (SOCKET_ERROR == WSAIoctl(
    m_pListenContext->m_socket,
    SIO_GET_EXTENSION_FUNCTION_POINTER,
    &GuidAcceptEx,
    sizeof(GuidAcceptEx),
    &m_lpfnAcceptEx, 
    sizeof(m_lpfnAcceptEx),
    &dwBytes,
    NULL,
    NULL)) {
      printf("WSAIoctl未能获取AcceptEx函数指针，错误代码:%d\n", WSAGetLastError());
      _DeInitialize();
      return false;
  }
  if (SOCKET_ERROR == WSAIoctl(
    m_pListenContext->m_socket,
    SIO_GET_EXTENSION_FUNCTION_POINTER,
    &GuidGetAcceptExSockAddrs,
    sizeof(GuidGetAcceptExSockAddrs),
    &m_lpfnGetAcceptExSockAddrs,
    sizeof(m_lpfnGetAcceptExSockAddrs),
    &dwBytes,
    NULL, 
    NULL)) {
      printf("WSAIoctl未能获取GuidGetAcceptExSockAddrs函数指针，错误代码:%d\n", WSAGetLastError());
      _DeInitialize();
      return false;
  }
  for (int i = 0; i < MAX_POST_ACCEPT; i++) {
    PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();
    if (!_PostAccept(pAcceptIoContext)) {
      m_pListenContext->RemoveContext(pAcceptIoContext);
      return false;
    }
  }
  printf("投递%d个AcceptEx请求完毕", MAX_POST_ACCEPT);
  return true;
}

void CIocp::_DeInitialize() {
  DeleteCriticalSection(&m_csContextList);
  RELEASE_HANDLE(m_hShutDownEvent);
  for (int i = 0; i < m_nThreads; i++) {
    RELEASE_HANDLE(m_phWorkerThreads[i]);
  }
  RELEASE(m_phWorkerThreads);
  RELEASE_HANDLE(m_hIOCompletionPort);
  RELEASE(m_pListenContext);
  printf("释放资源完毕\n");
}

bool CIocp::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext) {
  assert(INVALID_SOCKET != m_pListenContext->m_socket);

  DWORD dwBytes = 0;
  pAcceptIoContext->m_opType = ACCEPT_POSTED;
  WSABUF * p_wbuf = &pAcceptIoContext->m_wsaBuf;
  OVERLAPPED* p_ol = &pAcceptIoContext->m_overlapped;

  pAcceptIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  if (INVALID_SOCKET == pAcceptIoContext->m_sockAccept) {
    printf("创建用于Accept的Socket失败！错误代码:%d", WSAGetLastError());
    return false;
  }
  if (!m_lpfnAcceptEx(
    m_pListenContext->m_socket, 
    pAcceptIoContext->m_sockAccept, 
    p_wbuf->buf, 
    p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
    sizeof(SOCKADDR_IN) + 16,
    sizeof(SOCKADDR_IN) + 16, 
    &dwBytes, 
    p_ol)) {
      if (WSA_IO_PENDING != WSAGetLastError()) {
        printf("投递AcceptEx请求失败，错误代码：%d", WSAGetLastError());
        return false;
      }
  }
  return true;
}

bool CIocp::_DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext) {
  SOCKADDR_IN* ClientAddr = NULL;
  SOCKADDR_IN* LocalAddr = NULL;
  int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

  this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
    sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);
  printf("客户端 %s:%d 连入.\n", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port));
  printf("客户端 %s:%d 信息:%s\n", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port), pIoContext->m_wsaBuf.buf);

  PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
  pNewSocketContext->m_socket = pIoContext->m_sockAccept;
  memcpy(&(pNewSocketContext->m_clientAddr), ClientAddr, sizeof(SOCKADDR_IN));

  if (_AssociateWithIOCP(pNewSocketContext)) {
    RELEASE(pNewSocketContext);
    return false;
  }
  PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
  pNewIoContext->m_opType = RECV_POSTED;
  pNewIoContext->m_sockAccept = pNewSocketContext->m_socket;

  if (_PostRecv(pNewIoContext)) {
    pNewSocketContext->RemoveContext(pNewIoContext);
    return false;
  }

  _AddToContextList(pNewSocketContext);
  pIoContext->ResetBuffer();
  return _PostAccept(pIoContext);
}

bool CIocp::_PostRecv(PER_IO_CONTEXT* pIoContext) {
  DWORD dwFlags = 0;
  DWORD dwBytes = 0;
  WSABUF * p_wbuf = &pIoContext->m_wsaBuf;
  OVERLAPPED *p_ol = &pIoContext->m_overlapped;

  pIoContext->ResetBuffer();
  pIoContext->m_opType = RECV_POSTED;
  
  int nBytesRecv = WSARecv(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);
  if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError())) {
    printf("投递第一个WSARecv失败!");
    return false;
  }
  return true;
}

bool CIocp::_DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext) {
  SOCKADDR_IN* ClientAddr = &pSocketContext->m_clientAddr;
  printf("收到 %s:%d 信息: %s", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port), pIoContext->m_wsaBuf.buf);
  return _PostRecv(pIoContext);
}

bool CIocp::_AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext) {
  HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_socket, m_hIOCompletionPort, (DWORD)pContext, 0);

  if (NULL == hTemp) {
    printf("执行CreateIoCompletionPort()出现错误，错误代码:%d\n", GetLastError());
    return false;
  }
  return true;
}

void CIocp::_AddToContextList(PER_SOCKET_CONTEXT* pHandleData) {
  EnterCriticalSection(&m_csContextList);

  m_arrayClientContext.push_back(pHandleData);

  LeaveCriticalSection(&m_csContextList);
}

void CIocp::_RemoveContext(PER_SOCKET_CONTEXT* pSocketContext) {
  EnterCriticalSection(&m_csContextList);
  for (int i = 0; i < m_arrayClientContext.size(); i++) {
    if (pSocketContext == m_arrayClientContext.at(i)) {
      RELEASE(pSocketContext);
      m_arrayClientContext.erase(m_arrayClientContext.begin() + i);
      break;
    }
  }
  LeaveCriticalSection(&m_csContextList);
}

void CIocp::_ClearContextList() {
  EnterCriticalSection(&m_csContextList);
  for (int i = 0; i < m_arrayClientContext.size(); ++i) {
    delete m_arrayClientContext.at(i);
  }
  m_arrayClientContext.clear();
  LeaveCriticalSection(&m_csContextList);
}

std::string CIocp::GetLocalIP() {
  char hostname[MAX_PATH] = {0};
  gethostname(hostname, MAX_PATH);
  struct hostent FAR* lpHostEnt = gethostbyname(hostname);
  if (lpHostEnt == NULL) {
    return DEFAULT_IP;
  }
  LPSTR lpAddr = lpHostEnt->h_addr_list[0];
  struct in_addr inAddr;
  memmove(&inAddr, lpAddr, 4);
  m_strIP = std::string(inet_ntoa(inAddr));

  return m_strIP;
}

int CIocp::_GetNoOfProcessors() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors;
}


bool CIocp::_IsSocketAlive(SOCKET s) {
  int nByteSent = send(s, "", 0, 0);
  if (-1 == nByteSent) return false;
  return true;
}


bool CIocp::HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD dwError) {
  if (WAIT_TIMEOUT == dwError) {
    if (!_IsSocketAlive(pContext->m_socket)) {
      printf("检测到客户端异常退出!");
      _RemoveContext(pContext);
      return true;
    } else {
      printf("网络操作超时!重试中...");
      return true;
    }
  } else if (ERROR_NETNAME_DELETED == dwError) {
    printf("检测到客户端异常退出!");
    _RemoveContext(pContext);
    return true;
  } else {
    printf("完成端口操作出现错误，线程退出，错误代码:%d\n", dwError);
    return false;
  }
}
