#include <stdexcept>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <cstdint>
#include <vector>
// libevent
#include <evhttp.h>

// пример
// https://habrahabr.ru/post/217437/

using namespace std;

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

int multithreadedServer() {
    char const serverAddress[] = "127.0.0.1";
    std::uint16_t const serverPort = 5555;
    int const threadsCount = 8;
    
    try
    {
        // коллбек запроса
        void (*receivedRequest)(evhttp_request *, void *) = [] (evhttp_request *req, void *) {
            // выходной буффер запроса
            auto *outBuf = evhttp_request_get_output_buffer(req);
            if (!outBuf){
                return;
            }
            
            // тестовая задержка
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            
            // Выходные данные
            evbuffer_add_printf(outBuf, "<html><body><center><h1>Hello Wotld!</h1></center></body></html>");
            
            // отвечаем на запрос
            evhttp_send_reply(req, HTTP_OK, "", outBuf);
        };
        
        std::exception_ptr initException;
        
        bool volatile isRunning = true;
        evutil_socket_t socket = -1;
        
        // Функция в потоке
        auto threadFunc = [&] (){
            try {
                // каждый поток имеет свой объект обработки событий, в однопотоном варианте - это event_init
                EventHandler eventBase(event_base_new(), &event_base_free);
                if (!eventBase){
                    throw std::runtime_error("Failed to create new base_event.");
                }
                
                // Создаем сервер с обработчиком событий
                ServerPtr eventHttp(evhttp_new(eventBase.get()), &evhttp_free);
                if (!eventHttp){
                    throw std::runtime_error("Failed to create new evhttp.");
                }
                
                // привязываем функцию обработчик к серверу
                evhttp_set_gencb(eventHttp.get(), receivedRequest, nullptr);
                
                // если у нас есть уже сокет или его еще нету
                if (socket == -1){
                    // связываем сервер с адресом и портом
                    auto* bindedSocket = evhttp_bind_socket_with_handle(eventHttp.get(), serverAddress, serverPort);
                    if (!bindedSocket){
                        throw std::runtime_error("Failed to bind server socket.");
                    }
                    
                    // сокет создается на основании связки
                    socket = evhttp_bound_socket_get_fd(bindedSocket);
                    if (socket == -1){
                        throw std::runtime_error("Failed to get server socket for next instance.");
                    }
                }
                else {
                    //
                    int status = evhttp_accept_socket(eventHttp.get(), socket);
                    if (status == -1){
                        throw std::runtime_error("Failed to bind server socket for new instance.");
                    }
                }
                
                for ( ; isRunning ; ) {
                    // запуск
                    event_base_loop(eventBase.get(), EVLOOP_NONBLOCK);
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            catch (...){
                initException = std::current_exception();
            }
        };
        
        
        // не дает завершиться потокам
        auto threadDeleter = [&] (std::thread *t) {
            isRunning = false;
            t->join();
            delete t;
        };
        
        // указатель на поток + функция, вызываемая при уничтожении
        typedef std::unique_ptr<std::thread, decltype(threadDeleter)> ThreadPtr;
        // пулл потоков
        typedef std::vector<ThreadPtr> ThreadPool;
        
        
        ThreadPool threads;
        threads.reserve(threadsCount);
        
        for (int i = 0 ; i < threadsCount ; ++i) {
            ThreadPtr Thread(new std::thread(threadFunc), threadDeleter);
            
            // задержка старта следующего потока
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            if (initException != std::exception_ptr()){
                isRunning = false;
                std::rethrow_exception(initException);
            }
            
            // сохраняем поток
            threads.push_back(std::move(Thread));
        }
        
        std::cout << "Press Enter fot quit." << std::endl;
        std::cin.get();
        
        isRunning = false;
    }
    catch (std::exception const &e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}


int main()
{
    int result = multithreadedServer();

    return result;
}