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
#include <unistd.h>
// custom logger
#include "logger.h"

// globals
#define MAX_CLIENTS         10
#define MAX_BYTES           4096
#define MAX_ELEMENT_SIZE    10*(1<<10)
#define MAX_SIZE            200*(1<<20)

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
// check verify version supported by our server
int checkHTTPVersion(char *msg);
// send end client error message along wiht simple html
int sendErrorMessage(int socket, int statusCode);
// connect to remote server
int connectRemoteServer(char *hostaddr, int portNumber);
// function to handle all requests
int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq);

cache *find(char *url){
    cache *site = NULL;

    int temp_lock_val = pthread_mutex_lock(&lock);
    log_msg(LOG_DEBUG, "Cache lock acquired in find()");

    if(head!=NULL){
        site = head;
        while(site != NULL){
            if(!strcmp(site->url, url)){
                char msg[128];

                snprintf(msg, sizeof(msg), "Cache HIT for URL: %s", url);
                log_msg(LOG_INFO, msg);

                snprintf(msg, sizeof(msg), "LRU time before: %ld", site->time);
                log_msg(LOG_DEBUG, msg);

                site->time = time(NULL);

                snprintf(msg, sizeof(msg), "LRU time updated: %ld", site->time);
                log_msg(LOG_DEBUG, msg);

                break;
            }
            site = site->next;
        }
        if(site == NULL){
            char msg[128];
            snprintf(msg, sizeof(msg), "Cache MISS for URL: %s", url);
            log_msg(LOG_INFO, msg);
        }
    }else{
        log_msg(LOG_INFO, "Cache is empty");
    }

    temp_lock_val = pthread_mutex_unlock(&lock);
    log_msg(LOG_DEBUG, "Cache lock released in find()");

    return site;
}

int addCache(char *data, size_t size, char *url){
    int temp_lock_val = pthread_mutex_lock(&lock);
    log_msg(LOG_DEBUG, "Cache lock acquired in addCache()");

    int elementSize = size+1+strlen(url)+sizeof(cache);

    if(elementSize > MAX_ELEMENT_SIZE){
        log_msg(LOG_ERROR, "Cache element too large, skipping insert");

        temp_lock_val = pthread_mutex_unlock(&lock);
        log_msg(LOG_DEBUG, "Cache lock released in addCache()");
        return 0;
    }else{
        while (cacheSize+elementSize > MAX_SIZE ){   
            log_msg(LOG_DEBUG, "Cache overflow, triggering eviction");
            removeCache();
        }

        cache *element = (cache*)malloc(sizeof(cache));
        
        element->data = (char*)malloc(size+1);
        strcpy(element->data, data);

        element->url = (char*)malloc(1+(strlen(url)*sizeof(char)));
        strcpy(element->url, url);

        element->time = time(NULL);
        element->next = head;
        element->len = size;

        head = element;
        cacheSize += elementSize;

        char msg[128];
        snprintf(msg, sizeof(msg), "Cache INSERT: URL=%s SIZE=%zu", url, size);
        log_msg(LOG_INFO, msg);

        temp_lock_val = pthread_mutex_unlock(&lock);
        log_msg(LOG_DEBUG, "Cache lock released in addCache()");

        return 1;
    }
    return 0;
}

