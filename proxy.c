/*
 * proxy.c - CS:APP Web proxy
 *
 * Ben Ruktantichoke 20121092
 *
 * proxy.c implements a multithreaded HTTP proxy for web browsers.
 * It currently is designed to handled only HTTP 1.0 requets.
 * The proxy caches objects of previous requests in main memory.
 * Cache is implented as a hash table of linked lists.
 * A log file is created of all the requests clients have made.
 *
 */ 

#include "csapp.h"
#include <assert.h>
#include <pthread.h>

#define MAX_CACHE_SIZE 5242280 // Maximum size of Cache
#define MAX_OBJ_SIZE 524228    // Maximum object size in Cache
#define CACHE_SIZE 16384       // Number of Hash table entries

FILE * file = NULL;            // Log File
pthread_mutex_t net_mutex;     // Mutex for getting uri address
pthread_mutex_t file_mutex;    // Mutex for writing to logfile
pthread_rwlock_t cache_mutex;  // Mutex for acessing the cache

/* HTTP constant variables */
const char CRLF[] = "\r\n";
const char *HTTP_method[] = {"GET", "POST", "HEAD", "DELETE", "PUT", "LINK", "UNLINK"};
const char *header[] = {"Allow", "Authorization", "Content-Encoding", "Content-Length",
    "Content-Type", "Host", "Date", "Expires", "From", "If-Modified-Since",
    "Last-Modified", "Connection", "Location", "Pragma", "Referer", "Server",
    "User-Agent", "WWW-Authenticate", "Accept", "Accept-Charset", "Accept-Encoding",
    "Accept-Language", "Content-Language", "Link", "MIME-Version", "Retry-After",
    "Title", "URI"};

struct args {  // Argument structure passed to spawned threads
    int connfd;
    struct sockaddr_in sockaddr;
};

struct cache_obj {
    uint32_t hash; // URI hash
    char *mem;     // Return obj
    uint32_t size; // Size of Return obj
    struct list_obj *list_obj;
    struct cache_obj *next;
};

struct list_obj { // an object in LRU list
    struct cache_obj *cache_obj;
    struct list_obj *next;
    struct list_obj *prev;
};

struct obj_list { // LRU list
    struct list_obj *head;
    struct list_obj *tail;
} obj_list;

struct cache {  // Cache
    struct cache_obj *bin[CACHE_SIZE];
    uint32_t size;
} cache;

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);

/* Send HTTP style response to Client */
/* prints error if it occurs */
void send_error(int connfd, int code, char *message)
{
    size_t bytes;
    char error[256] = {0};
    bytes = sprintf(error, "HTTP/1.0 %d %s\r\n\r\n"
                    "<!DOCTYPE html>\n"
                    "<html>\n"
                    "<body>\n"
                    "<h1>%d %s</h1>\n"
                    "</body>\n"
                    "</html>", code, message, code, message);
    if (write(connfd, error, bytes) != bytes) {
        perror("Write: ");
    }
}

/* converts all string characters to lowercase */
void string_tolower(char *string)
{
    int index;
    for (index = 0; string[index]; index++) {
        string[index] = tolower(string[index]);
    }
}

/* converts all string cahracters to uppercase */
void string_toupper(char *string)
{
    int index;
    for (index = 0; string[index]; index++) {
        string[index] = toupper(string[index]);
    }
}

/* helper function: handle cleanup on each iteration of main loop */
void handle_continue(void *request, int connfd)
{
    Free(request);
    close(connfd);
}

/* djb2 hash function for strings */
unsigned long hash(unsigned char *str)
{
    unsigned long hash = 5381;
    int c;
    
    while (c = *str++)
        hash = ((hash << 5) + hash) ^ c; /* hash * 33 + c */
    
    return hash;
}

/*
 * open_clientfd_r - open connection to server at <hostname, port>
 *   and it has been built on top of open_clientfd to be thread-safe
 *   and return a socket descriptor ready for reading and writing.
 *   Returns -1 and sets errno on Unix error.
 *   Returns -2 and sets h_errno on DNS (gethostbyname) error.
 */
/* $begin open_clientfd_r */
int open_clientfd_r(char *hostname, int port)
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;
    
    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1; /* check errno for cause of error */
    
    /* Fill in the server's IP address and port */
    pthread_mutex_lock(&net_mutex);
    if ((hp = gethostbyname(hostname)) == NULL) {
        pthread_mutex_unlock(&net_mutex);
        return -2; /* check h_errno for cause of error */
    }
    pthread_mutex_unlock(&net_mutex);
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0],
          (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return clientfd;
}

