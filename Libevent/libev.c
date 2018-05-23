/*
 * proxy.c - CS:APP Web proxy
 *
 * Ben Ruktantichoke 20121092
 *
 * TODO:
 */

#include "csapp.h"
#include <assert.h>
#include <pthread.h>
#include <event2/event.h>
#include <fcntl.h>

#define MAX_CACHE_SIZE 5242280 // Maximum size of Cache
#define MAX_OBJ_SIZE 524228    // Maximum object size in Cache
#define CACHE_SIZE 16384       // Number of Hash table entries

FILE * file = NULL;            // Log File

/* HTTP constant variables */
const char CRLF[] = "\r\n";
const char *HTTP_method[] = {"GET", "POST", "HEAD", "DELETE", "PUT", "LINK", "UNLINK"};
const char *header[] = {"Allow", "Authorization", "Content-Encoding", "Content-Length",
    "Content-Type", "Host", "Date", "Expires", "From", "If-Modified-Since",
    "Last-Modified", "Connection", "Location", "Pragma", "Referer", "Server",
    "User-Agent", "WWW-Authenticate", "Accept", "Accept-Charset", "Accept-Encoding",
    "Accept-Language", "Content-Language", "Link", "MIME-Version", "Retry-After",
    "Title", "URI"};

struct cache_obj {
    uint32_t hash; // URI hash
    char *mem;     // Return obj
    uint32_t size; // Size of Return obj
    struct tm *expires;
    int is_expires;
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

struct args {
    struct event_base *base;
    struct sockaddr_in clientaddr;
    char uri[MAXLINE];
    int connfd;
    uint32_t hs;
    int no_cache;
    char *request;
    struct cache_obj *c_obj;
    long bytes_read;

};

struct write_args {
    struct event_base *base;
    struct sockaddr_in clientaddr;
    char uri[MAXLINE];
    int connfd;
    uint32_t hs;
    int no_cache;
    char *response;
    struct cache_obj *c_obj;
    long total_bytes;
    char *marker;
    int count;
};

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size, char *status);

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
void handle_continue(void *request, int connfd, void *mem)
{
    Free(request);
    close(connfd);
    Free(mem);
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
 * open_clientfd - open connection to server at <hostname, port>
 *   and return a socket descriptor ready for reading and writing.
 *   Returns -1 and sets errno on Unix error.
 *   Returns -2 and sets h_errno on DNS (gethostbyname) error.
 */
/* $begin open_clientfd */
int open_clientfd_r(int *serverfd, char *hostname, int port)
{
    struct hostent *hp;
    struct sockaddr_in serveraddr;
    
    if ((*serverfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1; /* check errno for cause of error */
    fcntl(*serverfd, F_SETFD, O_NONBLOCK);
    /* Fill in the server's IP address and port */
    if ((hp = gethostbyname(hostname)) == NULL)
        return -2; /* check h_errno for cause of error */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0],
          (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    
    /* Establish a connection with the server */
    if (connect(*serverfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return 0;
}

static void reWriteEvent(int serverfd, short event, void *arg)
{
    struct write_args *write_args = (struct write_args *) arg;
    int index = write_args->hs%CACHE_SIZE;
    
    // read response from server
    int bytes_read;
    if ((bytes_read = read(serverfd, write_args->marker, MAXBUF * write_args->count/2)) > 0) {
        //fprintf(stderr, "bytes_read2: %d\n", bytes_read);
        write_args->total_bytes += bytes_read;
        if (write_args->total_bytes < 3221225472) { //3Gb limit on file<< MUST ALLOCATE THIS!
            if (write_args->total_bytes > MAXBUF * write_args->count/2) {
                write_args->count *= 2;
                write_args->response = Realloc(write_args->response, MAXBUF * write_args->count);
                //fprintf(stderr, "total bytes: %ld Reallocating %d bytes\n", write_args->total_bytes, MAXBUF * write_args->count);
            }
            write_args->marker = write_args->response + write_args->total_bytes;
            //fprintf(stderr, "creating reWriteEvent\n");
            struct event *ev_read = event_new(write_args->base, serverfd, EV_READ, reWriteEvent, (void *)write_args);
            event_add(ev_read, NULL);
            return;
        }
    }
    if (bytes_read == -1 && errno != ECONNRESET) {
        send_error(write_args->connfd, 500, "Internal Error");
        perror("errorno1: ");
        Close(write_args->connfd);
        handle_continue(write_args->response, serverfd, write_args);
        return;
    }
    //fprintf(stderr, "In reWriteEvent\n");
    int resp_status = 0;
    sscanf(write_args->response, "%*s %d", &resp_status);
    //fprintf(stderr, "Response: %d\n", resp_status);
    if (resp_status == 304 && write_args->c_obj) {//handles is_expires and !is_expires
        if (write(write_args->connfd, write_args->c_obj->mem, write_args->c_obj->size) < 0){
            send_error(write_args->connfd, 500, "Internal Error");
            perror("errorno2: ");
            Close(write_args->connfd);
            handle_continue(write_args->response, serverfd, write_args);
            return;
        }
        
        //fprintf(stderr, "retrieved: %u from cache", write_args->c_obj->hash);
        char log[MAXBUF*2] = {0};
        format_log_entry(log, &(write_args->clientaddr), write_args->uri, write_args->c_obj->size, "Hit");
        ssize_t loglen = strlen(log);
        Fwrite(log, loglen, 1, file);
        fflush(file);
        
        if (!(write_args->c_obj->is_expires)) {
            time_t now = time(NULL);
            struct tm *gmt = gmtime(&now);
            memcpy(write_args->c_obj->expires, gmt, sizeof(struct tm));
            //fprintf(stderr, "(no expires field)\n");
        } else {
            //fprintf(stderr, "(with expires field)\n");
        }
        
        Close(write_args->connfd);
        handle_continue(write_args->response, serverfd, write_args);
        return;
    } else { /* Send Response to Client */
        if (write(write_args->connfd, write_args->response, write_args->total_bytes) < 0){
            send_error(write_args->connfd, 500, "Internal Error");
            perror("errorno3: ");
            Close(write_args->connfd);
            handle_continue(write_args->response, serverfd, write_args);
            return;
        }
        if (write_args->c_obj) {
            //fprintf(stderr, "Evicting old entry\thash:%u\n", write_args->hs);
            struct list_obj *l_obj = write_args->c_obj->list_obj;
            if (l_obj->prev) {
                l_obj->prev->next = l_obj->next;
                if (l_obj->next) {
                    l_obj->next->prev = l_obj->prev;
                } else {
                    obj_list.tail = l_obj->prev;
                }
            } else {
                obj_list.head = l_obj->next;
                if (l_obj->next) {
                    l_obj->next->prev = NULL;
                } else {
                    obj_list.tail = NULL;
                }
            }
            
            struct cache_obj *temp = cache.bin[index];
            struct cache_obj *prev_obj = NULL;
            if (temp->hash == write_args->c_obj->hash) {
                if (temp->next) {
                    cache.bin[index] = temp->next;
                } else {
                    cache.bin[index] = NULL;
                }
            } else {
                while (temp->hash != write_args->c_obj->hash) {
                    prev_obj = temp;
                    temp = temp->next;
                }
                prev_obj->next = NULL;
            }
            cache.size -= write_args->c_obj->size;
            Free(temp->expires);
            Free(temp->mem);
            Free(temp);
            Free(l_obj);
            write_args->c_obj = NULL;
        }
    }
    
    struct list_obj *l_obj = NULL;
    struct cache_obj *c_obj = NULL;
    /* Add object to cache */
    if (write_args->total_bytes < MAX_OBJ_SIZE) {
        c_obj = (struct cache_obj *) Calloc(1, sizeof(struct cache_obj));
        c_obj->hash = write_args->hs;
        c_obj->mem = (char *) Realloc(write_args->response, write_args->total_bytes);
        c_obj->size = write_args->total_bytes;
        c_obj->expires = (struct tm *) Calloc(1, sizeof(struct tm));
        
        char *tempt = strstr(write_args->response, "Expires:");
        time_t now = time(NULL);
        double seconds = 0;
        if (tempt) {
            //fprintf(stderr, "Found Expires Header\n");
            c_obj->is_expires = 1;
            strptime(tempt, "%*s %a, %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //char xyz[MAXLINE];
            //strftime(xyz, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //fprintf(stderr, "%s\n", xyz);
            //fprintf(stderr, "%s\n", write_args->response);
            seconds = difftime(mktime(c_obj->expires), now);
            //fprintf(stderr, "Add to cache if Seconds positive: %f\n", seconds);
        } else {
            struct tm *gmt = gmtime(&now);
            memcpy(c_obj->expires, gmt, sizeof(struct tm));
        }
        
        if (seconds > 0 || !tempt) {
            struct cache_obj *temp = NULL;
            l_obj = (struct list_obj *) Calloc(1, sizeof(struct list_obj));
            c_obj->list_obj = l_obj;
            l_obj->cache_obj = c_obj;
            
            l_obj->next = obj_list.head;
            if (obj_list.head != NULL) {
                obj_list.head->prev = l_obj;
            }
            if (obj_list.tail == NULL) {
                obj_list.tail = l_obj;
            }
            obj_list.head = l_obj;
            
            if ((temp = cache.bin[index]) == NULL) {
                cache.bin[index] = c_obj;
            } else {
                while (temp->next != NULL) { //Not 200 response!
                    if (temp->hash == c_obj->hash) {
                        Free(temp->expires);
                        Free(temp->mem);
                        struct list_obj *l_temp = temp->list_obj;
                        if (l_temp->prev) {
                            l_temp->prev->next = l_temp->next;
                        } else {
                            obj_list.head = l_temp->next;
                        }
                        if (l_temp->next) {
                            l_temp->next->prev = l_temp->prev;
                        } else {
                            obj_list.tail = l_temp->prev;
                        }
                        Free(l_temp);
                        cache.size -= temp->size;
                        temp->expires = c_obj->expires;
                        temp->mem = c_obj->mem;
                        temp->list_obj = c_obj->list_obj;
                        temp->size = c_obj->size;
                        temp->is_expires = c_obj->is_expires;
                        Free(c_obj);
                        c_obj = temp;
                        break;
                    }
                    temp = temp->next;
                }
                if (temp->hash == c_obj->hash && temp != c_obj) {
                    Free(temp->expires);
                    Free(temp->mem);
                    struct list_obj *l_temp = temp->list_obj;
                    if (l_temp->prev) {
                        l_temp->prev->next = l_temp->next;
                    } else {
                        obj_list.head = l_temp->next;
                    }
                    if (l_temp->next) {
                        l_temp->next->prev = l_temp->prev;
                    } else {
                        obj_list.tail = l_temp->prev;
                    }
                    Free(l_temp);
                    cache.size -= temp->size;
                    temp->expires = c_obj->expires;
                    temp->mem = c_obj->mem;
                    temp->list_obj = c_obj->list_obj;
                    temp->size = c_obj->size;
                    temp->is_expires = c_obj->is_expires;
                    Free(c_obj);
                    c_obj = temp;
                } else {
                    temp->next = c_obj;
                }
            }
            cache.size += c_obj->size;
            
            //fprintf(stderr, "Added: %u to cache\tsize: %u\n", write_args->hs, c_obj->size);
            if (write_args->no_cache == 0) {
                char log[MAXBUF*2] = {0};
                format_log_entry(log, &(write_args->clientaddr), write_args->uri, c_obj->size, "Miss");
                ssize_t loglen = strlen(log);
                Fwrite(log, loglen, 1, file);
                fflush(file);
            }
            
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
                        prev_obj= temp;
                        temp = temp->next;
                    }
                    prev_obj->next = NULL;
                }
                //fprintf(stderr, "Evicted: %u from cache\n", c_obj->hash);
                cache.size -= c_obj->size;
                Free(c_obj->mem);
                Free(c_obj->expires);
                Free(c_obj);
                Free(l_obj);
            }
            Close(serverfd);
            Close(write_args->connfd);
            Free(write_args);
        } else {
            Close(write_args->connfd);
            Free(c_obj->expires);
            handle_continue(c_obj->mem, serverfd, c_obj);
            Free(write_args);
        }
        return;
    }
    
    Close(write_args->connfd);
    handle_continue(write_args->response, serverfd, write_args);
}

static void reReadEvent(int serverfd, short event, void *arg)
{
    struct args *args = (struct args *) arg;
    Free(args->request);
    args->bytes_read = 0;
    struct write_args *write_args = (struct write_args *) Realloc(arg, sizeof(struct write_args));
    int index = write_args->hs%CACHE_SIZE;
    
    // read response from server
    write_args->response = (char *) Calloc(1, MAXBUF*2);
    write_args->marker = write_args->response;
    write_args->count = 2;
    //write_args->total_bytes = 0;
    int bytes_read, has_read = 0;
    if ((bytes_read = read(serverfd, write_args->marker, MAXBUF)) > 0) {
        has_read = 1;
        //fprintf(stderr, "Bytes_read: %d\n", bytes_read);
        write_args->total_bytes += bytes_read;
        write_args->marker = write_args->response + write_args->total_bytes;
        struct event *ev_read = event_new(write_args->base, serverfd, EV_READ, reWriteEvent, (void *)write_args);
        event_add(ev_read, NULL);
        return;
    }
    if ((errno != ECONNRESET && errno != 0)|| (!has_read)) {
        if (errno != ECONNRESET && errno != 0) {
            send_error(write_args->connfd, 500, "Internal Error");
        }
        if (errno != 0) {
            perror("errorno4: ");
        }
        Close(write_args->connfd);
        handle_continue(write_args->response, serverfd, write_args);
        return;
    }
    
    int resp_status = 0;
    //fprintf(stderr, "In rereadevent\n");
    sscanf(write_args->response, "%*s %d", &resp_status);
    //fprintf(stderr, "response: %d\n", resp_status);
    if (resp_status == 304 && write_args->c_obj) { //handles is_expires and !is_expires
        if (write(write_args->connfd, write_args->c_obj->mem, write_args->c_obj->size) < 0){
            send_error(write_args->connfd, 500, "Internal Error");
            perror("errorno5: ");
            Close(write_args->connfd);
            handle_continue(write_args->response, serverfd, write_args);
            return;
        }
        
        //fprintf(stderr, "retrieved: %u from cache", write_args->c_obj->hash);
        char log[MAXBUF*2] = {0};
        format_log_entry(log, &(write_args->clientaddr), write_args->uri, write_args->c_obj->size, "Hit");
        ssize_t loglen = strlen(log);
        Fwrite(log, loglen, 1, file);
        fflush(file);
        
        if (!write_args->c_obj->is_expires) {
            time_t now = time(NULL);
            struct tm *gmt = gmtime(&now);
            memcpy(write_args->c_obj->expires, gmt, sizeof(struct tm));
            //fprintf(stderr, "(no expires field\n");
        } else {
            //fprintf(stderr, "(with expires field\n)");
        }
        
        Close(write_args->connfd);
        handle_continue(write_args->response, serverfd, write_args);
        return;
    } else { /* Send Response to Client */
        if (write(write_args->connfd, write_args->response, write_args->total_bytes) < 0){
            send_error(write_args->connfd, 500, "Internal Error");
            perror("errorno6: ");
            Close(write_args->connfd);
            handle_continue(write_args->response, serverfd, write_args);
            return;
        } //Remove old Object from Cache
        if (write_args->c_obj) {
            //fprintf(stderr, "Evicting old entry\thash:%u\n", write_args->hs);
            struct list_obj *l_obj = write_args->c_obj->list_obj;
            
            if (l_obj->prev) {
                l_obj->prev->next = l_obj->next;
                if (l_obj->next) {
                    l_obj->next->prev = l_obj->prev;
                } else {
                    obj_list.tail = l_obj->prev;
                }
            } else {
                obj_list.head = l_obj->next;
                if (l_obj->next) {
                    l_obj->next->prev = NULL;
                } else {
                    obj_list.tail = NULL;
                }
            }

            struct cache_obj *temp = cache.bin[index];
            struct cache_obj *prev_obj = NULL;
            if (temp->hash == write_args->c_obj->hash) {
                if (temp->next) {
                    cache.bin[index] = temp->next;
                } else {
                    cache.bin[index] = NULL;
                }
            } else {
                while (temp->hash != write_args->c_obj->hash) {
                    prev_obj = temp;
                    temp = temp->next;
                }
                prev_obj->next = NULL;
            }
            cache.size -= write_args->c_obj->size;
            Free(temp->expires);
            Free(temp->mem);
            Free(temp);
            Free(l_obj);
            write_args->c_obj = NULL;
        }
    }

    struct list_obj *l_obj = NULL;
    struct cache_obj *c_obj = NULL;
    /* Add object to cache */
    if (write_args->total_bytes < MAX_OBJ_SIZE) {
        c_obj = (struct cache_obj *) Calloc(1, sizeof(struct cache_obj));
        c_obj->hash = write_args->hs;
        c_obj->mem = (char *) Realloc(write_args->response, write_args->total_bytes);
        c_obj->size = write_args->total_bytes;
        c_obj->expires = (struct tm *) Calloc(1, sizeof(struct tm));
        
        char *tempt = strstr(write_args->response, "Expires:");
        time_t now = time(NULL);
        double seconds = 0;
        if (tempt) {
            //fprintf(stderr, "Found Expires header\n");
            c_obj->is_expires = 1;
            strptime(tempt, "%*s %a, %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //char xyz[MAXLINE];
            //strftime(xyz, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //fprintf(stderr, "%s\n", xyz);
            //fprintf(stderr, "%s\n", write_args->response);
            seconds = difftime(mktime(c_obj->expires), now);
            //fprintf(stderr, "Add to cache if Seconds positive: %f\n", seconds);
        } else {
            struct tm *gmt = gmtime(&now);
            memcpy(c_obj->expires, gmt, sizeof(struct tm));
        }

        if (seconds > 0 || !tempt) {
            struct cache_obj *temp = NULL;
            l_obj = (struct list_obj *) Calloc(1, sizeof(struct list_obj));
            c_obj->list_obj = l_obj;
            l_obj->cache_obj = c_obj;
            
            l_obj->next = obj_list.head;
            if (obj_list.head != NULL) {
                obj_list.head->prev = l_obj;
            }
            if (obj_list.tail == NULL) {
                obj_list.tail = l_obj;
            }
            obj_list.head = l_obj;

            if ((temp = cache.bin[index]) == NULL) {
                cache.bin[index] = c_obj;
            } else {
                while (temp->next != NULL) {//This happends for responses that are not 200
                    if (temp->hash == c_obj->hash) {
                        Free(temp->expires);
                        Free(temp->mem);
                        struct list_obj *l_temp = temp->list_obj;
                        if (l_temp->prev) {
                            l_temp->prev->next = l_temp->next;
                        } else {
                            obj_list.head = l_temp->next;
                        }
                        if (l_temp->next) {
                            l_temp->next->prev = l_temp->prev;
                        } else {
                            obj_list.tail = l_temp->prev;
                        }
                        Free(l_temp);
                        cache.size -= temp->size;
                        temp->expires = c_obj->expires;
                        temp->mem = c_obj->mem;
                        temp->list_obj = c_obj->list_obj;
                        temp->size = c_obj->size;
                        temp->is_expires = c_obj->is_expires;
                        Free(c_obj);
                        c_obj = temp;
                        break;
                    }
                    temp = temp->next;
                }
                if (temp->hash == c_obj->hash && temp != c_obj) {
                    Free(temp->expires);
                    Free(temp->mem);
                    struct list_obj *l_temp = temp->list_obj;
                    if (l_temp->prev) {
                        l_temp->prev->next = l_temp->next;
                    } else {
                        obj_list.head = l_temp->next;
                    }
                    if (l_temp->next) {
                        l_temp->next->prev = l_temp->prev;
                    } else {
                        obj_list.tail = l_temp->prev;
                    }
                    Free(l_temp);
                    cache.size -= temp->size;
                    temp->expires = c_obj->expires;
                    temp->mem = c_obj->mem;
                    temp->list_obj = c_obj->list_obj;
                    temp->size = c_obj->size;
                    temp->is_expires = c_obj->is_expires;
                    Free(c_obj);
                    c_obj = temp;
                } else {
                    temp->next = c_obj;
                }
            }
            cache.size += c_obj->size;
            
            //fprintf(stderr, "Added: %u to cache\tsize: %u\n", write_args->hs, c_obj->size);
            if (!write_args->no_cache) {
                char log[MAXBUF*2] = {0};
                format_log_entry(log, &(write_args->clientaddr), write_args->uri, c_obj->size, "Miss");
                ssize_t loglen = strlen(log);
                Fwrite(log, loglen, 1, file);
                fflush(file);
            }

            /* LRU eviction */
            while (cache.size > MAX_CACHE_SIZE) {
                l_obj = obj_list.tail;
                c_obj = l_obj->cache_obj;
                
                if (l_obj->prev) {
                    l_obj->prev->next = NULL;
                } else {
                    obj_list.head = NULL;
                }
                obj_list.tail = l_obj->prev;
                
                index = c_obj->hash%CACHE_SIZE;
                temp = cache.bin[index];
                struct cache_obj *prev_obj = NULL;
                if (temp->hash == c_obj->hash) {
                    cache.bin[index] = NULL;
                } else {
                    while (temp->hash != c_obj->hash) {
                        prev_obj= temp;
                        temp = temp->next;
                    }
                    prev_obj->next = NULL;
                }
                //fprintf(stderr, "Evicted: %u from cache\n", c_obj->hash);
                cache.size -= c_obj->size;
                Free(c_obj->mem);
                Free(c_obj->expires);
                Free(c_obj);
                Free(l_obj);
            }
            Close(serverfd);
            Close(write_args->connfd);
            Free(write_args);
        } else {
            Close(write_args->connfd);
            Free(c_obj->expires);
            handle_continue(c_obj->mem, serverfd, c_obj);
            Free(write_args);
        }
        return;
    }
    
    Close(write_args->connfd);
    handle_continue(write_args->response, serverfd, write_args);
}

static void WriteEvent(int serverfd, short event, void *arg)
{
    struct args *args = (struct args *) arg;
    
    if (write(serverfd, args->request, args->bytes_read) < 0){
        send_error(args->connfd, 500, "Internal Error");
        handle_continue(args->request, args->connfd, args);
        close(serverfd);
        return;
    }
    //fprintf(stderr, "In WriteEvent\n");
    struct event *ev_read = event_new(args->base, serverfd, EV_READ, reReadEvent, (void *)args);
    event_add(ev_read, NULL);
}

static void ReadEvent(int connfd, short event, void *arg)
{
    struct args *args = (struct args *) arg;
    ssize_t h_size;
    size_t l_size;
    int simp_resp = 0;                                              /* Simple Response                       */
    char buf[MAXBUF] = {0}, request_line[MAXLINE] = {0};            /* Raw HTPP Request and Request-Line buf */
    char *buf_p = buf;                                              /* Pointer into buf for reading lines    */
    args->request = (char *) Calloc(1, MAXBUF);                     /* Request Sent to Server                */
    char *marker = args->request;                                   /* Pointer into request to append data   */
    char method[16] = {0}, version[16] = {0};                       /* Request-Line Fields                   */
    
    /* Read in initial Request */
    h_size = Read(connfd, buf, MAXBUF);
    if ((l_size = strcspn(buf, CRLF)) == h_size) {
        send_error(connfd, 400, "Bad Request");
        handle_continue(args->request, connfd, args);
        return;
    }
    memcpy(request_line, buf, l_size);   /* Extract Request-line */
    buf_p += l_size + 2;
    if (l_size + 2 == h_size) { //Telnet requests
        simp_resp = 1;
    }
    
    /* Parse Request-Line */
    int ret = sscanf(request_line, "%s %s %s", method, args->uri, version);
    if (ret == EOF) {
        send_error(connfd, 500, "Internal Error");
        handle_continue(args->request, connfd, args);
        return;
    } else if (ret < 3) { //This case shouldn't occur (HTTP 0.9) - tested and seems to be outdated.
        //fprintf(stderr, "Number of arguments in request_line is: %d\n", ret);
        simp_resp = 1;
    }
    string_toupper(method);
    
    /* Parse Uri */
    char hostname[MAXLINE] = {0}, pathname[MAXLINE] = {0};
    int r_port;
    string_tolower(args->uri);
    if ((parse_uri(args->uri, hostname, pathname, &r_port)) < 0) {
        send_error(connfd, 400, "Bad Request");
        handle_continue(args->request, connfd, args);
        return;
    }
    
    /* Open socket connection */
    int serverfd;
    if (open_clientfd_r(&serverfd, hostname, r_port) != 0) {
        send_error(connfd, 500, "Internal Error");
        handle_continue(args->request, connfd, args);
        return;
    }
    
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
            handle_continue(args->request, connfd, args);
            Close(serverfd);
            return;
        } else {
            send_error(connfd, 400, "Bad Request");
            handle_continue(args->request, connfd, args);
            Close(serverfd);
            return;
        }
    }
    
    /* Force HTTP version 1.0 */ /*
                                  if (strcmp(version, "HTTP/1.0") != 0) {
                                  memset(version, 0, sizeof(version));
                                  sprintf(version, "HTTP/1.0");
                                  }*/
    
    /* Check for presence of request objects in Cache */
    struct list_obj *l_obj = NULL;
    args->hs = hash(args->uri);
    int index = args->hs%CACHE_SIZE;
    if ((args->c_obj = cache.bin[index]) && strcmp(method, "GET") == 0) {
        do {
            if (args->c_obj->hash != args->hs) {
                args->c_obj = args->c_obj->next;
            } else {
                break;
            }
        } while (args->c_obj != NULL);
    } else {
        args->c_obj = NULL;
    }
    
    /* Write request_line into request to server */
    int bytes_written, content_length = 0;
    size_t bytes_read;
    if (simp_resp) {
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(args->request, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
            }
        } else {
            if ((bytes_written = sprintf(args->request, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
            }
        }
        marker = args->request + bytes_written;
    } else {
        //fprintf(stderr, "Checking Headers\n");
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(args->request, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
            }
        } else {
            if ((bytes_written = sprintf(args->request, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(connfd, 500, "Internal Error");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
            }
        }
        marker = args->request + bytes_written;
        
        int host_header = 0, connection = 0, c_len = 0, proxy = 0, pragma = 0;
        while ((l_size = strcspn(buf_p, CRLF)) != 0) {
            char head[64] = {0}, value[4096] = {0}; //Accounts for MAX cookie size
            if ((sscanf(buf_p, "%[^:]: %s", head, value) == 0)) {
                send_error(connfd, 400, "Bad Request");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
            }
            //fprintf(stderr, "head: %s\tvalue:%s\n", head, value);
            if (head[0] == 0) {
                send_error(connfd, 400, "Bad Request");
                handle_continue(args->request, connfd, args);
                Close(serverfd);
                return;
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
            if (!pragma) {
                if (strcasecmp(head, "Pragma") == 0){
                    pragma = 1;
                    if (strcasecmp(value, "no-cache") == 0){
                        args->no_cache = 1;
                    }
                }
            }
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
                        handle_continue(args->request, connfd, args);
                        Close(serverfd);
                        return;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: \r\n", head)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(args->request, connfd, args);
                        Close(serverfd);
                        return;
                    }
                }
            } else {
                if (valid_header == 1) {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", header[i], value)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(args->request, connfd, args);
                        Close(serverfd);
                        return;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", head, value)) < 0) {
                        send_error(connfd, 500, "Internal Error");
                        handle_continue(args->request, connfd, args);
                        Close(serverfd);
                        return;
                    }
                }
            }
            marker += bytes_written;
            buf_p += l_size + 2;
        }
        bytes_read = buf_p - buf;
        if ((bytes_read == h_size) || !host_header) {
            send_error(connfd, 400, "Bad Request");
            handle_continue(args->request, connfd, args);
            Close(serverfd);
            return;
        }
        buf_p += 2;
        bytes_read += 2;
        if (args->c_obj && !args->no_cache) {
            if (!args->c_obj->is_expires) {
                //fprintf(stderr, "Adding addtional header field\n");
                if ((bytes_written = strftime(marker, MAXBUF - bytes_read, "If-Modified-Since: %a, %d %b %Y %H:%M:%S %Z\r\n", args->c_obj->expires)) <= 0) {
                    send_error(connfd, 500, "Internal Error");
                    handle_continue(args->request, connfd, args);
                    Close(serverfd);
                    return;
                }
                marker += bytes_written;
            }
        }
    }
    sprintf(marker, "\r\n");
    marker += 2;
    //fprintf(stderr, "Headerfield checked\n");
    
    /* Log Entry for Cache-Hit*/
    if (args->c_obj) {
        if (!args->no_cache) {
            //Update LRU list
            l_obj = args->c_obj->list_obj;
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
            
            double seconds = 0;
            if (args->c_obj->is_expires) {
                time_t now = time(NULL);
                seconds = difftime(mktime(args->c_obj->expires), now);
                //fprintf(stderr, "Cache hit if Seconds positive: %f\n", seconds);
            }
            
            if (seconds > 0) { //Our Cached object is still valid
                if (write(connfd, args->c_obj->mem, args->c_obj->size) < 0){
                    send_error(connfd, 500, "Internal Error");
                    handle_continue(args->request, connfd, args);
                    Close(serverfd);
                    return;
                }
                //fprintf(stderr, "retrieved: %u from cache(with expires field)\n", args->c_obj->hash);
                
                char log[MAXBUF*2] = {0};
                format_log_entry(log, &(args->clientaddr), args->uri, args->c_obj->size, "Hit");
                ssize_t loglen = strlen(log);
                Fwrite(log, loglen, 1, file);
                fflush(file);
                
                Close(serverfd);
                handle_continue(args->request, args->connfd, args);
                return;
            }
        } else {
            /* Create Log Entry For no-Cache*/
            char log[MAXBUF*2] = {0};
            format_log_entry(log, &(args->clientaddr), args->uri, args->c_obj->size, "No-cache");
            ssize_t loglen = strlen(log);
            Fwrite(log, loglen, 1, file);
            fflush(file);
        }
    }
    
    if (simp_resp) {
        args->bytes_read = strlen(args->request);
    } else { //Assume request is smaller than 8k buffer(no multiple reads for req).
        if (content_length != 0 && (content_length < MAXBUF - (marker - args->request))) {
            memcpy(marker, buf_p, content_length);
        } else if (content_length > 0){
            //fprintf(stderr, "REQUEST BUFFER IS TOO SMALL\n");
        }
        args->bytes_read = marker - args->request + content_length;
    }
    //fprintf(stderr, "creating WriteEvent\n");
    struct event *ev_write = event_new(args->base, serverfd, EV_WRITE, WriteEvent, (void *)args);
    event_add(ev_write, NULL);
}

static void AcceptEvent(int listenfd, short event, void *arg)
{
    struct event *ev_read;
    struct args *args = (struct args *) Calloc(1, sizeof(struct args));
    args->base = arg;
    socklen_t clientlen = sizeof(struct sockaddr_in);
    //fprintf(stderr, "Accepting Event\n");
    args->connfd = Accept(listenfd, (struct sockaddr *) &(args->clientaddr), &clientlen);
    fcntl(args->connfd, F_SETFD, O_NONBLOCK);
    ev_read = event_new(args->base, args->connfd, EV_READ, ReadEvent, (void *)args);
    event_add(ev_read, NULL);
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

    Signal(SIGPIPE, SIG_IGN);
    
    if ((file = fopen("proxy.log", "a+")) == NULL) {
        perror("fopen: ");
        exit(1);
    }

    obj_list.head = NULL;
    obj_list.tail = NULL;
    cache.size = 0;
    memset(cache.bin, 0, CACHE_SIZE);
    
    /* Create Event */
    struct event *ev_accept;
    struct event_base *base = event_base_new();
    
    /* Open listen socket */
    int listenfd;
    fcntl(listenfd, F_SETFL, O_NONBLOCK);
    
    if ((listenfd = Open_listenfd(port)) < 0) {
        perror("Open_listenfd error: ");
        exit(1);
    }
    
    /* Register the event */
    ev_accept = event_new(base, listenfd, EV_READ | EV_PERSIST, AcceptEvent, base);
    event_add(ev_accept, NULL);
    
    /* Start event loop */
    event_base_dispatch(base);
    
    // Never Reached
    event_del(ev_accept);
    Fclose(file);
    struct list_obj *temp = obj_list.head;
    struct list_obj *next_obj = NULL;
    while (temp != NULL) {
        next_obj = temp->next;
        Free(temp->cache_obj->mem);
        Free(temp->cache_obj->expires);
        Free(temp->cache_obj);
        Free(temp);
        temp = next_obj;
    }
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
                      char *uri, int size, char *status)
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
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d %s\n", time_str, a, b, c, d, uri, size, status);
}