void removeCache(){
    cache *prev;
    cache *nxt;
    cache *temp;

    int temp_lock_val = pthread_mutex_lock(&lock);
    log_msg(LOG_DEBUG, "Cache lock acquired in removeCache()");

    if(head!=NULL){
        for(prev=head, nxt=head, temp=head; nxt->next!=NULL; nxt = nxt->next){
            if(((nxt->next)->time) < (temp->time)){
                temp = nxt->next;
                prev = nxt;
            }
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Evicting cache entry URL=%s", temp->url);
        log_msg(LOG_INFO, msg);

        if(temp == head){
            head = head->next;
        }else{
            prev->next = temp->next;
        }

        cacheSize = cacheSize - (temp->len) - sizeof(cache) - strlen(temp->url) - 1;

        free(temp->data);
        free(temp->url);
        free(temp);

        log_msg(LOG_DEBUG, "Cache eviction completed");
    }else{
        log_msg(LOG_DEBUG, "removeCache() called but cache is empty");
    }

    temp_lock_val = pthread_mutex_unlock(&lock);
    log_msg(LOG_DEBUG, "Cache lock released in removeCache()");
}

int connectRemoteServer(char *hostaddr, int portNumber){
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(remoteSocket < 0 ){
        log_msg(LOG_ERROR, "Socket creation failed in connectRemoteServer()");
        return -1;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Connecting to remote server %s:%d", hostaddr, portNumber);
    log_msg(LOG_INFO, msg);

    struct hostent *host = gethostbyname(hostaddr);

    if(host == NULL){
        log_msg(LOG_ERROR, "DNS resolution failed (gethostbyname)");
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portNumber);

    bcopy((char*)host->h_addr_list[0],
          (char*)&server_addr.sin_addr.s_addr,
          host->h_length);

    int connectStatus = connect(remoteSocket,
                                (struct sockaddr*)&server_addr,
                                (size_t)sizeof(server_addr));

    if(connectStatus < 0){
        log_msg(LOG_ERROR, "Connection to remote server failed");
        return -1;
    }

    log_msg(LOG_INFO, "Connection to remote server successful");

    return remoteSocket;
}

int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq){

    log_msg(LOG_INFO, "Handling new client request");

    char *buffer = (char *)malloc(sizeof(char)*MAX_BYTES);

    strcpy(buffer,"GET ");
    strcat(buffer,request->path);
    strcat(buffer," ");
    strcat(buffer, request->version);
    strcat(buffer, "\r\n");

    size_t len = strlen(buffer);

    if(ParsedHeader_set(request, "Connection", "close") < 0 ){
        log_msg(LOG_ERROR, "Failed to set Connection header");
    }

    if(ParsedHeader_get(request,"Host") == NULL){
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            log_msg(LOG_ERROR, "Failed to set Host header");
        }
    }

    if(ParsedRequest_unparse_headers(request, buffer + len, (size_t)MAX_BYTES - len) < 0 ){
        log_msg(LOG_ERROR, "Header unparsing failed");
    }

    int server_port = 80;
    if(request->port != NULL){
        server_port = atoi(request->port);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Forwarding request to %s:%d", request->host, server_port);
    log_msg(LOG_INFO, msg);

    int remoteSocketID = connectRemoteServer(request->host, server_port);

    if(remoteSocketID < 0){
        log_msg(LOG_ERROR, "Failed to connect to remote server");
        return -1;
    }

    int byteSend = send(remoteSocketID, buffer, strlen(buffer), 0);

    log_msg(LOG_DEBUG, "Request sent to remote server");

    bzero(buffer, MAX_BYTES);

    byteSend = recv(remoteSocketID, buffer, MAX_BYTES-1, 0);

    char *tempBuffer = (char*)malloc(MAX_BYTES*sizeof(char));
    int tempBufSize = MAX_BYTES;
    int tempBufferIDX = 0;

    while(byteSend > 0 ){

        byteSend = send(clientSocketID, buffer, byteSend, 0);

        for(size_t i=0; i<byteSend/sizeof(char); i++){
            tempBuffer[tempBufferIDX] = buffer[i];
            tempBufferIDX++; 
        }

        tempBufSize += MAX_BYTES;
        tempBuffer = (char*)realloc(tempBuffer, tempBufSize);

        if( byteSend < 0 ){
            log_msg(LOG_ERROR, "Error sending data to client");
            break;
        }

        bzero(buffer, MAX_BYTES);
        byteSend = recv(remoteSocketID, buffer, MAX_BYTES-1, 0);
    }

    tempBuffer[tempBufferIDX] = '\0';

    log_msg(LOG_INFO, "Response received and forwarded to client");

    free(buffer);

    addCache(tempBuffer, strlen(tempBuffer), tempReq);

    log_msg(LOG_INFO, "Response cached");

    free(tempBuffer);

    close(remoteSocketID);

    log_msg(LOG_DEBUG, "Remote socket closed");

    return 0;
}

int sendErrorMessage(int socket, int statusCode){

    char msg[128];
    snprintf(msg, sizeof(msg), "Sending error response with status code: %d", statusCode);
    log_msg(LOG_INFO, msg);

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
        log_msg(LOG_ERROR, "Failed to send error response to client");
        return -1;
    }

    log_msg(LOG_DEBUG, "Error response sent successfully");

    return 0;
}

int checkHTTPVersion(char *msg){
    int version = -1;

    if(strncmp(msg, "HTTP/1.1", 8) == 0){
        log_msg(LOG_DEBUG, "HTTP version detected: 1.1");
        version  = 1;
    }
    else if(strncmp(msg, "HTTP/1.0",8) == 0){
        log_msg(LOG_DEBUG, "HTTP version detected: 1.0");
        version = 1;
    }
    else{
        log_msg(LOG_ERROR, "Unsupported HTTP version");
        version = -1;
    }

    return version;
}

