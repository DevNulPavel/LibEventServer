#include "MultiThreadedTCP.h"
// std
#include <stdexcept>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <cstdint>
#include <vector>
#include <memory>
#include <queue>
#include <string>
#include <list>
#include <unordered_map>
// libevent
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event.h>
#include <evhttp.h>

// примеры
// https://habrahabr.ru/post/217437/
// http://incpp.blogspot.ru/2009/04/libevent.html
// https://www.ibm.com/developerworks/ru/library/l-Libevent1/
// работа с буфферами - http://www.wangafu.net/~nickm/libevent-book/Ref6_bufferevent.html
//                      http://www.wangafu.net/~nickm/libevent-book/Ref6a_advanced_bufferevents.html

//using namespace std;

typedef std::shared_ptr<event_base>  EventBasePtr;
typedef std::unique_ptr<evconnlistener, decltype(&evconnlistener_free)> ServerListenerPtr;  // указатель на сервер + функция, вызываемая при уничтожении
typedef std::unique_ptr<std::thread, std::function<void(std::thread*)>> ThreadPtr;  // указатель на поток + функция, вызываемая при уничтожении
typedef std::vector<ThreadPtr> ThreadPool;  // пулл потоков
typedef std::function<void()> Task;
typedef std::queue<Task> TasksQueue;
typedef std::lock_guard<std::mutex> LockGuard;
typedef std::unique_lock<std::mutex> UniqueLock;


