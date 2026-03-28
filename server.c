#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>

// globals
#define MAX_CLIENTS 10
#define MAX_BYTES   4096

typedef struct cache cache;

cache* head;
size_t cacheSize;

// env
int PORT = 8080;
int socketID = -1;

// launch threads
pthread_t threadID[MAX_CLIENTS];
// lock wait and signal
sem_t semaphore;
// mutex lock for threads
pthread_mutex_t lock;

// using LRU cache strategy time based, Link list based
struct cache{
    char *data;
    size_t len;
    char* url;
    time_t time;
    cache* next;
};

// to find url in cahce
cache* find(char *url);
// add new element in cache
int addCache(char *data, size_t size, char *url);
// to remove element if cache overflow
void removeCache();
// thread function
void *thread_fn(void *socketNEW);


void *thread_fn(void *socketNew){
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, p);
    printf("semaphore value: %d\n",p);

    int *t = (int *) socketNew;
    int socket = *t;
    int byteSendClient, len;

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));

    bzero(buffer, MAX_BYTES);

    byteSendClient = recv(socket, buffer, MAX_BYTES, 0);

    while(byteSendClient > 0){
        len = strlen(buffer);
        if(strstr(buffer, "\r\n\r\n") == NULL){
            byteSendClient = recv(socket, buffer + len, MAX_BYTES, 0);
        }else{
            break;
        }
    }

    char *temp = (char *)malloc(strlen(buffer)*sizeof(char)+1);
     

}

int main(int argc,char *argv[]){
    int clientSocketID;
    int clientLen;
    
    struct sockaddr_in server_addr, client_addr;
    
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    // first argument is portnumber
    if(argv == 2){
        PORT = atoi(argv[1]);
    }else{
        printf("Too few arguments");
        exit(1);
    }

    printf("Starting proxy server at port: %d\n",PORT);

    socketID = socket(AF_INET, SOCK_STREAM, 0);

    if(socketID<0){
        perror("Failed to create socket");
        exit(1);
    }

    int reuse = 1;
    if(setsockopt(socketID, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse) < 0)){
        perror("setsockopt failed to reuse socket");
    }

    bzero((char*)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(socketID, (struct sockaddr*)&server_addr, sizeof(server_addr) < 0 )){
        perror("PORT is not available");
        exit(1);
    }

    printf("Binding on port: %d\n",PORT);

    int listenStatus = listen(socketID, MAX_CLIENTS);
    if(listen<0){
        perror("Error in listening");
        exit(1);
    }

    int i = 0;
    int connectedSocketID[MAX_CLIENTS];

    while(1){
        bzero((char*)&client_addr, size(client_addr));
        clientLen = sizeof(client_addr);
        clientSocketID = accept(socketID, (struct sockaddr_in*)&client_addr, (socklen_t*)&clientLen);
        if(clientSocketID<0){
            perror("Not able to connect to client");
            ewit(1);
        }else{
            connectedSocketID[i] = clientSocketID;
        }

        struct sockaddr_in* clientptr = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = clientptr->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN); 

        printf("Client is conneted to PORT:%d with IP:%s\n", ntohs(client_addr.sin_port), ip_addr);

        pthread_create(&threadID[i], NULL, thread_fn, (void *)&connectedSocketID[i]);

        i++;
    }
    close(socketID);

    return 0;
}