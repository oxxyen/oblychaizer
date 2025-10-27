#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include "config/serv_config.h"

volatile sig_atomic_t stop_server = 0;
int server_fd;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int active_clients = 0;

void *client_handler(void *arg);
void handle_sigint(int sig);

int main() {
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    signal(SIGINT, handle_sigint); // Обработка Ctrl+C

    // Создание сокета
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Настройка адреса сервера
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

    // Привязка сокета
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Прослушивание порта
    if (listen(server_fd, MAX_PENDING) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", SERVER_PORT);

    while (!stop_server) {
        int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) {
            if (stop_server) break; // Завершение работы
            perror("Accept failed");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (active_clients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&clients_mutex);
            printf("Max clients reached. Connection refused.\n");
            close(new_socket);
            continue;
        }
        active_clients++;
        pthread_mutex_unlock(&clients_mutex);

        printf("New connection from %s:%d. Active clients: %d\n",
               inet_ntoa(address.sin_addr), ntohs(address.sin_port), active_clients);

        pthread_t thread_id;
        int *pclient = malloc(sizeof(int));
        *pclient = new_socket;

        if (pthread_create(&thread_id, NULL, client_handler, pclient) != 0) {
            perror("Failed to create thread");
            free(pclient);
            close(new_socket);
            pthread_mutex_lock(&clients_mutex);
            active_clients--;
            pthread_mutex_unlock(&clients_mutex);
        } else {
            pthread_detach(thread_id); // Автоматически освобождает ресурсы по завершению
        }
    }

    printf("Server shutting down...\n");
    close(server_fd);
    return 0;
}

void *client_handler(void *arg) {
    int client_socket = *((int *)arg);
    free(arg);

    char buffer[BUFFER_SIZE];
    int bytes_read;

    while ((bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        printf("Received from client: %s", buffer);

        send(client_socket, buffer, bytes_read, 0);
    }

    printf("Client disconnected. Active clients before decrement: %d\n", active_clients);
    close(client_socket);

    pthread_mutex_lock(&clients_mutex);
    active_clients--;
    pthread_mutex_unlock(&clients_mutex);

    return NULL;
}

void handle_sigint(int sig) {
    (void)sig;
    stop_server = 1;
    close(server_fd);
    printf("\nSIGINT received, stopping server...\n");
}
