#include "SingleThreadedTCP.h"
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

//using namespace std;


typedef std::unique_ptr<event_base, decltype(&event_base_free)>  EventBasePtr;  // указатель на базовый цикл + функция, вызываемая при уничтожении
typedef std::unique_ptr<evconnlistener, decltype(&evconnlistener_free)> ServerListenerPtr;  // указатель на сервер + функция, вызываемая при уничтожении
typedef std::unique_ptr<std::thread, std::function<void(std::thread*)>> ThreadPtr;  // указатель на поток + функция, вызываемая при уничтожении
typedef std::vector<ThreadPtr> ThreadPool;  // пулл потоков
typedef std::function<void()> Task;
typedef std::queue<Task> TasksQueue;
typedef std::lock_guard<std::mutex> LockGuard;
typedef std::unique_lock<std::mutex> UniqueLock;

//////////////////////////////////////////////////
// Список менеджеров сервера
//////////////////////////////////////////////////
class ServerTasksHandler;
class ClientsManager;
struct ServerManagers{
    std::shared_ptr<ServerTasksHandler> tasksHandler;
    std::shared_ptr<ClientsManager> clientsManager;
};

//////////////////////////////////////////////////
// Многопоточный обработчик запросов
//////////////////////////////////////////////////
class ServerTasksHandler{
public:
    ServerTasksHandler(event_base* base, int threadsCount):
        _enabled(true),
        _base(base),
        _updateEventObject(nullptr) {
        
        createMainLoopCallbacksHandler();
        creatThreads(threadsCount);
    }
    
    ~ServerTasksHandler(){
        event_free(_updateEventObject);
    }
    
    void creatThreads(int threadsCount){
        // не дает завершиться потокам при удалении объекта
        auto threadDeleteLock = [&] (std::thread *t) {
            t->join();
            delete t;
        };
        
        // резервируем память под указатели потоков
        _threads.reserve(threadsCount);
        
        for (int i = 0; i < threadsCount ; ++i) {
            // создаем обхект потока
            auto threadPtrObject = new std::thread(std::bind(&ServerTasksHandler::threadFunction, this));
            ThreadPtr thread(threadPtrObject, threadDeleteLock);
            
            // задержка старта следующего потока
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // сохраняем поток
            _threads.push_back(std::move(thread));
        }
    }

    void createMainLoopCallbacksHandler(){
        // коллбек таймаута чтения
        auto updateEvent = [](evutil_socket_t socketFd, short flags, void* arg){
            ServerTasksHandler* thisObj = (static_cast<ServerTasksHandler*>(arg));
            TasksQueue& queue = thisObj->_mainLoopQueue;
            std::mutex& mutex = thisObj->_mutex;
            
            // сброс
            UniqueLock lock(mutex);
            if (queue.size() == 0) {
                return;
            }
            TasksQueue tasksCopy = queue;
            queue = TasksQueue();
            lock.unlock();
            
            while (tasksCopy.size() > 0) {
                Task& task = tasksCopy.front();
                task();
                tasksCopy.pop();
            }
        };
        
        _updateEventObject = event_new(_base, -1, EV_PERSIST | EV_READ | EV_WRITE, updateEvent, this);
        timeval time;
        time.tv_sec = 0;
        time.tv_usec = 50;
        event_add(_updateEventObject, &time);
    }

    void addTaskToQueue(const Task& task){
        // объект блокировки
        UniqueLock locker(_mutex);
        _threadQueue.push(task);
        _conditionVariable.notify_one();
    }
    
    void syncTaskDispatch(const Task& task){
        std::condition_variable condVar;
        std::atomic_bool complete(false);
        Task taskWrapper = [&](){
            task();
            complete = true;
            condVar.notify_all();
        };
        addTaskToQueue(task);
        
        if (complete == false) {
            std::unique_lock<std::mutex> locker(_mutex);
            condVar.wait(locker);
        }
    }

    size_t getTaskCount(){
        UniqueLock locker(_mutex);
        return _threadQueue.size();
    }
    
    bool isEmpty(){
        UniqueLock locker(_mutex);
        return _threadQueue.empty();
    }

