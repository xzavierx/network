#include "iocp.h"
#include <synchapi.h>

int main() {
  CIocp iocp;
  iocp.LoadSocketLib();
  iocp.Start();
  while(true) {
    Sleep(1000);
  }
}