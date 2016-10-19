#include "SingleThreadedDNS.h"
// std
#include <stdexcept>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <cstdint>
#include <vector>
// libevent
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/dns.h>
#include <event2/util.h>
#include <event.h>
#include <evhttp.h>
#include <evdns.h>


// примеры
// https://habrahabr.ru/post/217437/
// http://incpp.blogspot.ru/2009/04/libevent.html
// https://www.ibm.com/developerworks/ru/library/l-Libevent1/

//using namespace std;

typedef std::unique_ptr<event_base, decltype(&event_base_free)>  EventHandler;
typedef std::unique_ptr<evhttp, decltype(&evhttp_free)> ServerPtr;


int n_pending_requests = 0;
struct event_base *base = NULL;

struct UserData {
    char *name; /* the name we're resolving */
    int idx; /* its position on the command line */
};

void dnsCallback(int errcode, struct evutil_addrinfo *addr, void *ptr)
{
    UserData* data = (UserData*)ptr;
    const char *name = data->name;
    if (errcode) {
        printf("%d. %s -> %s\n", data->idx, name, evutil_gai_strerror(errcode));
    } else {
        struct evutil_addrinfo *ai;
        printf("%d. %s", data->idx, name);
        if (addr->ai_canonname){
            printf(" [%s]", addr->ai_canonname);
        }
        puts("");
        for (ai = addr; ai; ai = ai->ai_next) {
            char buf[128];
            const char *s = NULL;
            if (ai->ai_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
                s = evutil_inet_ntop(AF_INET, &sin->sin_addr, buf, 128);
            } else if (ai->ai_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ai->ai_addr;
                s = evutil_inet_ntop(AF_INET6, &sin6->sin6_addr, buf, 128);
            }
            if (s)
                printf("    -> %s\n", s);
        }
        evutil_freeaddrinfo(addr);
    }
    free(data->name);
    free(data);
    if (--n_pending_requests == 0)
        event_base_loopexit(base, NULL);
}

/* Take a list of domain names from the command line and resolve them in
 * parallel. */
int singleThreadDNSServer()
{
    int adressesCount = 1;
    const char* adresses[] = {"localhost"};

    base = event_base_new();
    if (!base){
        return 1;
    }
    
    evdns_base* dnsbase = evdns_base_new(base, 1);
    if (!dnsbase){
        return 2;
    }

    for (int i = 0; i < adressesCount; ++i) {
        /* Unless we specify a socktype, we'll get at least two entries for
         * each address: one for TCP and one for UDP. That's not what we
         * want. */
        evutil_addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = EVUTIL_AI_CANONNAME;
        hints.ai_socktype = SOCK_STREAM;    // тут был коммент
        hints.ai_protocol = IPPROTO_TCP;
        
        UserData* userDataPtr = (UserData*)malloc(sizeof(UserData));
        if (!userDataPtr) {
            perror("malloc");
            exit(1);
        }
        if (!(userDataPtr->name = strdup(adresses[i]))) {
            perror("strdup");
            exit(1);
        }
        userDataPtr->idx = i;
        
        ++n_pending_requests;
        evdns_getaddrinfo_request* req = evdns_getaddrinfo(dnsbase, adresses[i], NULL /* no service name given */,
                                &hints, dnsCallback, userDataPtr);
        if (req == NULL) {
            printf("    [request for %s returned immediately]\n", adresses[i]);
            /* No need to free user_data or decrement n_pending_requests; that
             * happened in the callback. */
        }
    }
    
    if (n_pending_requests > 0){
        event_base_dispatch(base);
    }
    
    evdns_base_free(dnsbase, 0);
    event_base_free(base);
    
    return 0;
}