    void callbackInMainLoop(const Task& task){
        UniqueLock locker(_mutex);
        _mainLoopQueue.push(task);
        
        event_base_loop(_base, EVLOOP_NONBLOCK);
        //event_active(_updateEventObject, EV_WRITE | EV_READ | EV_PERSIST, 0);
    }

private:
    std::atomic_bool _enabled;
    std::mutex _mutex;
    std::condition_variable _conditionVariable;
    TasksQueue _threadQueue;
    ThreadPool _threads;
    TasksQueue _mainLoopQueue;
    event_base* _base;
    event* _updateEventObject;

private:
    void threadFunction() {
        while (_enabled) {
            // объект блокировки
            std::unique_lock<std::mutex> locker(_mutex);
            
            // Ожидаем уведомления, и убедимся что это не ложное пробуждение
            // Поток должен проснуться если очередь не пустая либо он выключен
            auto conditionFunction = [&](){
                bool enable = (_threadQueue.empty() == false) || (_enabled == false);
                return enable;
            };
            _conditionVariable.wait(locker, conditionFunction);

            // выдергиваем функцию
            auto functionObject = _threadQueue.front();
            _threadQueue.pop();
            
            // Разблокируем мютекс перед вызовом функтора
            locker.unlock();
            
            // вызываем функцию
            functionObject();
        }
    }
};

//////////////////////////////////////////////////
// Потокобезопасный клиент
//////////////////////////////////////////////////
class Client: public std::enable_shared_from_this<Client> {
public:
    Client(std::mutex& mutex, bufferevent* bufferEvent, evutil_socket_t fd):
        _parentMutex(mutex),
        _bufferEvent(bufferEvent),
        _fd(fd){
    }
    
    void handleReceivedData(const ServerManagers& managers){
        // блокировка чтения из разных потоков
        UniqueLock lock(_bufferMutex);
        
        evbuffer* buf_input = bufferevent_get_input(_bufferEvent);
        //evbuffer* buf_output = bufferevent_get_output(_bufferEvent);

        // буффер для отложенной задачи
        size_t inputDataLength = evbuffer_get_length(buf_input);
        std::vector<char> dataBuffer(inputDataLength, 0);
        
        // копируем в буффер
        evbuffer_copyout(buf_input, dataBuffer.data(), inputDataLength);
        
        // прочитали/записали все данные из буффера - очистили
        evbuffer_drain(buf_input, inputDataLength);
        
        lock.unlock();
        
        //std::string inputText(dataBuffer.begin(), dataBuffer.end());
        //printf("Прочитал сервер: %s\n", inputText.c_str());
        
        //bufferevent_flush(_bufferEvent, EV_READ, bufferevent_flush_mode::BEV_FLUSH);
        
        startClientTask(managers, dataBuffer);
    }
    
    void startClientTask(const ServerManagers& managers, const std::vector<char>& dataBuffer){
        UniqueLock lock(_parentMutex);
        std::weak_ptr<Client> clientWeakPtr = shared_from_this();
        evutil_socket_t fd = clientWeakPtr.lock()->_fd;
        lock.unlock();
        
        Task threadTask = [this, dataBuffer, &managers, clientWeakPtr, fd](){
            
            // тестовая задержка
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
            // Проверка удаления
            UniqueLock lock(_parentMutex);
            if (clientWeakPtr.expired()) {
                return;
            }
            lock.unlock();
            
            // отправка в фоновом потоке
            sendServerAnswer(dataBuffer);
            
            // коллбек в главном потоке после завершения
            /*managers.tasksHandler->callbackInMainLoop([dataBuffer, clientWeakPtr](){
                //std::string inputText(dataBuffer.begin(), dataBuffer.end());
                //printf("Обработал сервер: %s\n", inputText.c_str());
                
                if (clientWeakPtr.expired()) {
                    return;
                }
                clientWeakPtr.lock()->sendServerAnswer(dataBuffer);
            });*/
        };
        managers.tasksHandler->addTaskToQueue(threadTask);
    }
    
