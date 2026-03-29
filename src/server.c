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
#include <sys/select.h>
#include <errno.h>
// custom logger
#include "logger.h"

// globals
#define MAX_CLIENTS         10
#define MAX_BYTES           4096
#define MAX_ELEMENT_SIZE    10*(1<<10)
#define MAX_SIZE            200*(1<<20)
#define HASH_SIZE           1024
#define MAX_CACHEABLE_SIZE  (5 * 1024 * 1024)

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
    cache *next;
    cache *prev;
    cache *hash_next;
};

cache *head = NULL;
cache *tail = NULL;
cache *hash_table[HASH_SIZE] = {NULL};
size_t cacheSize = 0;

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
int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq, const char *method);
//
int handle_connect_tunnel(int clientSocket, int remoteSocket);

// djb2 hash function for strings
unsigned int hash_url(const char *url) {
    unsigned long hash = 5381;
    int c;
    while ((c = *url++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % HASH_SIZE;
}

void move_to_head(cache *node) {
    if (node == head) return;

    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    
    if (node == tail) tail = node->prev;

    node->next = head;
    node->prev = NULL;
    if (head) head->prev = node;
    head = node;
    
    if (tail == NULL) tail = head;
}

cache *find(char *url){
    int temp_lock_val = pthread_mutex_lock(&lock);
    log_msg(LOG_DEBUG, "Cache lock acquired in find()");
    //
    unsigned int hash_idx = hash_url(url);
    cache *site = hash_table[hash_idx];

    while (site != NULL) {
        if (!strcmp(site->url, url)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Cache HIT for URL: %s", url);
            log_msg(LOG_INFO, msg);

            site->time = time(NULL);
            move_to_head(site);

            pthread_mutex_unlock(&lock);
            return site;
        }
        site = site->hash_next;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Cache MISS for URL: %s", url);
    log_msg(LOG_INFO, msg);

    pthread_mutex_unlock(&lock);
    return NULL;
}

int addCache(char *data, size_t size, char *url){
    int temp_lock_val = pthread_mutex_lock(&lock);
    log_msg(LOG_DEBUG, "Cache lock acquired in addCache()");

    int elementSize = size + 1 + strlen(url) + sizeof(cache);

    if (elementSize > MAX_ELEMENT_SIZE) {
        log_msg(LOG_ERROR, "Cache element too large, skipping insert");
        pthread_mutex_unlock(&lock);
        return 0;
    }

    while (cacheSize + elementSize > MAX_SIZE) {   
        log_msg(LOG_DEBUG, "Cache overflow, triggering eviction");
        removeCache();
    }

    cache *element = (cache*)malloc(sizeof(cache));
    
    element->data = (char*)malloc(size + 1);
    memcpy(element->data, data, size);
    element->data[size] = '\0';

    element->url = (char*)malloc(1 + (strlen(url) * sizeof(char)));
    strcpy(element->url, url);

    element->time = time(NULL);
    element->len = size;
    

    element->prev = NULL;
    element->next = NULL;
    unsigned int hash_idx = hash_url(url);
    element->hash_next = hash_table[hash_idx];
    hash_table[hash_idx] = element;

    move_to_head(element);
    cacheSize += elementSize;

    char msg[128];
    snprintf(msg, sizeof(msg), "Cache INSERT: URL=%s SIZE=%zu", url, size);
    log_msg(LOG_INFO, msg);

    pthread_mutex_unlock(&lock);
    return 1;
}

void removeCache(){
    cache *temp = tail;

    if (temp == NULL) {
        log_msg(LOG_DEBUG, "removeCache() called but cache is empty");
        return;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Evicting cache entry URL=%s", temp->url);
    log_msg(LOG_INFO, msg);

    if (temp->prev) temp->prev->next = NULL;
    tail = temp->prev;
    if (head == temp) head = NULL;

    unsigned int hash_idx = hash_url(temp->url);
    cache *curr = hash_table[hash_idx];
    cache *prev = NULL;

    while (curr != NULL) {
        if (curr == temp) {
            if (prev) {
                prev->hash_next = curr->hash_next;
            } else {
                hash_table[hash_idx] = curr->hash_next;
            }
            break;
        }
        prev = curr;
        curr = curr->hash_next;
    }

    cacheSize = cacheSize - (temp->len) - sizeof(cache) - strlen(temp->url) - 1;
    free(temp->data);
    free(temp->url);
    free(temp);

    log_msg(LOG_DEBUG, "Cache eviction completed (O(1))");
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

int handle_connect_tunnel(int clientSocket, int remoteSocket){
    fd_set read_fds;
    int max_fd = (clientSocket > remoteSocket) ? clientSocket : remoteSocket;
    char buffer[MAX_BYTES];
    int bytes_read;

    log_msg(LOG_INFO, "HTTPS CONNECT tunnel established. Starting blind relay.");

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(clientSocket, &read_fds);
        FD_SET(remoteSocket, &read_fds);


        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            log_msg(LOG_ERROR, "select() failed during CONNECT tunnel");
            break;
        }

        if (FD_ISSET(clientSocket, &read_fds)) {
            bytes_read = recv(clientSocket, buffer, MAX_BYTES, 0);
            if (bytes_read <= 0) {
                log_msg(LOG_DEBUG, "Client closed connection in tunnel");
                break; 
            }
            if (send(remoteSocket, buffer, bytes_read, 0) <= 0) {
                log_msg(LOG_ERROR, "Failed to send data to remote server in tunnel");
                break;
            }
        }

        if (FD_ISSET(remoteSocket, &read_fds)) {
            bytes_read = recv(remoteSocket, buffer, MAX_BYTES, 0);
            if (bytes_read <= 0) {
                log_msg(LOG_DEBUG, "Remote server closed connection in tunnel");
                break; 
            }
            if (send(clientSocket, buffer, bytes_read, 0) <= 0) {
                log_msg(LOG_ERROR, "Failed to send data to client in tunnel");
                break;
            }
        }
    }

    log_msg(LOG_INFO, "HTTPS CONNECT tunnel closed.");
    return 0;
}

int handle_request(int clientSocketID, struct ParsedRequest *request, char *tempReq, const char *method){

    log_msg(LOG_INFO, "Handling new client request");

    char *buffer = (char *)malloc(sizeof(char)*MAX_BYTES);
    bzero(buffer, MAX_BYTES);

    snprintf(buffer, MAX_BYTES, "%s %s %s\r\n",
         method,
         request->path ? request->path : "/",
         request->version);

    size_t len = strlen(buffer);

    if (request->port != NULL) {
        char host_header[256];
        snprintf(host_header, sizeof(host_header), "%s:%s", request->host, request->port);
        ParsedHeader_set(request, "Host", host_header);
    } else {
        ParsedHeader_set(request, "Host", request->host);
    }

    ParsedHeader_set(request, "Connection", "close");

    ParsedHeader_remove(request, "Proxy-Connection");
    ParsedHeader_remove(request, "Keep-Alive");
    ParsedHeader_remove(request, "Upgrade");
    ParsedHeader_remove(request, "Transfer-Encoding");

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
        free(buffer);
        return -1;
    }

    int total_sent = 0;
    int req_len = strlen(buffer);

    while (total_sent < req_len) {
        int sent = send(remoteSocketID, buffer + total_sent, req_len - total_sent, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                log_msg(LOG_DEBUG, "send interrupted, retrying");
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_msg(LOG_DEBUG, "send would block, retrying");
                continue;
            }

            log_msg(LOG_ERROR, "send() failed while forwarding request");
            free(buffer);
            close(remoteSocketID);
            return -1;
        }

        if (sent == 0) {
            log_msg(LOG_ERROR, "send() returned 0, connection closed unexpectedly");
            free(buffer);
            close(remoteSocketID);
            return -1;
        }

        total_sent += sent;
    }

    log_msg(LOG_DEBUG, "Request sent to remote server");

    struct ParsedHeader *cl_header = ParsedHeader_get(request, "Content-Length");
    int content_length = 0;

    if (cl_header != NULL) {
        content_length = atoi(cl_header->value);
    }

    if (content_length > 0) {
        char *body_start = strstr(tempReq, "\r\n\r\n");
        if (body_start != NULL) {
            body_start += 4;

            int body_in_buffer = strlen(body_start);
            if (body_in_buffer > 0) {
                send(remoteSocketID, body_start, body_in_buffer, 0);
                content_length -= body_in_buffer;
            }
        }

        while (content_length > 0) {
            bzero(buffer, MAX_BYTES);

            int to_read = (content_length < MAX_BYTES) ? content_length : MAX_BYTES;
            int bytes_read = recv(clientSocketID, buffer, to_read, 0);

            if (bytes_read <= 0) break;

            int sent_total = 0;
            while (sent_total < bytes_read) {
                int sent = send(remoteSocketID, buffer + sent_total, bytes_read - sent_total, 0);
                if (sent <= 0) break;
                sent_total += sent;
            }

            content_length -= bytes_read;
        }
    }

    char *tempBuffer = (char*)malloc(MAX_BYTES);
    int tempBufSize = MAX_BYTES;
    int tempBufferIDX = 0;

    bzero(buffer, MAX_BYTES);
    int bytes_read;
    while (1) {
        bytes_read = recv(remoteSocketID, buffer, MAX_BYTES, 0);

        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;

            log_msg(LOG_ERROR, "recv failed");
            break;
        }
        break;
    }

    while (bytes_read > 0) {
        int sent_total = 0;
        while (sent_total < bytes_read) {
            int sent = send(clientSocketID, buffer + sent_total, bytes_read - sent_total, 0);
            if (sent <= 0) {
                log_msg(LOG_ERROR, "Error sending data to client");
                break;
            }
            sent_total += sent;
        }

        if (tempBufferIDX + bytes_read >= tempBufSize) {
            tempBufSize += MAX_BYTES;
            if (tempBufferIDX + bytes_read > MAX_CACHEABLE_SIZE) {
                log_msg(LOG_INFO, "Response too large, skipping cache");
                free(tempBuffer);
                tempBuffer = NULL;
            }
        }

        memcpy(tempBuffer + tempBufferIDX, buffer, bytes_read);
        tempBufferIDX += bytes_read;

        bzero(buffer, MAX_BYTES);
        bytes_read = recv(remoteSocketID, buffer, MAX_BYTES, 0);
    }

    tempBuffer[tempBufferIDX] = '\0';

    log_msg(LOG_INFO, "Response received and forwarded to client");

    if (strcmp(method, "GET") == 0) {
        char cacheKey[2048];
        snprintf(cacheKey, sizeof(cacheKey), "%s:%s:%s",
                method,
                request->host,
                request->path);

        addCache(tempBuffer, tempBufferIDX, cacheKey);
        log_msg(LOG_INFO, "Response cached");
    } else {
        log_msg(LOG_INFO, "Response not cached (method not GET)");
    }

    free(buffer);
    free(tempBuffer);
    close(remoteSocketID);

    log_msg(LOG_DEBUG, "Remote socket closed");

    return 0;
}