//////////////////////////////////////////////////
// TCP Server
//////////////////////////////////////////////////
int multiThreadedTcpServerFilter() {
    std::uint16_t const serverPort = 5555;
    int const threadsCount = 16;
    
    
    std::mutex mutex;
    std::condition_variable condVar;
    std::atomic_bool isActive(true);
    std::vector<EventBasePtr> events;
    std::atomic<evutil_socket_t> socket(-1);
    
    // Функция в потоке
    auto threadFunc = [&] (){
        //////////////////////////////////////////////////
        // Callbacks
        //////////////////////////////////////////////////
        // обработка принятия соединения
        auto accept_connection_cb = [](evconnlistener* listener,
                                       evutil_socket_t fd, sockaddr* addr, int sock_len,
                                       void* arg) {
            // обработчик ивентов базовый
            event_base* base = evconnlistener_get_base(listener);
            
            // При обработке запроса нового соединения необходимо создать для него объект bufferevent
            bufferevent* buf_ev_classic = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE /*| BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS*/);
            if (buf_ev_classic == nullptr) {
                std::cout << "Ошибка при создании объекта bufferevent." << std::endl;
                return;
            }
            
            // размер информации о размере
            typedef char DataSizeType;
            
            // обертка-фильтр
            auto inputFilter = [](evbuffer *src, evbuffer *dst, ev_ssize_t dst_limit, bufferevent_flush_mode mode, void *ctx)-> bufferevent_filter_result {
                // Читаем данные
                size_t receivedDataSize = evbuffer_get_length(src);
                
                // проверка на большие размеры буффера (10Mb)
                /*if (receivedDataSize < 1024 * 1024 * 10) {
                    return bufferevent_filter_result::BEV_NEED_MORE;
                }*/
                
                // если мало данных о размере - ждем
                if (receivedDataSize < sizeof(DataSizeType)) {
                     return bufferevent_filter_result::BEV_NEED_MORE;
                }
                
                int64_t dataSize = 0;
                evbuffer_copyout(src, &dataSize, sizeof(DataSizeType));
                
                // если мало данных в буффере - ждем еще
                if (receivedDataSize < (sizeof(DataSizeType) + dataSize)) {
                    return bufferevent_filter_result::BEV_NEED_MORE;
                }
                
                // удаляем данные о размере из начала
                evbuffer_drain(src, sizeof(DataSizeType));
                // TODO: копируем в выходной буффер (или перемещаем?????)
                evbuffer_add_buffer(dst, src);
                
                return bufferevent_filter_result::BEV_OK;
            };
            auto outFilter = [](evbuffer *src, evbuffer *dst, ev_ssize_t dst_limit, bufferevent_flush_mode mode, void *ctx)-> bufferevent_filter_result {
            
                // добавление информации о размере в начало
                DataSizeType dataSize = evbuffer_get_length(src);
                // TODO: копируем в выходной буффер (или перемещаем?????)
                evbuffer_add(dst, &dataSize, sizeof(DataSizeType));
                evbuffer_add_buffer(dst, src);
                
                return bufferevent_filter_result::BEV_OK;
            };
            auto filterDestroyCallback = [](void*){
            };
            bufferevent* buf_ev = bufferevent_filter_new(buf_ev_classic, inputFilter, outFilter, 0, filterDestroyCallback, nullptr);
            if (buf_ev_classic == nullptr) {
                std::cout << "Ошибка при создании ФИЛЬТРУЮЩЕГО объекта bufferevent." << std::endl;
                return;
            }
            // Функция обратного вызова для события: данные готовы для чтения в buf_ev
            auto echo_read_cb = [](bufferevent* buf_ev, void *arg) {
                evbuffer* buf_input = bufferevent_get_input(buf_ev);
                evbuffer* buf_output = bufferevent_get_output(buf_ev);
                
                // новый размер буффера
                size_t receivedDataSize = evbuffer_get_length(buf_input);
                
                // временная область с данными
                std::vector<char> dataBuffer;
                dataBuffer.resize(receivedDataSize);
                
                // копируем
                evbuffer_copyout(buf_input, dataBuffer.data(), receivedDataSize);
                
                // чистим входной буффер
                evbuffer_drain(buf_input, receivedDataSize);
                
                
                // искусственная задержка
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                
                
                // выводим данные
                size_t outSize = evbuffer_get_length(buf_output);
                
                evbuffer_add_printf(buf_output, "Server handled: ");
                evbuffer_add(buf_output, dataBuffer.data(), dataBuffer.size());
                
                // чистим выходной буффер
                evbuffer_drain(buf_output, outSize);
                
            };
            
            // Функция обратного вызова для события: данные готовы для записи в buf_ev
            auto echo_write_cb = [](bufferevent* buf_ev, void *arg) {
                //std::cout << "Write callback" << std::endl;
            };
            
            // коллбек обработки ивента
            auto echo_event_cb = [](bufferevent* buf_ev, short events, void *arg){
                if(events & BEV_EVENT_READING){
                    std::cout << "Ошибка во время чтения bufferevent" << std::endl;
                }
                if(events & BEV_EVENT_WRITING){
                    std::cout << "Ошибка во время записи bufferevent" << std::endl;
                }
                if(events & BEV_EVENT_ERROR){
                    std::cout << "Ошибка объекта bufferevent" << std::endl;
                }
                if(events & BEV_EVENT_TIMEOUT){
                    // пишем в буффер об долгом пинге
                    //evbuffer* buf_output = bufferevent_get_output(buf_ev);
                    //evbuffer_add_printf(buf_output, "Kick by timeout\n");
                    // уничтожаем объект буффер
                    if (buf_ev) {
                        bufferevent_free(buf_ev);
                        buf_ev = nullptr;
                    }
                    std::cout << "Таймаут bufferevent\n" << std::endl;
                }
                if(events & BEV_EVENT_CONNECTED){
                    std::cout << "Соединение в bufferevent" << std::endl;
                }
                if(events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)){
                    // уничтожаем объект буффер
                    if (buf_ev) {
                        bufferevent_free(buf_ev);
                        buf_ev = nullptr;
                    }
                }
            };
            
            // коллбеки обработи
            bufferevent_setcb(buf_ev, echo_read_cb, echo_write_cb, echo_event_cb, nullptr);
            bufferevent_enable(buf_ev, (EV_READ | EV_WRITE));
            // размеры буффера для вызова коллбеков
            bufferevent_setwatermark(buf_ev, EV_READ, 2, 0);   // 2+
            bufferevent_setwatermark(buf_ev, EV_WRITE, 2, 0);   // 20+
            // таймауты
            timeval readWriteTimeout;
            readWriteTimeout.tv_sec = 600;
            readWriteTimeout.tv_usec = 0;
            bufferevent_set_timeouts(buf_ev, &readWriteTimeout, &readWriteTimeout);
        };
        
        //////////////////////////////////////////////////
        // Setup
        //////////////////////////////////////////////////
        // каждый поток имеет свой объект обработки событий, в однопотоном варианте - это event_init
        EventBasePtr eventBase(event_base_new(), &event_base_free);
        if (!eventBase){
            std::cout << "Ошибка при создании объекта event_base." << std::endl;
            return;
        }
        
        mutex.lock();
        events.push_back(eventBase);
        mutex.unlock();
        
        // Будущий объект listener
        evconnlistener* listenerPtr = nullptr;
        
        // если у нас есть уже сокет или его еще нету
        if (socket == -1){
            // адрес
            sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;    /* работа с доменом IP-адресов */
            sin.sin_addr.s_addr = htonl(INADDR_ANY);  /* принимать запросы с любых адресов */
            sin.sin_port = htons(serverPort);
            
            // Создаем сервер с обработчиком событий
            listenerPtr = evconnlistener_new_bind(eventBase.get(), accept_connection_cb, nullptr,
                                                                  (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_THREADSAFE),
                                                                  -1, (sockaddr*)&sin, sizeof(sin));
            if (!listenerPtr){
                std::cout << "Не получилось создать listener" << std::endl;
                return;
            }
        
            // сокет создается на основании связки
            socket = evconnlistener_get_fd(listenerPtr);
            if (socket == -1){
                std::cout << "Не получилось получить объект сокет из listener" << std::endl;
            }
        } else {
            // Создаем сервер с обработчиком событий
            evconnlistener* listenerPtr = evconnlistener_new(eventBase.get(), accept_connection_cb, nullptr,
                                                             (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_THREADSAFE),
                                                             -1, socket);
            if (!listenerPtr){
                std::cout << "Не получилось создать listener с сокетом" << std::endl;
                return;
            }
        }
        
        // листенер
        ServerListenerPtr listener(listenerPtr, &evconnlistener_free);
        
        // запуск (неблокирующий)