    void sendServerAnswer(const std::vector<char>& data){
        // блокировка записей из разных потоков + блокировка удаления у родителя
        LockGuard lock(_bufferMutex);
    
        //evbuffer* buf_input = bufferevent_get_input(_bufferEvent);
        evbuffer* buf_output = bufferevent_get_output(_bufferEvent);
    
        // Данные просто копируются из буфера ввода в буфер вывода
        evbuffer_add_printf(buf_output, "Server handled: ");
        // копируем входной буффер
        evbuffer_add(buf_output, data.data(), data.size());
        
        // прочитали/записали все данные из буффера - очистили
        evbuffer_drain(buf_output, data.size());
        
        //bufferevent_flush(_bufferEvent, EV_WRITE, bufferevent_flush_mode::BEV_FLUSH);
    }
    
public:
    std::mutex _bufferMutex;
    std::mutex& _parentMutex;
    bufferevent* _bufferEvent;
    evutil_socket_t _fd;
};

typedef std::shared_ptr<Client> ClientPtr;

//////////////////////////////////////////////////
// Потокобезопасный менеджер клиентов
//////////////////////////////////////////////////
class ClientsManager{
public:
    ClientPtr getClient(bufferevent* buffer){
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_clients.count(buffer)) {
            ClientPtr client = _clients[buffer];
            return client;
        }
        return nullptr;
    }
    
    ClientPtr addClient(bufferevent* buffer, evutil_socket_t fd){
        LockGuard lock(_mutex);
        
        ClientPtr client = nullptr;
        if (_clients.count(buffer) == 0) {
            client = std::make_shared<Client>(_mutex, buffer, fd);
            _clients[buffer] = client;
        }else{
            client = _clients[buffer];
        }
        
        return client;
    }
    
    void removeClient(bufferevent* buffer){
        LockGuard lock(_mutex);
        if (_clients.count(buffer)) {
            ClientPtr client = _clients[buffer];
            
            // чтобы не удалился при чтении и записи
            UniqueLock lock(client->_bufferMutex);
            _clients.erase(buffer);
            lock.unlock();
        }
    }
    
private:
    std::mutex _mutex;
    std::unordered_map<bufferevent*, ClientPtr> _clients;
    
private:
    
};