const char* get_http_reason_phrase(int statusCode) {
    switch (statusCode) {
        /* 4xx Client Error */
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        
        /* 5xx Server Error */
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        
        default: return "Unknown Status";
    }
}

int sendErrorMessage(int socket, int statusCode){

    char msg[128];
    snprintf(msg, sizeof(msg), "Sending error response with status code: %d", statusCode);
    log_msg(LOG_INFO, msg);

    char str[2048];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %y %H:%M:%S %Z", &data);

    const char *reasonPhrase = get_http_reason_phrase(statusCode);
    
    char statusLine[256];
    snprintf(statusLine, sizeof(statusLine), "HTTP/1.1 %d %s\r\n", statusCode, reasonPhrase);

    char body[512];
    snprintf(body, sizeof(body), 
        "<html>\n"
        "<head><title>%d %s</title></head>\n"
        "<body bgcolor=\"white\">\n"
        "<center><h1>%d %s</h1></center>\n"
        "<hr><center>ProxyServer/1.0</center>\n"
        "</body>\n"
        "</html>", 
        statusCode, reasonPhrase, statusCode, reasonPhrase);
    
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
    
    size_t total_len = strlen(str);
    size_t bytes_sent_total = 0;

    while (bytes_sent_total < total_len) {
        ssize_t bytes_sent = send(socket, str + bytes_sent_total, total_len - bytes_sent_total, 0);
        
        if (bytes_sent < 0) {
            log_msg(LOG_ERROR, "Failed to send error response to client");
            return -1;
        }
        bytes_sent_total += bytes_sent;
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

    char original_method[16] = {0};
    char target_url[1024] = {0};
    char http_version[16] = {0};
    

    sscanf(buffer, "%15s %1023s %15s", original_method, target_url, http_version);
    size_t method_len = strlen(original_method);


    if (strcmp(original_method, "CONNECT") == 0) {
        log_msg(LOG_INFO, "CONNECT method detected. Initiating tunnel setup.");
        
        char *host = target_url;
        int port = 443;
        

        char *colon = strchr(target_url, ':');
        if (colon != NULL) {
            *colon = '\0';
            port = atoi(colon + 1);
        }

        int remoteSocketID = connectRemoteServer(host, port);
        if (remoteSocketID < 0) {
             log_msg(LOG_ERROR, "Failed to connect to remote server for CONNECT tunnel");
             sendErrorMessage(socket, 502); // Bad Gateway
        } else {

             const char *reply = "HTTP/1.1 200 Connection Established\r\n\r\n";
             send(socket, reply, strlen(reply), 0);
             

             handle_connect_tunnel(socket, remoteSocketID);
             
             close(remoteSocketID);
        }
        

        shutdown(socket, SHUT_RDWR);
        close(socket);
        free(buffer);
        sem_post(&semaphore);
        return NULL;
    }

    if (strcmp(original_method, "GET") != 0) {
        if (method_len >= 3) {
            buffer[0] = 'G'; buffer[1] = 'E'; buffer[2] = 'T';
            for (size_t i = 3; i < method_len; i++) {
                buffer[i] = ' ';
            }
        }
    }

    char *tempReq = (char *)malloc(strlen(buffer)*sizeof(char)+1);
    for(size_t i=0; i<strlen(buffer); i++){
        tempReq[i] = buffer[i];
    }
    tempReq[strlen(buffer)] = '\0';

    struct ParsedRequest *request = ParsedRequest_create();
    // FIXME: request 
    char cacheKey[2048];
    snprintf(cacheKey, sizeof(cacheKey), "%s:%s:%s",
            original_method,
            request->host,
            request->path);

    cache *temp = find(cacheKey);

    // CACHE HIT
    if(temp != NULL){

        log_msg(LOG_INFO, "Serving response from cache");

        int total_sent = 0;
        int size = temp->len;

        while (total_sent < size) {
            int chunk = (size - total_sent > MAX_BYTES) ? MAX_BYTES : (size - total_sent);

            int sent_total = 0;
            while (sent_total < chunk) {
                int sent = send(socket, temp->data + total_sent + sent_total, chunk - sent_total, 0);
                if (sent <= 0) {
                    log_msg(LOG_ERROR, "Error sending cached response");
                    break;
                }
                sent_total += sent;
            }
            total_sent += sent_total;
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

            int is_supported = 0;
            const char *methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH", "TRACE", "CONNECT"};
            for(int i=0; i<9; i++){
                if(strcmp(original_method, methods[i]) == 0){
                    is_supported = 1; 
                    break;
                }
            }

            if(is_supported){

                char log_buf[128];
                snprintf(log_buf, sizeof(log_buf), "Processing %s request", original_method);
                log_msg(LOG_INFO, log_buf);

                if(request->host && request->path && checkHTTPVersion(request->version)==1){

                    byteSendClient = handle_request(socket, request, tempReq, original_method);

                    if(byteSendClient == -1){
                        log_msg(LOG_ERROR, "Request handling failed, sending 500");
                        sendErrorMessage(socket, 500);
                    }

                }else{
                    log_msg(LOG_ERROR, "Invalid request fields, sending 500");
                    sendErrorMessage(socket, 500);
                }

            }else{
                log_msg(LOG_ERROR, "Unsupported HTTP method");
                sendErrorMessage(socket, 501);
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


#ifndef BENCHMARKING
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
#endif