/* Main function that worker thread executes */
/* Handles each client request */
void *worker(void *vargp)
{
    struct args *args = vargp;
    int connfd = args->connfd;
    struct sockaddr_in clientaddr = args->sockaddr;
    Pthread_detach(pthread_self());
    Free(vargp);
    
    ssize_t h_size;
    size_t l_size;
    int simp_resp = 0;                                              /* Simple Response                       */
    char buf[MAXBUF] = {0}, request_line[MAXLINE] = {0};            /* Raw HTPP Request and Request-Line buf */
    char *buf_p = buf;                                              /* Pointer into buf for reading lines    */
    char *request = (char *) Calloc(1, MAXBUF);                     /* Request Sent to Server                */
    char *marker = request;                                         /* Pointer into request to append data   */
    char method[16] = {0}, uri[MAXLINE] = {0}, version[16] = {0};   /* Request-Line Fields                   */
    
    /* Read in initial Request */
    h_size = Read(connfd, buf, MAXBUF);
    if ((l_size = strcspn(buf, CRLF)) == h_size) {
        send_error(connfd, 400, "Bad Request");
        handle_continue(request, connfd);
        return NULL;
    }
    memcpy(request_line, buf, l_size);   /* Extract Request-line */
    buf_p += l_size + 2;
    if (l_size + 2 == h_size) { //Telnet requests
        simp_resp = 1;
    }
    
    /* Parse Request-Line */
    int ret = sscanf(request_line, "%s %s %s", method, uri, version);
    if (ret == EOF) {
        send_error(connfd, 500, "Internal Error");
        handle_continue(request, connfd);
        return NULL;
    } else if (ret < 3) { //This case shouldn't occur (HTTP 0.9) - tested and seems to be outdated.
        //fprintf(stderr, "Number of arguments in request_line is: %d\n", ret);
        simp_resp = 1;
    }
    string_toupper(method);

    /* Check for presence of request objects in Cache */
    struct cache_obj *c_obj = NULL;
    struct list_obj *l_obj = NULL;
    uint32_t hs = hash(uri);
    int index = hs%CACHE_SIZE;
    pthread_rwlock_wrlock(&cache_mutex);
    if ((c_obj = cache.bin[index]) && strcmp(method, "GET") == 0) {
        do {
            if (c_obj->hash != hs) {
                c_obj = c_obj->next;
            } else {
                break;
            }
        } while (c_obj != NULL);
        if (c_obj) {
            l_obj = c_obj->list_obj;
            int head = 0;
            if (l_obj->prev) {
                l_obj->prev->next = l_obj->next;
                if (l_obj->next) {
                    l_obj->next->prev = l_obj->prev;
                } else {
                    obj_list.tail = l_obj->prev;
                }
            } else {
                head = 1;
            }
            if (!head) {
                obj_list.head->prev = l_obj;
                l_obj->next = obj_list.head;
                obj_list.head = l_obj;
            }
            pthread_rwlock_unlock(&cache_mutex);
            pthread_rwlock_rdlock(&cache_mutex);
            /* Send Response to Client */
            if (write(connfd, c_obj->mem, c_obj->size) < 0){
                pthread_rwlock_unlock(&cache_mutex);
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                return NULL;
            }
            
            /* Create Log Entry */
            char log[MAXBUF*2] = {0};
            format_log_entry(log, &clientaddr, uri, c_obj->size);
            pthread_rwlock_unlock(&cache_mutex);
            ssize_t loglen = strlen(log);
            pthread_mutex_lock(&file_mutex);
            Fwrite(log, loglen, 1, file);
            pthread_mutex_unlock(&file_mutex);
            fflush(file);
            
            handle_continue(request, connfd);
            //fprintf(stderr, "retrieved: %u from cache\n", c_obj->hash);
            return NULL;
        }
    }
    pthread_rwlock_unlock(&cache_mutex);

    /* Check Valid Method */
    int i, is_valid = 0;
    for (i = 0; i < (sizeof(HTTP_method)/sizeof(HTTP_method[0] - 1)); i++) {
        if (strcmp(method, HTTP_method[i]) == 0) {
            is_valid = 1;
            break;
        }
    }
    if (!is_valid) {
        if (strcmp(method, "CONNECT") == 0 || strcmp(method, "TRACE") == 0) {
            send_error(connfd, 501, "Not Implemented");
            handle_continue(request, connfd);
            return NULL;
        } else {
            send_error(connfd, 400, "Bad Request");
            handle_continue(request, connfd);
            return NULL;
        }
    }
    
    /* Force HTTP version 1.0 */ /*
    if (strcmp(version, "HTTP/1.0") != 0) {
        memset(version, 0, sizeof(version));
        sprintf(version, "HTTP/1.0");
    }*/
    
    /* Parse Uri */
    char hostname[MAXLINE] = {0}, pathname[MAXLINE] = {0};
    int r_port;
    string_tolower(uri);
    if ((parse_uri(uri, hostname, pathname, &r_port)) < 0) {
        send_error(connfd, 400, "Bad Request");
        handle_continue(request, connfd);
        return NULL;
    }
    
    /* Write request_line into request to server */
    int bytes_written, content_length = 0;
    size_t bytes_read;
    if (simp_resp) {
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(request, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                return NULL;
            }
        } else {
            if ((bytes_written = sprintf(request, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                return NULL;
            }
        }
        marker = request + bytes_written;
    } else {
        // fprintf(stderr, "Checking Headers\n");
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(request, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                return NULL;
            }
        } else {
            if ((bytes_written = sprintf(request, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                return NULL;
            }
        }
        marker = request + bytes_written;
        
        int host_header = 0, connection = 0, c_len = 0, proxy = 0;
        while ((l_size = strcspn(buf_p, CRLF)) != 0) {
            char head[64] = {0}, value[4096] = {0}; //Accounts for MAX cookie size
            
            if ((sscanf(buf_p, "%[^:]: %s", head, value) == 0)) {
                send_error(connfd, 400, "Bad Request");
                handle_continue(request, connfd);
                return NULL;
            }
            //fprintf(stderr, "head: %s\tvalue:%s\n", head, value);
            if (head[0] == 0) {
                send_error(connfd, 400, "Bad Request");
                handle_continue(request, connfd);
                return NULL;;
            }
            if (!host_header) {
                if (strcasecmp(head, "Host") == 0) {
                    host_header = 1;
                }
            }
            if (!connection) {
                if (strcasecmp(head, "Connection") == 0) {
                    memset(value, 0, sizeof(value));
                    sprintf(value, "close");
                    connection = 1;
                }
            }
            if (!proxy) {
                if (strcasecmp(head, "Proxy-Connection") == 0) {
                    memset(value, 0, sizeof(value));
                    sprintf(value, "close");
                    proxy = 1;
                }
            }
            if (!c_len) {
                if (strcasecmp(head, "Content-Length") == 0){
                    content_length = atoi(value);
                    c_len = 1;
                }
            }/*
              if (!strcasecmp(head, "Keep-Alive")) {
              continue;
              }*/
            
            /* Check if one of RFC 1945 prescribed Headers */
            int valid_header = 0;
            for (i = 0; i < (sizeof(header)/sizeof(header[0]) - 1); i++) {
                if (strcasecmp(head, header[i]) == 0) {
                    valid_header = 1;
                    break;
                }
            }
            /* format header fields and append to HTTP_request */
            if (value[0] == 0) {
                if (valid_header == 1) {
                    if ((bytes_written = sprintf(marker, "%s: \r\n", header[i])) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(request, connfd);
                        return NULL;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: \r\n", head)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(request, connfd);
                        return NULL;
                    }
                }
            } else {
                if (valid_header == 1) {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", header[i], value)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(request, connfd);
                        return NULL;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", head, value)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(request, connfd);
                        return NULL;
                    }
                }
            }
            marker += bytes_written;
            buf_p += l_size + 2;
        }
        bytes_read = buf_p - buf;
        if ((bytes_read == h_size) || !host_header) {
            send_error(connfd, 400, "Bad Request");
            handle_continue(request, connfd);
            return NULL;
        }
        buf_p += 2;
        bytes_read += 2;
    }
    sprintf(marker, "\r\n");
    marker += 2;
    
    /* Open socket connection and send http request to server */
    int serverfd = open_clientfd_r(hostname, r_port);
    
    if (simp_resp) {//HTTP 0.9
        if (write(serverfd, request, strlen(request)) < 0){
            send_error(connfd, 500, "Internal Error");
            handle_continue(request, connfd);
            close(serverfd);
            return NULL;
        }
    } else { // This doesn't really happen too (Probably large POST)
        if (content_length > MAXBUF - (bytes_read)) {/* HTTP request did not fit in 8kB buffer */
            //fprintf(stderr, "***********UNTESTED***********\n");
            /* copy the remaining material in read request into http request */
            size_t remainder = h_size - (bytes_read); //size of content already read
            memcpy(marker, buf_p, remainder);
            size_t temp = marker - request;
            marker += remainder;
            //assert(marker-request == MAXBUF);
            
            /* allocate enough memory to store entire http request */
            int total_bytes = temp + content_length;
            if ((request = (char *) realloc(request, total_bytes)) == NULL) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                close(serverfd);
                return NULL;
            }
            marker = request + temp + remainder;
            
            /* read rest of http request, assumes HTTP header is no longer than MAXBUF */
            if ((temp = read(connfd, marker, content_length - remainder)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                close(serverfd);
                return NULL;
            }
            
            /* write to server */
            if (write(serverfd, request, total_bytes) < 0){
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                close(serverfd);
                return NULL;
            }
        } else { /* (http request is smaller than buffer */
            /* append auxillary info if it is given */
            // fprintf(stderr, "Regular HTTP/1.0 Request\n");
            if (content_length != 0) {
                memcpy(marker, buf_p, content_length);
            }
            bytes_read += content_length;
            
            if (write(serverfd, request, bytes_read) < 0){
                send_error(connfd, 500, "Internal Error");
                handle_continue(request, connfd);
                close(serverfd);
                return NULL;
            }
        }
    }
    
    // read response from server
    char *response = (char *) Calloc(1, MAXBUF);
    marker = response;
    int total_bytes = 0;
    int count = 1;
    while ((bytes_read = rio_readn(serverfd, marker, MAXBUF * count - total_bytes)) > 0) {
        total_bytes += bytes_read;
        if (total_bytes < 16777216) {
            // fprintf(stderr, "TRYING TO ALLOCATE: %u\n", MAXBUF * count * 2);
            response = (char *) Realloc(response, MAXBUF * count * 2);
            marker = response + total_bytes;
            count*=2;
        }
    }
    if (bytes_read == -1) {
        send_error(connfd, 500, "Internal Error");
        handle_continue(request, connfd);
        handle_continue(response, serverfd);
        return NULL;
    }
    
    /* Send Response to Client */
    if (write(connfd, response, total_bytes) < 0){
        send_error(connfd, 500, "Internal Error");
        handle_continue(request, connfd);
        handle_continue(response, serverfd);
        return NULL;
    }
    
    /* Add object to cache */
    if (total_bytes < MAX_OBJ_SIZE) {
        c_obj = (struct cache_obj *) Calloc(1, sizeof(struct cache_obj));
        c_obj->hash = hs;
        c_obj->mem = (char *) Realloc(response, total_bytes);
        c_obj->size = total_bytes;
        
        l_obj = (struct list_obj *) Calloc(1, sizeof(struct list_obj));
        c_obj->list_obj = l_obj;
        l_obj->cache_obj = c_obj;
        
        pthread_rwlock_wrlock(&cache_mutex);
        l_obj->next = obj_list.head;
        if (obj_list.head != NULL) {
            obj_list.head->prev = l_obj;
        }
        if (obj_list.tail == NULL) {
            obj_list.tail = l_obj;
        }
        obj_list.head = l_obj;
        
        struct cache_obj *temp = NULL;
        if ((temp = cache.bin[index]) == NULL) {
            cache.bin[index] = c_obj;
        } else {
            while (temp->next != NULL) {
                if (temp->hash == c_obj->hash) {
                    //fprintf(stderr, "Hash Collision!!\n");
                }
                temp = temp->next;
            }
            if (temp->hash == c_obj->hash) {
                //fprintf(stderr, "Hash collision");
            }
            temp->next = c_obj;
        }
        cache.size += c_obj->size;
        
        /* LRU eviction */
        while (cache.size > MAX_CACHE_SIZE) {
            l_obj = obj_list.tail;
            c_obj = l_obj->cache_obj;
            
            l_obj->prev->next = NULL;
            obj_list.tail = l_obj->prev;
            
            index = c_obj->hash%CACHE_SIZE;
            temp = cache.bin[index];
            struct cache_obj *prev_obj = NULL;
            if (temp->hash == c_obj->hash) {
                if (temp->next) {
                    cache.bin[index] = temp->next;
                } else {
                    cache.bin[index] = NULL;
                }
            } else {
                while (temp->hash != c_obj->hash) {
                    prev_obj = temp;
                    temp = temp->next;
                }
                prev_obj->next = NULL;
            }
            //fprintf(stderr, "Evicted: %u from cache\n", c_obj->hash);
            cache.size -= c_obj->size;
            Free(c_obj->mem);
            Free(c_obj);
            Free(l_obj);
            c_obj = NULL;
        }
        pthread_rwlock_unlock(&cache_mutex);
        if (c_obj) {
            //fprintf(stderr, "Added: %u to cache\tsize: %u\n", hs, c_obj->size);
        }
    }

    /* Create Log Entry */
    char log[MAXBUF*2] = {0};
    format_log_entry(log, &clientaddr, uri, total_bytes);
    ssize_t loglen = strlen(log);
    pthread_mutex_lock(&file_mutex);
    Fwrite(log, loglen, 1, file);
    pthread_mutex_unlock(&file_mutex);
    fflush(file);
    
    Close(serverfd);
    handle_continue(request, connfd);
}

/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv)
{
    /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }
    int port = atoi(argv[1]);
    
    /* Check port */
    if ((port < 1024) || (port > 65535)) {
        fprintf(stderr, "<port number> must be between 1024 and 65536\n");
        exit(0);
    }
    
    /* Open listen socket */
    int listenfd;
    if ((listenfd = Open_listenfd(port)) < 0) {
        perror("Open_listenfd error: ");
        exit(1);
    }
    
    Signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&net_mutex, NULL);
    pthread_rwlock_init(&cache_mutex, NULL);
    
    if ((file = fopen("proxy.log", "a+")) == NULL) {
        perror("fopen: ");
        exit(1);
    }
    
    /* Main Loop */
    obj_list.head = NULL;
    obj_list.tail = NULL;
    cache.size = 0;
    memset(cache.bin, 0, CACHE_SIZE);
    socklen_t clientlen = sizeof(struct sockaddr_in);
    struct sockaddr_in clientaddr;
    pthread_t tidp;
    struct args *argp = NULL;
    while (1) {
        /* Establish Connection */
        argp = (struct args *) Calloc(1, sizeof(struct args));
        argp->connfd = Accept(listenfd, (struct sockaddr *) &clientaddr, &clientlen);
        argp->sockaddr = clientaddr;
        /* Create Worker Thread to handle Client */
        Pthread_create(&tidp, NULL, worker, argp);
    }
    
    // Never Reached
    Fclose(file);
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&net_mutex);
    pthread_rwlock_wrlock(&cache_mutex);
    struct list_obj *temp = obj_list.head;
    struct list_obj *next_obj = NULL;
    while (temp != NULL) {
        next_obj = temp->next;
        Free(temp->cache_obj->mem);
        Free(temp->cache_obj);
        Free(temp);
        temp = next_obj;
    }
    pthread_rwlock_unlock(&cache_mutex);
    pthread_rwlock_destroy(&cache_mutex);
    exit(0);
}

/*
 * parse_uri - URI parser
 * 
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    //char *pathbegin;
    //int len;
    int ret;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }
    
    hostbegin = uri + 7;
    hostend = hostbegin;
    while (strncmp(hostend, "/", 1) != 0) {
        hostend += 1;
        if (!strncmp(hostend, ":", 1)) {
            ret = sscanf(uri, "http://%[^:]:%d/%s", hostname, port, pathname);
            //fprintf(stderr, "hostname: %s\tport:%d\tpathname:%s\n",hostname, *port, pathname);
            if (ret < 2) {
                //fprintf(stderr, "*******Error in parsing uri*********\n");
                return -1;
            }
            return 0;
        }
    }
    if (hostend == hostbegin) {
        hostname[0] = '\0';
        return -1;
    }
    ret = sscanf(uri, "http://%[^/]/%s", hostname, pathname);
    //fprintf(stderr, "hostname: %s\tpathname:%s\n", hostname, pathname);
    if (ret < 1) {
        //fprintf(stderr, "**********Error in parsing uri**********\n");
        return -1;
    }
    *port = 80;

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring. 
 * 
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, 
		      char *uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /* 
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;


    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s\n", time_str, a, b, c, d, uri);
}