//////////////////////////////////////////////////
// TCP Server
//////////////////////////////////////////////////
int tcpServer() {
    //////////////////////////////////////////////////
    // Callbacks
    //////////////////////////////////////////////////
    // обработка принятия соединения
    auto accept_connection_cb = [](evconnlistener* listener,
                                   evutil_socket_t fd, sockaddr* addr, int sock_len,
                                   void* arg) {

        ServerManagers& managers = *(static_cast<ServerManagers*>(arg));
        
        // обработчик ивентов базовый
        event_base* base = evconnlistener_get_base(listener);
        
        // При обработке запроса нового соединения необходимо создать для него объект bufferevent
        int bufferEventFlags = BEV_OPT_CLOSE_ON_FREE /*| BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS*/;
        bufferevent* buf_ev = bufferevent_socket_new(base, fd, bufferEventFlags);
        if (buf_ev == nullptr) {
            fprintf(stderr, "Ошибка при создании объекта bufferevent.\n");
            return;
        }
        
        // создание клиента
        ClientPtr client = managers.clientsManager->addClient(buf_ev, fd);
        
        // Функция обратного вызова для события: данные готовы для чтения в buf_ev
        auto echo_read_cb = [](bufferevent* buf_ev, void *arg) {
            ServerManagers& managers = *(static_cast<ServerManagers*>(arg));
            
            ClientPtr client = managers.clientsManager->getClient(buf_ev);
            if (client) {
                client->handleReceivedData(managers);
            }else{
                perror("Не нашли клиента\n");
            }
        };
        
        // Функция обратного вызова для события: данные готовы для записи в buf_ev
        auto echo_write_cb = [](bufferevent* buf_ev, void *arg) {
            //ClientsManager& clientsManager = *(reinterpret_cast<ClientsManager*>(arg));
        
            //std::cout << "Write callback" << std::endl;
        };
        
        // коллбек обработки ивента
        auto echo_event_cb = [](bufferevent* buf_ev, short events, void *arg){
            ServerManagers& managers = *(static_cast<ServerManagers*>(arg));
        
            if(events & BEV_EVENT_READING){
                perror("Ошибка во время чтения bufferevent\n");
            }
            if(events & BEV_EVENT_WRITING){
                perror("Ошибка во время записи bufferevent\n");
            }
            if(events & BEV_EVENT_ERROR){
                perror("Ошибка объекта bufferevent\n");
            }
            if(events & BEV_EVENT_TIMEOUT){
                // пишем в буффер об долгом пинге
                //evbuffer* buf_output = bufferevent_get_output(buf_ev);
                //evbuffer_add_printf(buf_output, "Kick by timeout\n");

                // уничтожаем объект буффер
                if (buf_ev) {
                    managers.clientsManager->removeClient(buf_ev);
                
                    bufferevent_free(buf_ev);
                    buf_ev = nullptr;
                }
                perror("Таймаут bufferevent\n");
            }
            if(events & BEV_EVENT_CONNECTED){
                perror("Соединение в bufferevent");
            }
            if(events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)){
                // уничтожаем объект буффер
                if (buf_ev) {
                    managers.clientsManager->removeClient(buf_ev);
                    
                    bufferevent_free(buf_ev);
                    buf_ev = nullptr;
                }
            }
        };
        
        // коллбеки обработи
        bufferevent_setcb(buf_ev, echo_read_cb, echo_write_cb, echo_event_cb, &managers);
        bufferevent_enable(buf_ev, (EV_READ | EV_WRITE));
        // таймауты
        timeval readWriteTimeout;
        readWriteTimeout.tv_sec = 600;
        readWriteTimeout.tv_usec = 0;
        bufferevent_set_timeouts(buf_ev, &readWriteTimeout, &readWriteTimeout);
    };
    
    // ошибка в принятии соединения
    auto accept_error_cb = []( struct evconnlistener *listener, void *arg){
        struct event_base *base = evconnlistener_get_base( listener );
        int error = EVUTIL_SOCKET_ERROR();
        fprintf(stderr, "Ошибка %d (%s) в мониторе соединений. Завершение работы.\n",
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
    EventBasePtr base(event_base_new(), &event_base_free);
    if(!base){
        fprintf(stderr, "Ошибка при создании объекта event_base.\n" );
        return -1;
    }
    
    // инициализация многопоточности ??
    evthread_make_base_notifiable(base.get());
    
    // коллбек-ивент для периодических событий
    timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    event* updateEventObject = event_new(base.get(), fileno(stdin), EV_TIMEOUT | EV_PERSIST, updateEvent, NULL);
    event_add(updateEventObject, &tv);
    
    // адрес
    const int port = 5555;
    sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;    /* работа с доменом IP-адресов */
    sin.sin_addr.s_addr = htonl(INADDR_ANY);  /* принимать запросы с любых адресов */
    sin.sin_port = htons(port);
    
    // Многопоточный обработчик задач + Менеджер клиентов
    std::shared_ptr<ServerTasksHandler> tasksHandler = std::make_shared<ServerTasksHandler>(base.get(), 4);
    std::shared_ptr<ClientsManager> clientsManager = std::make_shared<ClientsManager>();
    
    // менеджеры
    std::shared_ptr<ServerManagers> managers = std::make_shared<ServerManagers>();
    managers->tasksHandler = tasksHandler;
    managers->clientsManager = clientsManager;
    
    // лиснер
    evconnlistener* listenerPtr = evconnlistener_new_bind(base.get(), accept_connection_cb, managers.get(),
                                                          (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_THREADSAFE),
                                                          -1, (sockaddr*)&sin, sizeof(sin));
    ServerListenerPtr listener(listenerPtr, &evconnlistener_free);
    // проверка ошибки создание листнера
    if(!listener){
        perror( "Ошибка при создании объекта evconnlistener" );
        return -1;
    }
    
    // обработчик ошибки
    evconnlistener_set_error_cb(listener.get(), accept_error_cb );
    
    // запуск обработки событий
    event_base_dispatch(base.get());
    
    // удаляем менеджеры
    tasksHandler = nullptr;
    clientsManager = nullptr;
    managers = nullptr;
    
    // delete all
    event_free(updateEventObject);
    listener = nullptr;
    base = nullptr;
    
    return 0;
}
