#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

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

int connectRemoteServer(char *hostaddr, int portNumber){
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(remoteSocket < 0 ){
        printf("Error in creating socket");
        return -1;
    }
    struct hostent *host = gethostbyname(hostaddr);
    if(host == NULL){
        fprintf(stderr, "No such host exits");
        return -1;
    }
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portNumber);

    bcopy((char*)host->h_addr_list[0], (char*)&server_addr.sin_addr.s_addr, host->h_length);

    int connectStatus = connect(remoteSocket, (struct sockaddr*)&server_addr, (size_t)sizeof(server_addr));
    if(connectStatus < 0){
        fprintf(stderr, "Error in connecting to remote server");
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq){
    char *buffer = (char *)malloc(sizeof(char)*MAX_BYTES);
    strcpy(buffer,"GET ");
    strcat(buffer,request->path);
    strcat(buffer," ");
    strcat(buffer, request->version);
    strcat(buffer, "\r\n");

    size_t len = strlen(buffer);

    if(ParsedHeader_set(request, "Connection", "close") < 0 ){
        printf("Set header key is not working\n");
    }

    if(ParsedHeader_get(request,"Host") == NULL){
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            printf("Set Host header key is not working\n");
        }
    }

    if(ParsedRequest_unparse_headers(request, buffer + len, (size_t)MAX_BYTES - len) < 0 ){
        printf("Unparse failed\n");
    }

    int server_port = 80;
    if(request->port != NULL){
        server_port = atoi(request->port);
    }

    int remoteSocketID = connectRemoteServer(request->host, server_port);

    if(remoteSocketID < 0){
        return -1;
    }

    int byteSend = send(remoteSocketID, buffer, strlen(buffer), 0);
    bzero(buffer, MAX_BYTES);

    byteSend = recv(remoteSocketID, buffer, MAX_BYTES-1, 0);
    char *tempBuffer = (char*)malloc(MAX_BYTES*sizeof(char));

    int tempBufSize = MAX_BYTES;

    int tempBufferIDX = 0;
    while(byteSend > 0 ){
        byteSend = send(clientSocketID, buffer, byteSend, 0);
        for(int i=0; i<byteSend/sizeof(char); i++){
            tempBuffer[tempBufferIDX] = buffer[i];
            tempBufferIDX++; 
        }
        tempBufSize += MAX_BYTES;
        tempBuffer = (char*)realloc(tempBuffer, tempBufSize);
        if( byteSend < 0 ){
            perror("Error in sending data to client\n");
            break;
        }
        bzero(buffer, MAX_BYTES);
        byteSend = recv(remoteSocketID, buffer, MAX_BYTES-1, 0);
    }
    tempBuffer[tempBufferIDX] = '\0';

    free(buffer);
    addCache(tempBuffer, strlen(tempBuffer), tempReq);
    free(tempBuffer);
    close(remoteSocketID);

    return 0;
}

int sendErrorMessage(int socket, int statusCode){
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %y %H:%M:%S %Z", &data);

    char statusLine[256];
    char body[512];
    
    switch (statusCode)
    {
    case 400:
        snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 400 Bad Request\r\n");
        snprintf(body, sizeof(body), "<html><body><h1>400 Bad Request</h1></body></html>");
        break;
    case 403:
        snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 403 Forbidden\r\n");
        snprintf(body, sizeof(body), "<html><body><h1>403 Forbidden</h1></body></html>");
        break;
    case 404:
        snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 404 Not Found\r\n");
        snprintf(body, sizeof(body), "<html><body><h1>404 Not Found</h1></body></html>");
        break;
    case 500:
        snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 500 Internal Server Error\r\n");
        snprintf(body, sizeof(body), "<html><body><h1>500 Internal Server Error</h1></body></html>");
        break;
    default:
        snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 502 Bad Gateway\r\n");
        snprintf(body, sizeof(body), "<html><body><h1>502 Bad Gateway</h1></body></html>");
        break;
    }
    
    snprintf(str, sizeof(str), 
        "%s"
        "Date: %s\r\n"
        "Server: ProxyServer/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        statusLine, currentTime, strlen(body), body);
    
    int bytesSent = send(socket, str, strlen(str), 0);
    if(bytesSent < 0){
        perror("Error sending error message");
        return -1;
    }
    
    return 0;
}

int checkHTTPVersion(char *msg){
    int version = -1;
    if(strncmp(msg, "HTTP/1.1", 8) == 0){
        version  = 1;
    }else if(strncmp(msg, "HTTP/1.0",8) == 0){
        version = 1;
    }else{
        version = -1;
    }
    return version;

}

void *thread_fn(void *socketNew){
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, &p);
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

    char *tempReq = (char *)malloc(strlen(buffer)*sizeof(char)+1);
    for(int i=0; i<strlen(buffer); i++){
        tempReq[i] = buffer[i];
    }

    cache *temp = find(tempReq);
    // if already in cache
    if(temp != NULL){
        int size = temp->len/sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];

        while(pos<size){
            bzero(response, MAX_BYTES);
            for(int i=0; i<MAX_BYTES; i++){
                response[i] = temp->data[i];
                pos++;
            }
            send(socket, response, MAX_BYTES, 0);
        }
        printf("Data retrived from the cache\n");
        printf("%s\n\n", response);
    }else if(byteSendClient > 0){
        len = strlen(buffer);
        struct ParsedRequest *request = ParsedRequest_create();
        if(ParsedRequest_parse(request, buffer, len) < 0){
            printf("Parsing failed");
        }else{
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method, "GET")){
                if(request->host && request->path && checkHTTPVersion(request->version)==1){
                    byteSendClient = handle_request(socket, request, tempReq);
                    if(byteSendClient == -1){
                        sendErrorMessage(socket, 500 );
                    }
                }else{
                    sendErrorMessage(socket,500);
                }
            }else{
                printf("Currently PROXY SERVER only SUPPORT GET method\n");
            }
        }
        ParsedRequest_destroy(request);
    }else if(byteSendClient == 0){
        printf("Client is disconnected");
    }

    shutdown(socket, SHUT_RDWR);

    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, p);
    printf("Semaphore POST value is %d:\n",p);
    free(tempReq);
    return NULL;
}

int main(int argc,char *argv[]){
    int clientSocketID;
    int clientLen;
    
    struct sockaddr_in server_addr, client_addr;
    
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    // first argument is portnumber
    if(argc == 2){
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
    if(setsockopt(socketID, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0){
        perror("setsockopt failed to reuse socket");
    }

    bzero((char*)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(socketID, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        perror("PORT is not available");
        exit(1);
    }

    printf("Binding on port: %d\n",PORT);

    int listenStatus = listen(socketID, MAX_CLIENTS);
    if(listenStatus<0){
        perror("Error in listening");
        exit(1);
    }

    int i = 0;
    int connectedSocketID[MAX_CLIENTS];

    while(1){
        bzero((char*)&client_addr, sizeof(client_addr));
        clientLen = sizeof(client_addr);
        clientSocketID = accept(socketID, (struct sockaddr_in*)&client_addr, (socklen_t*)&clientLen);
        if(clientSocketID<0){
            perror("Not able to connect to client");
            exit(1);
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