//        event_base_loop(eventBase.get(), EVLOOP_NONBLOCK);
        
        // запуск цикла - блокирующий
        event_base_dispatch(eventBase.get());
        
        std::cout << "Выход из цикла обработки" << std::endl;
    };
    
    // пулл потоков
    ThreadPool threads;
    threads.reserve(threadsCount);
    
    // не дает завершиться потокам
    auto threadDeleter = [&] (std::thread *t) {
        t->join();
        delete t;
    };
    
    events.reserve(threadsCount);
    
    for (int i = 0 ; i < threadsCount ; ++i) {
        ThreadPtr Thread(new std::thread(threadFunc), threadDeleter);
        
        // задержка старта следующего потока
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // сохраняем поток
        threads.push_back(std::move(Thread));
    }
    
    // ожидаем нажатия для завершения
    std::cout << "Write \"Exit\" fot quit." << std::endl;
    std::string text;
    std::cin >> text;
    while (text.find("Exit") == std::string::npos) {
        text.clear();
        std::cin >> text;
    }
    std::cout << "Quit in progress." << std::endl;
    
    // завершение
    timeval timeVal;
    timeVal.tv_sec = 0;
    timeVal.tv_usec = 500;
    for (const EventBasePtr& event: events) {
        event_base_loopexit(event.get(), &timeVal);
        //event_base_loopbreak(event.get());
    }
    events.clear();
    threads.clear();
    
    std::cout << "Quit complete." << std::endl;
    
    return 0;
}

