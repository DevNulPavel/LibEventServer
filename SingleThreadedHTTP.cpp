#include "SingleThreadedHTTP.h"
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
#include <event.h>
#include <evhttp.h>


// примеры
// https://habrahabr.ru/post/217437/
// http://incpp.blogspot.ru/2009/04/libevent.html
// https://www.ibm.com/developerworks/ru/library/l-Libevent1/

//using namespace std;

typedef std::unique_ptr<event_base, decltype(&event_base_free)>  EventHandler;
typedef std::unique_ptr<evhttp, decltype(&evhttp_free)> ServerPtr;


int simpleOneThreadServer(){
    // Инициализация цикла, только для однопоточного режима
    if (!event_init()) {
        std::cerr << "Failed to init libevent." << std::endl;
        return -1;
    }
    
    // создаем сервер
    char const* serverAddress = "127.0.0.1";
    uint16_t serverPort = 5555;
    
    // сервер + функция, вызываемая при уничтожении
    ServerPtr server(evhttp_start(serverAddress, serverPort), &evhttp_free);
    
    // не удалось создать сервер
    if (!server) {
        std::cerr << "Failed to init http server." << std::endl;
        return -1;
    }
    
    // коллбек запроса
    void (*receivedRequest)(evhttp_request*, void*) = [](evhttp_request* request, void* data){
        // выходной буффер запроса
        evbuffer* outBuf = evhttp_request_get_output_buffer(request);
        if (!outBuf){
            return;
        }
        
        // тестовая задержка
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        
        // Выходные данные
        evbuffer_add_printf(outBuf, "<html><body><center><h1>Hello Wotld! TestData!!!</h1></center></body></html>");
        
        // отвечаем на запрос
        evhttp_send_reply(request, HTTP_OK, "", outBuf);
    };
    
    // включаем обработчик вызовов
    evhttp_set_gencb(server.get(), receivedRequest, nullptr);
    
    // ошибка цикла LibEvent
    if (event_dispatch() == -1){
        std::cerr << "Failed to run messahe loop." << std::endl;
        return -1;
    }
    return 0;
}

