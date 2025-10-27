#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// #include "../../common/protocol.h"

#define SERVER_PORT 5555
#define SERVER_IP "127.0.0.1"
#define BLACKLOG 5
#define BUFFER_SIZE 1024

int main(void) {

    int sockfd, clientfd;

    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    char buffer[BUFFER_SIZE];

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {

        perror("error socket");
        close(sockfd);
        return -1;

    }


    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    memset(server_addr.sin_zero, 0, sizeof(server_addr.sin_zero));

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {

        perror("bind");
        close(sockfd);

        return -1;

    }


    if(listen(sockfd, BLACKLOG) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    printf("server listen on %s:%d\n", SERVER_IP, SERVER_PORT);

    while (1) {

        clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &addr_len);

        if (clientfd < 0) {

            perror("accept");
            continue;

        }

        printf("accepted connection from %s:%d\n",
            inet_ntoa(client_addr.sin_addr), htons(client_addr.sin_port)
        );

        int bytes_received = recv(clientfd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {

            buffer[bytes_received] = '\0';
            printf("received: %s\n", buffer);

            const char *reply = "hello from server!";

            send(clientfd, reply, strlen(reply), 0);
        }

    }

    close(sockfd);
    return 0;



}
