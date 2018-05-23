/*
 * proxy.c - CS:APP Web proxy
 *
 * Ben Ruktantichoke 20121092
 *
 * TODO: Ignores no-cache field
 */

#include "csapp.h"
#include <assert.h>
#include <pthread.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <fcntl.h>

#define MAX_CACHE_SIZE 5242280 // Maximum size of Cache
#define MAX_OBJ_SIZE 524228    // Maximum object size in Cache
#define CACHE_SIZE 16384       // Number of Hash table entries

FILE * file = NULL;            // Log File
struct event_base *base = NULL;     //libevent base
struct evdns_base *dns_base = NULL; //asynchronous dns handler (I don't know how to use this)

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

struct write_args {
    struct bufferevent *bev;         // Recieving Connection
    struct bufferevent *pev;         // Transmitting Connection
    struct sockaddr_in clientaddr;   // Client's address
    char uri[MAXLINE];
    uint32_t hs;
    int no_cache;
    struct cache_obj *c_obj;
    char *response;
    long total_bytes;
    char *marker;
    int init;      // If initialized
    int count;     // For memory allocation
};

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size, char *status);
struct evbuffer *ReadEv(struct bufferevent *bev, int *buf_len);
static void Event(struct bufferevent *bev, short events, void *ptr);
static void ReadEvent(struct bufferevent *bev, void *ptr);
static void CloseEvent(struct bufferevent *bev, void *ptr);

