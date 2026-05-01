#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define BUF_SIZE 4096
#define LOG_FILE_PATH "logs/server.log"

volatile sig_atomic_t server_running = 1;
static FILE* log_file = NULL;

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
static void log_error(const char* context, int err) {
    log_msg("ERROR: %s: %s (errno=%d)", context, strerror(err), err);
}

/* Обработчик сигнала */
static void handle_signal(int sig) {
    (void)sig;
    log_msg("Received signal %d, shutting down...", sig);
    server_running = 0;
}

/* Установка обработчиков сигналов */
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

/* Рабочий поток: читает данные и отправляет обратно (эхо) */
static void* echo_client(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    char buf[BUF_SIZE];
    ssize_t n;
    int total_bytes = 0;
    
    log_msg("Client connected: fd=%d", client_fd);
    
    while ((n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t wn = send(client_fd, buf + w, n - w, 0);
            if (wn <= 0) {
                if (errno == EINTR) continue;
                log_error("send() failed for fd", errno);
                close(client_fd);
                return NULL;
            }
            w += wn;
        }
        total_bytes += n;
        log_msg("Echoed %zd bytes to fd=%d (total: %d)", n, client_fd, total_bytes);
    }
    
    if (n == 0) {
        log_msg("Client disconnected gracefully: fd=%d (total echoed: %d bytes)",
                client_fd, total_bytes);
    } else if (n < 0 && errno != EINTR) {
        log_error("recv() failed for fd", errno);
    }
    
    close(client_fd);
    log_msg("Connection closed: fd=%d", client_fd);
    return NULL;
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
    
    /* Создание сокета */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("socket() failed", errno);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Отключение привязки к адресу уже занятого порта при перезапуске */
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("setsockopt(SO_REUSEADDR) failed", errno);
        close(listen_fd);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("bind() failed", errno);
        close(listen_fd);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    /* Listen */
    if (listen(listen_fd, BACKLOG) < 0) {
        log_error("listen() failed", errno);
        close(listen_fd);
        close_log_file();
        return EXIT_FAILURE;
    }
    
    log_msg("Server listening on port %d (pid=%d)", port, (int)getpid());
    log_msg("Press Ctrl+C to stop");
    
    /* Основной цикл принятия соединений */
    while (server_running) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int rv = select(listen_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (rv > 0 && FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in peer;
            socklen_t peerlen = sizeof(peer);
            int* client_fd_ptr = malloc(sizeof(int));
            if (!client_fd_ptr) {
                log_error("malloc() failed for client_fd_ptr", errno);
                continue;
            }
            
            int client_fd = accept(listen_fd, (struct sockaddr*)&peer, &peerlen);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    free(client_fd_ptr);
                    continue;
                }
                log_error("accept() failed", errno);
                free(client_fd_ptr);
                continue;
            }
            
            *client_fd_ptr = client_fd;
            
            /* Логируем подключение */
            char host[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
            log_msg("Accepted connection from %s:%u, fd=%d", host, ntohs(peer.sin_port), client_fd);
            
            /* Создаем поток на обработку клиента */
            pthread_t tid;
            if (pthread_create(&tid, NULL, echo_client, client_fd_ptr) != 0) {
                log_error("pthread_create() failed", errno);
                close(client_fd);
                free(client_fd_ptr);
            } else {
                /* Отделяем поток, чтобы не ждать его завершения */
                pthread_detach(tid);
            }
        } else if (rv < 0 && errno != EINTR) {
            log_error("select() failed", errno);
            break;
        }
    }
    
    /* Корректное завершение */
    log_msg("Server shutting down...");
    
    if (listen_fd >= 0) {
        close(listen_fd);
        log_msg("Listening socket closed");
    }
    
    log_msg("Waiting for active connections to finish...");
    sleep(2);
    
    log_msg("Server exited.");
    close_log_file();
    return EXIT_SUCCESS;
}