#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define BUF_SIZE 4096
#define MAX_EVENTS 1024
#define THREAD_POOL_SIZE 4
#define LOG_FILE_PATH "logs/server.log"

volatile sig_atomic_t server_running = 1;
static int epoll_fd;
static int listen_fd;
static pthread_t worker_threads[THREAD_POOL_SIZE];
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static FILE* log_file = NULL;

typedef struct task_t {
    int client_fd;
    struct task_t* next;
} task_t;

static task_t* task_queue = NULL;
static task_t* task_queue_tail = NULL;

/* Инициализация файла лога */
int init_log_file(void) {
    log_file = fopen(LOG_FILE_PATH, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file: %s (errno=%d: %s)\n", 
                LOG_FILE_PATH, errno, strerror(errno));
        return -1;
    }
    
    setbuf(log_file, NULL);
    return 0;
}

/* Закрытие файла лога */
void close_log_file(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

/* Функция логирования */
static void log_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t seconds = ts.tv_sec;
    struct tm tm;
    localtime_r(&seconds, &tm);
    
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
    
    char message[1024];
    vsnprintf(message, sizeof(message), fmt, ap);
    
    fprintf(stderr, "[%s] %s\n", time_str, message);
    
    if (log_file) {
        fprintf(log_file, "[%s] %s\n", time_str, message);
    }
    
    va_end(ap);
}

/* Логирование ошибки с errno */
static void log_error(const char *context, int err) {
    log_msg("ERROR: %s: %s (errno=%d)", context, strerror(err), err);
}

/* Обработчик сигнала */
static void handle_signal(int sig) {
    (void)sig;
    log_msg("Received signal %d, shutting down...", sig);
    server_running = 0;
}

/* Создание и настройка серверного сокета */
static int setup_server_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        log_error("socket() failed", errno);
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("socket() failed", errno);
        close(sock);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("bind() failed", errno);
        close(sock);
        return -1;
    }
    
    if (listen(sock, BACKLOG) < 0) {
        log_error("listen() failed", errno);
        close(sock);
        return -1;
    }
    
    return sock;
}

/* Добавление задачи в очередь thread pool */
static void add_task(int client_fd) {
    task_t* task = malloc(sizeof(task_t));
    if (!task) {
        log_error("malloc() failed", errno);
        close(client_fd);
        return;
    }
    
    task->client_fd = client_fd;
    task->next = NULL;
    
    pthread_mutex_lock(&queue_mutex);
    if (task_queue_tail) {
        task_queue_tail->next = task;
        task_queue_tail = task;
    } else {
        task_queue = task_queue_tail = task;
    }
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

/* Получение задачи из очереди */
static int get_task(void) {
    pthread_mutex_lock(&queue_mutex);
    while (task_queue == NULL && server_running) {
        pthread_cond_wait(&queue_cond, &queue_mutex);
    }
    
    if (!server_running) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }
    
    task_t* task = task_queue;
    task_queue = task->next;
    if (!task_queue) {
        task_queue_tail = NULL;
    }
    
    int client_fd = task->client_fd;
    free(task);
    pthread_mutex_unlock(&queue_mutex);
    
    return client_fd;
}

/* Обработка клиента (эхо) */
static void handle_client(int client_fd) {
    char buf[BUF_SIZE];
    ssize_t n;
    int total_bytes = 0;
    
    log_msg("Client connected: fd=%d", client_fd);
    
    while (1) {
        n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = send(client_fd, buf + sent, n - sent, 0);
                if (w <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* Временно нет места для отправки */
                        break;
                    }
                    if (errno != EINTR) {
                        log_msg("Error sending to fd=%d: %s", client_fd, strerror(errno));
                    }
                    close(client_fd);
                    return;
                }
                sent += w;
            }
            total_bytes += n;
        } else if (n == 0) {
            /* Клиент закрыл соединение */
            log_msg("Client disconnected gracefully: fd=%d (total echoed: %d bytes)", 
                    client_fd, total_bytes);
            close(client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno != EINTR) {
                log_msg("Error reading from fd=%d: %s", client_fd, strerror(errno));
                close(client_fd);
                return;
            }
        }
    }
    
    log_msg("Connection processed: fd=%d (total echoed: %d bytes)", client_fd, total_bytes);
    
    /* Возвращаем сокет в epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = client_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev) < 0) {
        log_error("epoll_ctl(MOD) failed", errno);
        close(client_fd);
    }
}

/* Рабочий поток из пула */
static void* worker_thread(void* arg) {
    (void)arg;
    while (server_running) {
        int client_fd = get_task();
        if (client_fd >= 0) {
            handle_client(client_fd);
        }
    }
    return NULL;
}

int setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        log_error("sigaction(SIGINT) failed", errno);
        return -1;
    }
    
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        log_error("sigaction(SIGTERM) failed", errno);
        return -1;
    }
    
    /* Игнорируем SIGPIPE */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        log_error("sigaction(SIGPIPE) failed", errno);
        return -1;
    }
    
    return 0;
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }
    
    if (init_log_file() < 0) {
        return EXIT_FAILURE;
    }
    
    /* Установка обработчиков сигналов */
    if (setup_signal_handlers() < 0) {
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Создание серверного сокета */
    listen_fd = setup_server_socket(port);
    if (listen_fd < 0) {
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Создание epoll */
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_error("epoll_create1() failed", errno);
        close(listen_fd);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Добавление слушающего сокета в epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        log_error("epoll_ctl(ADD) failed", errno);
        close(listen_fd);
        close(epoll_fd);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Запуск thread pool */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_thread, NULL) != 0) {
            log_error("pthread_create() failed", errno);
            server_running = 0;
            break;
        }
    }
    
    log_msg("Server listening on port %d (pid=%d)", port, (int)getpid());
    log_msg("Epoll (ET) + Thread Pool (%d workers)", THREAD_POOL_SIZE);
    log_msg("Press Ctrl+C to stop");
    
    /* Основной цикл epoll */
    struct epoll_event events[MAX_EVENTS];
    int client_count = 0;
    
    while (server_running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait() failed", errno);
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            
            if (fd == listen_fd) {
                /* Принимаем новых клиентов */
                while (1) {
                    struct sockaddr_in peer;
                    socklen_t peerlen = sizeof(peer);
                    
                    int client_fd = accept4(listen_fd, (struct sockaddr*)&peer,
                                            &peerlen, SOCK_NONBLOCK);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        log_error("accept4() failed", errno);
                        break;
                    }
                    
                    /* Добавляем клиента в epoll */
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        log_error("epoll_ctl(ADD client) failed", errno);
                        close(client_fd);
                        continue;
                    }
                    
                    client_count++;
                    
                    /* Логируем подключение */
                    char host[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
                    log_msg("Accepted connection #%d from %s:%u, fd=%d", 
                            client_count, host, ntohs(peer.sin_port), client_fd);
                }
            } else {
                add_task(fd);
            }
        }
    }
    
    /* Корректное завершение */
    log_msg("Server shutting down...");
    
    if (listen_fd >= 0) {
        close(listen_fd);
        log_msg("Listening socket closed");
    }
    
    if (epoll_fd >= 0) {
        close(epoll_fd);
        log_msg("Epoll fd closed");
    }
    
    pthread_mutex_lock(&queue_mutex);
    server_running = 0;
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    
    log_msg("Waiting for active connections to finish...");
    
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (worker_threads[i]) {
            pthread_join(worker_threads[i], NULL);
        }
    }
    
    log_msg("Total clients served: %d", client_count);
    log_msg("Server exited.");
    
    close_log_file();
    return EXIT_SUCCESS;
}