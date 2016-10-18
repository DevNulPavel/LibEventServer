
#include "SingleThreadedHTTP.h"
#include "MultiThreadedHTTP.h"
#include "SingleThreadedTCP.h"
#include "MultiThreadedTCP.h"
#include "MultiThreadedTCPFilter.h"

// примеры
// https://habrahabr.ru/post/217437/
// http://incpp.blogspot.ru/2009/04/libevent.html
// https://www.ibm.com/developerworks/ru/library/l-Libevent1/


int main()
{
    int result = multiThreadedTcpServerFilter();

    return result;
}
