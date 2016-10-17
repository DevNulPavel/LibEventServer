
#include "SingleThreadedHTTP.h"
#include "MultiThreadedHTTP.h"
#include "SingleThreadedTCP.h"

// примеры
// https://habrahabr.ru/post/217437/
// http://incpp.blogspot.ru/2009/04/libevent.html
// https://www.ibm.com/developerworks/ru/library/l-Libevent1/


int main()
{
    int result = tcpServer();

    return result;
}