void *thread_fn(void *socketNew){

    sem_wait(&semaphore);

    int p;
    sem_getvalue(&semaphore, &p);

    char msg[128];
    snprintf(msg, sizeof(msg), "Thread started, semaphore value: %d", p);
    log_msg(LOG_DEBUG, msg);

    int *t = (int *) socketNew;
    int socket = *t;

    snprintf(msg, sizeof(msg), "Handling client socket: %d", socket);
    log_msg(LOG_INFO, msg);

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

    log_msg(LOG_DEBUG, "Full HTTP request received");

    char *tempReq = (char *)malloc(strlen(buffer)*sizeof(char)+1);
    for(size_t i=0; i<strlen(buffer); i++){
        tempReq[i] = buffer[i];
    }

    cache *temp = find(tempReq);

    // CACHE HIT
    if(temp != NULL){

        log_msg(LOG_INFO, "Serving response from cache");

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

        log_msg(LOG_DEBUG, "Cache response sent to client");
    }

    // CACHE MISS → PROCESS REQUEST
    else if(byteSendClient > 0){

        log_msg(LOG_INFO, "Cache MISS - parsing request");

        len = strlen(buffer);

        struct ParsedRequest *request = ParsedRequest_create();

        if(ParsedRequest_parse(request, buffer, len) < 0){
            log_msg(LOG_ERROR, "Request parsing failed");
        }
        else{

            log_msg(LOG_DEBUG, "Request parsed successfully");

            bzero(buffer, MAX_BYTES);

            if(!strcmp(request->method, "GET")){

                log_msg(LOG_INFO, "Processing GET request");

                if(request->host && request->path && checkHTTPVersion(request->version)==1){

                    byteSendClient = handle_request(socket, request, tempReq);

                    if(byteSendClient == -1){
                        log_msg(LOG_ERROR, "Request handling failed, sending 500");
                        sendErrorMessage(socket, 500 );
                    }

                }else{
                    log_msg(LOG_ERROR, "Invalid request fields, sending 500");
                    sendErrorMessage(socket,500);
                }

            }else{
                log_msg(LOG_ERROR, "Unsupported HTTP method (only GET supported)");
            }
        }

        ParsedRequest_destroy(request);
    }

    else if(byteSendClient == 0){
        log_msg(LOG_INFO, "Client disconnected");
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);

    log_msg(LOG_DEBUG, "Socket closed");

    free(buffer);

    sem_post(&semaphore);

    sem_getvalue(&semaphore, &p);
    snprintf(msg, sizeof(msg), "Thread finished, semaphore value: %d", p);
    log_msg(LOG_DEBUG, msg);

    free(tempReq);

    return NULL;
}

int main(int argc,char *argv[]){

    int clientSocketID;
    int clientLen;
    
    struct sockaddr_in server_addr, client_addr;
    
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    log_init();

    if(argc == 2){
        PORT = atoi(argv[1]);
    }else{
        log_msg(LOG_ERROR, "Too few arguments. Usage: ./proxy <port>");
        exit(1);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Starting proxy server at port: %d", PORT);
    log_msg(LOG_INFO, msg);

    socketID = socket(AF_INET, SOCK_STREAM, 0);

    if(socketID<0){
        log_msg(LOG_ERROR, "Failed to create socket");
        exit(1);
    }

    int reuse = 1;
    if(setsockopt(socketID, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0){
        log_msg(LOG_ERROR, "setsockopt failed to reuse socket");
    }

    bzero((char*)&server_addr, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(socketID, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        log_msg(LOG_ERROR, "PORT bind failed (maybe already in use)");
        exit(1);
    }

    snprintf(msg, sizeof(msg), "Binding successful on port: %d", PORT);
    log_msg(LOG_INFO, msg);

    int listenStatus = listen(socketID, MAX_CLIENTS);

    if(listenStatus<0){
        log_msg(LOG_ERROR, "Error in listening on socket");
        exit(1);
    }

    log_msg(LOG_INFO, "Server is now listening for incoming connections");

    int i = 0;
    int connectedSocketID[MAX_CLIENTS];

    while(1){

        bzero((char*)&client_addr, sizeof(client_addr));
        clientLen = sizeof(client_addr);

        clientSocketID = accept(socketID, (struct sockaddr*)&client_addr, (socklen_t*)&clientLen);

        if(clientSocketID<0){
            log_msg(LOG_ERROR, "Failed to accept client connection");
            exit(1);
        }else{
            connectedSocketID[i] = clientSocketID;
        }

        struct sockaddr_in* clientptr = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = clientptr->sin_addr;

        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN); 

        snprintf(msg, sizeof(msg), "Client connected PORT:%d IP:%s",
                 ntohs(client_addr.sin_port), str);
        log_msg(LOG_INFO, msg);

        pthread_create(&threadID[i], NULL, thread_fn, (void *)&connectedSocketID[i]);

        i++;
    }

    close(socketID);

    log_msg(LOG_INFO, "Server shutting down");

    log_destroy();

    return 0;
}