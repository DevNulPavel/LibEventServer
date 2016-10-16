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


int tcpServer() {
    //////////////////////////////////////////////////
    // Callbacks
    //////////////////////////////////////////////////
    // обработка принятия соединения
    auto accept_connection_cb = [](evconnlistener *listener,
                                   evutil_socket_t fd, sockaddr *addr, int sock_len,
                                   void* arg) {
        
        // обработчик ивентов базовый
        event_base* base = evconnlistener_get_base(listener);
        
        // При обработке запроса нового соединения необходимо создать для него объект bufferevent
        int bufferEventFlags = BEV_OPT_CLOSE_ON_FREE /*| BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS | BEV_OPT_THREADSAFE*/;
        bufferevent* buf_ev = bufferevent_socket_new(base, fd, bufferEventFlags);
        if (buf_ev == nullptr) {
            fprintf(stderr, "Ошибка при создании объекта bufferevent.\n");
            return;
        }
        
        // Функция обратного вызова для события: данные готовы для чтения в buf_ev
        auto echo_read_cb = [](bufferevent* buf_ev, void *arg) {
            
            evbuffer* buf_input = bufferevent_get_input(buf_ev);
            evbuffer* buf_output = bufferevent_get_output(buf_ev);
            
            // тестовая задержка
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
            // Данные просто копируются из буфера ввода в буфер вывода
            evbuffer_add_printf(buf_output, "You write: ");
            evbuffer_add_buffer(buf_output, buf_input);
        };
        
        // Функция обратного вызова для события: данные готовы для записи в buf_ev
        auto echo_write_cb = [](bufferevent* buf_ev, void *arg) {
            //std::cout << "Write callback" << std::endl;
        };
        
        // коллбек обработки ивента
        auto echo_event_cb = [](bufferevent* buf_ev, short events, void *arg){
            if(events & BEV_EVENT_READING){
                perror("Ошибка во время чтения bufferevent");
            }
            if(events & BEV_EVENT_WRITING){
                perror("Ошибка во время записи bufferevent");
            }
            if(events & BEV_EVENT_ERROR){
                perror("Ошибка объекта bufferevent");
            }
            if(events & BEV_EVENT_TIMEOUT){
                perror("Таймаут bufferevent");
            }
            if(events & BEV_EVENT_CONNECTED){
                perror("Соединение в bufferevent");
            }
            if(events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)){
                bufferevent_free(buf_ev);
            }
        };
        
        // коллбеки обработи
        bufferevent_setcb(buf_ev, echo_read_cb, echo_write_cb, echo_event_cb, NULL);
        bufferevent_enable(buf_ev, (EV_READ | EV_WRITE));
    };

    // ошибка в принятии соединения
    auto accept_error_cb = []( struct evconnlistener *listener, void *arg){
        struct event_base *base = evconnlistener_get_base( listener );
        int error = EVUTIL_SOCKET_ERROR();
        fprintf( stderr, "Ошибка %d (%s) в мониторе соединений. Завершение работы.\n",
                error, evutil_socket_error_to_string( error ) );
        event_base_loopexit(base, NULL);
    };
    
    // коллбек таймаута чтения
    auto updateEvent = [](evutil_socket_t socketFd, short event, void* arg){
        const char* data = reinterpret_cast<const char*>(arg);
        printf( "Сокет %d - активные события: %s%s%s%s; %s\n", (int)socketFd,
               (event & EV_TIMEOUT) ? " таймаут" : "",
               (event & EV_READ)    ? " чтение"  : "",
               (event & EV_WRITE)   ? " запись"  : "",
               (event & EV_SIGNAL)  ? " сигнал"  : "", data);
        /*
        if (event & EV_TIMEOUT) {
            std::cout << "Таймаут события" << std::endl;
        } else if (event & EV_READ) {
            std::cout << "Таймаут EV_READ" << std::endl;
        } else if (event & EV_PERSIST) {
            std::cout << "Таймаут PERSIST" << std::endl;
        }
        */
    };

    //////////////////////////////////////////////////
    // setup
    //////////////////////////////////////////////////
    // обработчик событий
    event_base* base = event_base_new();
    if(!base){
        fprintf(stderr, "Ошибка при создании объекта event_base.\n" );
        return -1;
    }
    
    // коллбек-ивент для периодических событий
    timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    event* updateEventObject = event_new(base, fileno(stdin), EV_TIMEOUT | EV_PERSIST, updateEvent, NULL);
    event_add(updateEventObject, &tv);
    
    
    // адрес
    const int port = 5555;
    sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;    /* работа с доменом IP-адресов */
    sin.sin_addr.s_addr = htonl(INADDR_ANY);  /* принимать запросы с любых адресов */
    sin.sin_port = htons(port);
    
    // лиснер
    evconnlistener* listener = evconnlistener_new_bind(base, accept_connection_cb, NULL,
                                       (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE),
                                       -1, (sockaddr*)&sin, sizeof(sin));
    // проверка ошибки создание листнера
    if(!listener){
        perror( "Ошибка при создании объекта evconnlistener" );
        return -1;
    }
    
    // обработчик ошибки
    evconnlistener_set_error_cb(listener, accept_error_cb );
    
    // запуск обработки событий
    event_base_dispatch(base);
    
    // delete all
    event_free(updateEventObject);
    evconnlistener_free(listener);
    event_base_free(base);
    
    return 0;
}


int main()
{
    int result = tcpServer();

    return result;
}