/* Send HTTP style response to bev. bev should be client. */
/* prints error if it occurs */
void send_error(struct bufferevent *bev, int code, char *message)
{
    struct evbuffer *tx;
    tx = bufferevent_get_output(bev);
    size_t bytes;
    char error[256] = {0};
    bytes = sprintf(error, "HTTP/1.0 %d %s\r\n\r\n"
                    "<!DOCTYPE html>\n"
                    "<html>\n"
                    "<body>\n"
                    "<h1>%d %s</h1>\n"
                    "</body>\n"
                    "</html>", code, message, code, message);
    evbuffer_add(tx, error, bytes);
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

/* djb2 hash function for strings */
unsigned long hash(unsigned char *str)
{
    unsigned long hash = 5381;
    int c;
    
    while (c = *str++)
        hash = ((hash << 5) + hash) ^ c; /* hash * 33 + c */
    
    return hash;
}


static void reReadEvent(struct bufferevent *bev, void *arg)
{
    //fprintf(stderr, "In reReadEvent\n");
    struct evbuffer *rx, *tx;
    struct write_args *write_args = (struct write_args *) arg;
    int index = write_args->hs%CACHE_SIZE;
    if (write_args->init != 1) {
        //fprintf(stderr, "init\n");
        write_args->total_bytes = 0;
        write_args->pev = write_args->bev;
        write_args->bev = bev;
        write_args->response = (char *) Calloc(1, MAXBUF);
        write_args->marker = write_args->response;
        write_args->init = 1;
        write_args->count = 2;
    }
    
    // read response from server
    rx = bufferevent_get_input(bev);
    int bytes_read = evbuffer_get_length(rx);
    if (bytes_read > 0 && write_args->count != -1) {
        int temp_bytes = write_args->total_bytes;
        write_args->total_bytes += bytes_read;
        if (write_args->total_bytes < 3221225472) { //3Gb limit on file<< MUST ALLOCATE THIS!
            if (write_args->total_bytes > MAXBUF * write_args->count/2) {
                write_args->count *= 2;
                write_args->response = Realloc(write_args->response, MAXBUF * write_args->count);
                //fprintf(stderr, "total bytes: %ld Reallocating %d bytes\n", write_args->total_bytes, MAXBUF * write_args->count);
                write_args->marker = write_args->response + temp_bytes;
            }
            int retz = bufferevent_read(bev, write_args->marker, bytes_read);
            //fprintf(stderr, "actual:%d\tbytes_read: %d\n", retz, bytes_read);
            write_args->marker = write_args->response + write_args->total_bytes;
            //fprintf(stderr, "not done reading\n");
            return;
        } else {
            //fprintf(stderr, "Too large object\n");
        }
    }
    int retx = bufferevent_read(bev, write_args->marker, bytes_read);
    //fprintf(stderr, "actual: %d\tbytes_read: %d\n", retx, bytes_read);
    write_args->total_bytes += bytes_read;
    write_args->marker = write_args->response + write_args->total_bytes;
    if (bytes_read == -1) {
        send_error(write_args->pev, 500, "Internal Error");
        Free(write_args->response);
        ////Free(write_args);
        return;
    }
    
    int resp_status = 0;
    sscanf(write_args->response, "%*s %d", &resp_status);
    //fprintf(stderr, "%s\n", write_args->response);
    //fprintf(stderr, "response: %d\n", resp_status);
    if (resp_status == 304 && write_args->c_obj) { //handles is_expires and !is_expires
        tx = bufferevent_get_output(write_args->pev);
        evbuffer_add(tx, write_args->c_obj->mem, write_args->c_obj->size);
        bufferevent_write_buffer(write_args->pev, tx);
        
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
        
        Free(write_args->response);
        //Free(write_args);
        return;
    } else { /* Send Response to Client */
        //fprintf(stderr, "Sending response to Client\n");
        tx = bufferevent_get_output(write_args->pev);
        evbuffer_add(tx, write_args->response, write_args->total_bytes);
        bufferevent_write_buffer(write_args->pev, tx);
        //fprintf(stderr, "%s\n", write_args->response);
        
        //struct evbuffer *ttx = bufferevent_get_output(write_args->pev);
        //int bufy_len = evbuffer_get_length(ttx);
        //fprintf(stderr, "To client\ttotal_bytes:%ld\tbuf_len: %d\n", write_args->total_bytes, bufy_len);
        
        //Remove old Object from Cache
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
        //double seconds = 0;
        if (tempt) {
            //fprintf(stderr, "Found Expires header\n");
            c_obj->is_expires = 1;
            strptime(tempt, "%*s %a, %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //char xyz[MAXLINE];
            //strftime(xyz, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", c_obj->expires);
            //fprintf(stderr, "%s\n", xyz);
            //seconds = difftime(mktime(c_obj->expires), now);
            //fprintf(stderr, "Add to cache if Seconds positive: %f\n", seconds);
        } else {
            struct tm *gmt = gmtime(&now);
            memcpy(c_obj->expires, gmt, sizeof(struct tm));
        }
        
        if (/*seconds > 0 || !tempt*/1) {
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
            //Free(write_args);
        } else {
            Free(c_obj->expires);
            Free(c_obj->mem);
            Free(c_obj);
            //Free(write_args);
        }
        return;
    }
    
    Free(write_args->response);
    //Free(write_args);
}

static void ReadEvent(struct bufferevent *bev, void *ptr)
{
    //fprintf(stderr, "In ReadEvent\n");
    struct write_args *write_args = ptr;
    struct evbuffer *rx, *tx;                  // Bufferrs for Receiving (rx) and Transmitting (tx)
    ssize_t h_size;
    size_t l_size;
    int simp_resp = 0;                                              /* Simple Response                       */
    char request_line[MAXLINE] = {0};                               /* Request-Line buf                      */
    char srequest[MAXBUF];                                          /* Request Sent to Server                */
    char buf[MAXBUF] = {0};                      //Stores Request from client
    char *marker = srequest;                                        /* Pointer into request to append data   */
    char method[16] = {0}, version[16] = {0};                       /* Request-Line Fields                   */
    char *buf_p = buf;                           // Pointer into Client Request
    
    /* Read in initial Request */
    rx = bufferevent_get_input(bev);
    bufferevent_read_buffer(bev, rx);
    h_size = evbuffer_get_length(rx);
    evbuffer_copyout(rx, buf, h_size);  // Better implementation is to use this buffer str8 up
    bufferevent_disable(bev, EV_READ);  // Since we assume client is not sending huge stuff,
                                        // we're not gonna read from client anymore
    
    //fprintf(stderr, "Copied Request\n");
    if ((l_size = strcspn(buf, CRLF)) == h_size) {
        send_error(bev, 400, "Bad Request");
        //Free(write_args);
        return;
    }
    /* Extract Request-line */
    memcpy(request_line, buf, l_size);
    buf_p += l_size + 2;
    if (l_size + 2 == h_size) { //Telnet requests
        simp_resp = 1;
    }
    
    /* Parse Request-Line */
    int ret = sscanf(request_line, "%s %s %s", method, write_args->uri, version);
    if (ret == EOF) {
        send_error(bev, 500, "Internal Error");
        //Free(write_args);
        return;
    } else if (ret < 3) { //This case shouldn't occur (HTTP 0.9) - tested and seems to be outdated.
        //fprintf(stderr, "Number of arguments in request_line is: %d\n", ret);
        simp_resp = 1;
    }
    string_toupper(method);
    
    /* Parse Uri */
    char hostname[MAXLINE] = {0}, pathname[MAXLINE] = {0};
    int r_port;
    string_tolower(write_args->uri);
    if ((parse_uri(write_args->uri, hostname, pathname, &r_port)) < 0) {
        send_error(bev, 400, "Bad Request");
        //Free(write_args);
        return;
    }
    
    /* Check for presence of request objects in Cache */
    struct list_obj *l_obj = NULL;
    write_args->hs = hash(write_args->uri);
    int index = write_args->hs%CACHE_SIZE;
    if ((write_args->c_obj = cache.bin[index]) && strcmp(method, "GET") == 0) {
        do {
            if (write_args->c_obj->hash != write_args->hs) {
                write_args->c_obj = write_args->c_obj->next;
            } else {
                break;
            }
        } while (write_args->c_obj != NULL);
    } else {
        write_args->c_obj = NULL;
    }
    
    /* Log Entry for Cache-Hit*/
    if (write_args->c_obj) {
        if (!write_args->no_cache) {
            //Update LRU list
            l_obj = write_args->c_obj->list_obj;
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
            /*
            double seconds = 0;
            if (write_args->c_obj->is_expires) {
                time_t now = time(NULL);
                seconds = difftime(mktime(write_args->c_obj->expires), now);
                //fprintf(stderr, "Cache hit if Seconds positive: %f\n", seconds);
            }
            */
            if (/*seconds > 0*/1) { //Our Cached object is still valid
                tx = bufferevent_get_output(bev);
                evbuffer_add(tx, write_args->c_obj->mem, write_args->c_obj->size);
                bufferevent_write_buffer(bev, tx);
                bufferevent_setcb(bev, NULL, CloseEvent, Event, NULL);
                //fprintf(stderr, "retrieved: %u from cache(with expires field)\n", write_args->c_obj->hash);
                
                char log[MAXBUF*2] = {0};
                format_log_entry(log, &(write_args->clientaddr), write_args->uri, write_args->c_obj->size, "Hit");
                ssize_t loglen = strlen(log);
                Fwrite(log, loglen, 1, file);
                fflush(file);
                
                //Free(write_args);
                return;
            }
        } else {
            /* Create Log Entry For no-Cache*/
            char log[MAXBUF*2] = {0};
            format_log_entry(log, &(write_args->clientaddr), write_args->uri, write_args->c_obj->size, "No-cache");
            ssize_t loglen = strlen(log);
            Fwrite(log, loglen, 1, file);
            fflush(file);
        }
    }

    /* Open socket connection */
    //fprintf(stderr, "opening connection\n");
    write_args->pev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!write_args->pev) {
        //fprintf(stderr, "socket new failed\n");
    }
    if (bufferevent_socket_connect_hostname(write_args->pev, NULL, AF_UNSPEC, hostname, r_port) < 0) {
        //fprintf(stderr, "socket connect failed\n");
    }
    bufferevent_setcb(write_args->pev, reReadEvent, NULL, Event, write_args);
    bufferevent_enable(write_args->pev, EV_READ|EV_WRITE);
    //fprintf(stderr, "opened connection\n");

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
            send_error(bev, 501, "Not Implemented");
            bufferevent_free(write_args->pev);
            write_args->pev = NULL;
            //Free(write_args);
            return;
        } else {
            send_error(bev, 400, "Bad Request");
            bufferevent_free(write_args->pev);
            write_args->pev = NULL;
            //Free(write_args);
            return;
        }
    }
    
    /* Force HTTP version 1.0 */ /*
                                  if (strcmp(version, "HTTP/1.0") != 0) {
                                  memset(version, 0, sizeof(version));
                                  sprintf(version, "HTTP/1.0");
                                  }*/
    
    /* Write request_line into request to server */
    int bytes_written, content_length = 0;
    size_t bytes_read;
    if (simp_resp) {
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(srequest, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(bev, 500, "Internal Error");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                Free(write_args);
                return;
            }
        } else {
            if ((bytes_written = sprintf(srequest, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(bev, 500, "Internal Error");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                Free(write_args);
                return;
            }
        }
        marker = srequest + bytes_written;
    } else {
        //fprintf(stderr, "Checking Headers\n");
        if (pathname[0] == 0) {
            if ((bytes_written = sprintf(srequest, "%s / HTTP/1.0\r\n", method)) < 0) {
                send_error(bev, 500, "Internal Error");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                //Free(write_args);
                return;
            }
        } else {
            if ((bytes_written = sprintf(srequest, "%s /%s HTTP/1.0\r\n", method, pathname)) < 0) {
                send_error(bev, 500, "Internal Error");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                ////Free(write_args);
                return;
            }
        }
        marker = srequest + bytes_written;
        
        int host_header = 0, connection = 0, c_len = 0, proxy = 0, pragma = 0;
        while ((l_size = strcspn(buf_p, CRLF)) != 0) {
            char head[64] = {0}, value[4096] = {0}; //Accounts for MAX cookie size
            if ((sscanf(buf_p, "%[^:]: %s", head, value) == 0)) {
                send_error(bev, 400, "Bad Request");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                //Free(write_args);
                return;
            }
            //fprintf(stderr, "head: %s\tvalue:%s\n", head, value);
            if (head[0] == 0) {
                send_error(bev, 400, "Bad Request");
                bufferevent_free(write_args->pev);
                write_args->pev = NULL;
                //Free(write_args);
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
                        write_args->no_cache = 1;
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
                        send_error(bev, 500, "Internal Error");
                        bufferevent_free(write_args->pev);
                        write_args->pev = NULL;
                        //Free(write_args);
                        return;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: \r\n", head)) < 0) {
                        send_error(bev, 500, "Internal Error");
                        bufferevent_free(write_args->pev);
                        write_args->pev = NULL;
                        //Free(write_args);
                        return;
                    }
                }
            } else {
                if (valid_header == 1) {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", header[i], value)) < 0) {
                        send_error(bev, 500, "Internal Error");
                        bufferevent_free(write_args->pev);
                        write_args->pev = NULL;
                        //Free(write_args);
                        return;
                    }
                } else {
                    if ((bytes_written = sprintf(marker, "%s: %s\r\n", head, value)) < 0) {
                        send_error(bev, 500, "Internal Error");
                        bufferevent_free(write_args->pev);
                        write_args->pev = NULL;
                        //Free(write_args);
                        return;
                    }
                }
            }
            marker += bytes_written;
            buf_p += l_size + 2;
        }
        bytes_read = buf_p - buf;
        if ((bytes_read == h_size) || !host_header) {
            send_error(bev, 400, "Bad Request");
            bufferevent_free(write_args->pev);
            //Free(write_args);
            return;
        }
        buf_p += 2;
        bytes_read += 2;
        if (write_args->c_obj && !write_args->no_cache) {
            if (!write_args->c_obj->is_expires) {
                //fprintf(stderr, "Adding addtional header field\n");
                if ((bytes_written = strftime(marker, MAXBUF - bytes_read, "If-Modified-Since: %a, %d %b %Y %H:%M:%S %Z\r\n", write_args->c_obj->expires)) <= 0) {
                    send_error(bev, 500, "Internal Error");
                    bufferevent_free(write_args->pev);
                    write_args->pev = NULL;
                    //Free(write_args);
                    return;
                }
                marker += bytes_written;
            }
        }
    }
    sprintf(marker, "\r\n");
    marker += 2;
    //fprintf(stderr, "Headerfield checked\n");
    
    if (simp_resp) {
        write_args->total_bytes = strlen(srequest);
    } else { //Assume request is smaller than 8k buffer(no multiple reads for req).
        if (content_length != 0 && (content_length < MAXBUF - (marker - srequest))) {
            memcpy(marker, buf_p, content_length);
        } else if (content_length > 0){
            //fprintf(stderr, "REQUEST BUFFER IS TOO SMALL\n");
        }
        write_args->total_bytes = marker - srequest + content_length;
    }

    //fprintf(stderr, "writing data to output buffer\n");
    tx = bufferevent_get_output(write_args->pev);
    evbuffer_add(tx, srequest, write_args->total_bytes);
    bufferevent_write_buffer(write_args->pev, tx);
    
    // Sanity Checks
    //struct evbuffer *ttx = bufferevent_get_output(write_args->pev);
    //int bufy_len = evbuffer_get_length(ttx);
    //fprintf(stderr, "Relaying from Client to Server: total_bytes: %d\ttx_len: %d\n", write_args->total_bytes, bufy_len);
}

static void CloseEvent(struct bufferevent *bev, void *ptr)
{
    struct write_args* write_args = ptr;
    struct evbuffer *b = bufferevent_get_output(bev);
    
    if (evbuffer_get_length(b) == 0) {
        bufferevent_free(bev);
    }
}

static void Event(struct bufferevent *bev, short events, void *ptr) //write_args-> init == 0 or -1 means bev is client
{                                                                   // maintains write_args->bev == bev
    //fprintf(stderr, "In Event\n");
    struct write_args* write_args = ptr;
    if (events & BEV_EVENT_CONNECTED) {
        //fprintf(stderr, "Connected to server\n");
        write_args->init = -1;
    } else if (events & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
        if (events & BEV_EVENT_ERROR) {
            int err = bufferevent_socket_get_dns_error(bev);
            if (err) {
                //fprintf(stderr, "DNS error: %s\n", evutil_gai_strerror(err));
            }
        }
        if (write_args->init > 0) { //Maybe we have to use some CloseEvent Semantics
            //fprintf(stderr, "Server connection closed. Entering last read/write.\n");
            write_args->count = -1;
            reReadEvent(bev, write_args);
            bufferevent_setcb(write_args->pev, NULL, CloseEvent, Event, NULL);
            //Maybe have to wait until all data is written (CloseEvent)
        } else if (write_args->init == -1) {
            //fprintf(stderr, "Client Closed Connection. Closing Both Server and Clients.\n");
            bufferevent_free(write_args->pev);
        } else {
            //fprintf(stderr, "Client closed connection. No server.\n");
        }
        
        Free(write_args);
        bufferevent_free(bev);
    }
}

static void AcceptEvent(struct evconnlistener *listener, evutil_socket_t fd,
                        struct sockaddr *a, int slen, void *p)
{
    //fprintf(stderr, "In Accept Event\n");
    struct write_args *write_args = (struct write_args *) Calloc(1, sizeof(struct write_args));
    write_args->bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    memcpy(&write_args->clientaddr, a, sizeof(struct sockaddr_in));
    bufferevent_setcb(write_args->bev, ReadEvent, NULL, Event, write_args);
    bufferevent_enable(write_args->bev, EV_READ|EV_WRITE);
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
    
    struct sockaddr_storage listen_on_addr;
    memset(&listen_on_addr, 0, sizeof(listen_on_addr));
    struct sockaddr_in *sin = (struct sockaddr_in*)&listen_on_addr;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = htonl(INADDR_ANY);
    sin->sin_family = AF_INET;
    socklen_t clientlen = sizeof(struct sockaddr_in);
    
    /* Create Event */
    base = event_base_new();
    dns_base = evdns_base_new(base, 1);
    
    /* Create Listener */
    struct evconnlistener *listener = evconnlistener_new_bind(base, AcceptEvent, NULL, LEV_OPT_CLOSE_ON_FREE, -1,
                                                              (struct sockaddr *)&listen_on_addr, clientlen);
    
    /* Start event loop */
    event_base_dispatch(base);
    
    // Never Reached
    evconnlistener_free(listener);
    event_base_free(base);
    evdns_base_free(dns_base);
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


