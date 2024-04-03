/*
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

#include "csapp.h"
#include "http_parser.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)
#define HOSTLEN 256
#define SERVLEN 8

/* Typedef for convenience */
typedef struct sockaddr SA;

/* Information about a connected client. */
typedef struct {
    struct sockaddr_in addr; // Socket address
    socklen_t addrlen;       // Socket address length
    int connfd;              // Client connection file descriptor
    char host[HOSTLEN];      // Client host
    char serv[SERVLEN];      // Client service (port)
} client_info;

typedef struct CacheObject {
    char *url;
    char *content; // Dynamically allocated to store the web object
    int size;      // Size of the web object
    struct CacheObject *prev, *next; // Pointers for the LRU doubly linked list
    int ref_count;                   // Reference count
} CacheObject;

typedef struct {
    CacheObject *head, *tail; // Head and tail for LRU tracking
    int total_size;           // Current total size of all cached objects
    pthread_mutex_t lock;     // Mutex for thread-safe access
} Cache;

/* URI parsing results. */
typedef enum { PARSE_ERROR, PARSE_STATIC, PARSE_DYNAMIC } parse_result;

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20230411 Firefox/63.0.1";
Cache global_cache;

void remove_from_cache(CacheObject *obj);
void cache_release(CacheObject *obj);

/*
 * init_cache - initialize the cache
 */
void init_cache() {
    global_cache.head = global_cache.tail = NULL;
    global_cache.total_size = 0;
    pthread_mutex_init(&global_cache.lock, NULL);
}

/*
 * free_cache - freeing the cache
 */
void free_cache() {
    // Acquire lock
    pthread_mutex_lock(&global_cache.lock);

    // Free each cache object and its contents
    CacheObject *current = global_cache.head;
    while (current != NULL) {
        CacheObject *temp = current;
        current = current->next;
        free(temp->url);
        free(temp->content);
        free(temp);
    }

    // disable lock
    pthread_mutex_unlock(&global_cache.lock);
    pthread_mutex_destroy(&global_cache.lock);
}

/*
 * find_in_cache - Look up a URL in the cache
 */
CacheObject *find_in_cache(const char *url) {
    pthread_mutex_lock(&global_cache.lock); // Lock the cache

    CacheObject *current = global_cache.head;
    while (current != NULL) {
        if (strcmp(current->url, url) == 0) {
            current->ref_count++; // Increment reference count
            pthread_mutex_unlock(&global_cache.lock); // Unlock the cache
            return current;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&global_cache.lock); // Unlock the cache if not found
    return NULL;                              // Not found
}

/*
 * cache_evict - Remove the least recently used object from the cache
 */
void cache_evict(int size_needed) {
    pthread_mutex_lock(&global_cache.lock);
    while (global_cache.total_size + size_needed > MAX_CACHE_SIZE &&
           global_cache.tail != NULL) {
        CacheObject *lru = global_cache.tail;

        // Update pointers of neighboring objects in the list
        if (lru->prev) {
            lru->prev->next = lru->next;
            global_cache.tail = lru->prev;
        }

        // Update the cache's head and tail pointers

        // Update the total cache size
        global_cache.total_size -= lru->size;

        // Free the LRU object's resources
        // decrement the counter
        if (lru->ref_count == 0) {
            free(lru->url);
            free(lru->content);
            free(lru);
        }
    }
    pthread_mutex_unlock(&global_cache.lock);
}

/*
 * add_to_cache - Add a new object to the cache
 */
void add_to_cache(const char *url, const char *content, int size) {
    pthread_mutex_lock(&global_cache.lock);

    // Check if the object is too large to be cached
    if (size > MAX_OBJECT_SIZE) {
        pthread_mutex_unlock(&global_cache.lock);
        return;
    }

    // Evict objects if necessary to make room for the new object
    pthread_mutex_unlock(&global_cache.lock);

    cache_evict(size);
    pthread_mutex_lock(&global_cache.lock);

    // Create a new cache object
    CacheObject *new_obj = malloc(sizeof(CacheObject));
    if (!new_obj) {
        pthread_mutex_unlock(&global_cache.lock);
        return; // Failed to allocate memory
    }

    new_obj->url = strdup(url);
    new_obj->content = malloc(size);
    if (new_obj->content == NULL) {
        free(new_obj->url);
        free(new_obj);
        pthread_mutex_unlock(&global_cache.lock);
        return; // Failed to allocate memory
    }
    memcpy(new_obj->content, content, size);
    new_obj->size = size;
    new_obj->ref_count = 1; // Initialize reference count

    // Add the new object to the front of the list (head) for LRU policy
    new_obj->prev = NULL;
    new_obj->next = global_cache.head;
    if (global_cache.head != NULL) {
        global_cache.head->prev = new_obj;
    }
    // if the list is empty, set the tail to the new object
    global_cache.head = new_obj;
    if (global_cache.tail == NULL) {
        global_cache.tail = new_obj;
    }

    // Update the total size of the cache
    global_cache.total_size += size;

    pthread_mutex_unlock(&global_cache.lock);
}

/*
 * cache_release - decrement the reference count of a cache object and remove
 * it if the reference count reaches 0
 */
void cache_release(CacheObject *obj) {
    pthread_mutex_lock(&global_cache.lock);
    obj->ref_count--;
    if (obj->ref_count == 0) {
        // Free the LRU object's resources
        // decrement the counter
        free(obj->url);
        free(obj->content);
        free(obj);
    }
    pthread_mutex_unlock(&global_cache.lock);
}

/*
 * clienterror - returns an error message to the client
 */
void clienterror(int fd, const char *errnum, const char *shortmsg,
                 const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Tiny Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s</p>\r\n"
                       "<hr /><em>The Tiny Web server</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

/*
 * copy_obj - copy a CacheObject structure
 */
CacheObject *copy_obj(CacheObject *original) {

    pthread_mutex_lock(&global_cache.lock);
    if (original == NULL) {
        pthread_mutex_unlock(&global_cache.lock);
        return NULL;
    }

    CacheObject *copy = malloc(sizeof(CacheObject));
    if (copy == NULL) {
        pthread_mutex_unlock(&global_cache.lock);
        return NULL; // Memory allocation failed
    }

    copy->url = strdup(original->url);
    copy->content = malloc(original->size);
    if (copy->content == NULL) {
        free(copy->url);
        free(copy);
        pthread_mutex_unlock(&global_cache.lock);
        return NULL; // Memory allocation failed
    }

    memcpy(copy->content, original->content, original->size);
    copy->size = original->size;
    copy->ref_count = 1;            // Initialize reference count for the copy
    copy->prev = copy->next = NULL; // New object will be placed at the head
    pthread_mutex_unlock(&global_cache.lock);
    return copy;
}

/*
 * remove_from_cache - remove an object from the cache
 */
void remove_from_cache(CacheObject *obj) {
    pthread_mutex_lock(&global_cache.lock);
    if (obj->prev) {
        obj->prev->next = obj->next;
    } else {
        // obj is the head
        global_cache.head = obj->next;
    }

    if (obj->next) {
        obj->next->prev = obj->prev;
    } else {
        // obj is the tail
        global_cache.tail = obj->prev;
    }

    global_cache.total_size -= obj->size;
    obj->ref_count--;
    if (obj->ref_count == 0) {
        // Free the LRU object's resources
        free(obj->url);
        free(obj->content);
        free(obj);
    }
    pthread_mutex_unlock(&global_cache.lock);
}

/*
 * serve - handle one HTTP request/response transaction
 */
void serve(client_info *client) {
    rio_t rio;
    rio_readinitb(&rio, client->connfd);

    /* Read request line */
    char buf[MAXLINE];
    if (rio_readlineb(&rio, buf, sizeof(buf)) <= 0) {
        return;
    }

    /* Parse the request line and check if it's well-formed */
    parser_t *parser = parser_new();

    parser_state parse_state = parser_parse_line(parser, buf);

    if (parse_state != REQUEST) {
        parser_free(parser);
        clienterror(client->connfd, "400", "Bad Request",
                    "server received a malformed request");
        return;
    }

    const char *method, *path, *uri;
    parser_retrieve(parser, METHOD, &method);
    parser_retrieve(parser, PATH, &path);
    parser_retrieve(parser, URI, &uri);

    /* Check that the method is GET */
    if (strcmp(method, "GET") != 0) {
        parser_free(parser);
        clienterror(client->connfd, "501", "Not Implemented",
                    "server does not implement this method");
        return;
    }
    // CHECK FOR THE URI IN THE CACHE
    char *uri_copy = strdup(uri);
    CacheObject *cached_obj = find_in_cache(uri);

    // Inside the serve function
    if (cached_obj != NULL) {
        CacheObject *copy_cached_obj = copy_obj(cached_obj);
        if (copy_cached_obj != NULL) {
            // Add the copy to the beginning of the cache
            add_to_cache(copy_cached_obj->url, copy_cached_obj->content,
                         copy_cached_obj->size);

            // Remove the original object from the cache
            remove_from_cache(cached_obj);
            // object from the cache

            // Use the copy to send data to the client
            rio_writen(client->connfd, copy_cached_obj->content,
                       copy_cached_obj->size);
        }

        cache_release(cached_obj);      // Decrement reference count of original
        cache_release(copy_cached_obj); // Decrement reference count of copy
        free(uri_copy);
        parser_free(parser);
        return;
    }

    // send request to the server if the object is not in the cache

    const char *port, *host;
    parser_retrieve(parser, PORT, &port);
    parser_retrieve(parser, HOST, &host);

    int serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        parser_free(parser);
        clienterror(serverfd, "404", "not found",
                    "server could not parse request headers");

        return;
    }

    rio_t rio_server;
    rio_readinitb(&rio_server, serverfd);

    // Construct the new HTTP request
    char request[PARSER_MAXLINE];
    sprintf(request, "%s %s HTTP/1.0\r\n", method,
            uri); // Convert to HTTP/1.0
    // Add necessary headers
    sprintf(request + strlen(request), "Host: %s\r\n", host);
    sprintf(request + strlen(request), "User-Agent: %s\r\n", header_user_agent);
    sprintf(request + strlen(request), "Connection: close\r\n");
    sprintf(request + strlen(request), "Proxy-Connection: close\r\n");

    // Add additional headers from the client's request
    char header_buf[MAXLINE];
    while (true) {
        if (rio_readlineb(&rio, header_buf, sizeof(header_buf)) <= 0) {
            break; // Break the loop if no more headers or error occurs
        }

        /* Check for end of request headers */
        if (strcmp(header_buf, "\r\n") == 0) {
            break; // End of headers
        }

        // Parse the request header
        parser_state parse_state = parser_parse_line(parser, header_buf);
        if (parse_state != HEADER) {
            clienterror(client->connfd, "400", "Bad Request",
                        "server could not parse request headers");
            break; // Break on parsing error
        }

        header_t *header = parser_retrieve_next_header(parser);
        if (header == NULL) {
            continue; // Skip if no header is retrieved
        }

        // Check if the header is not one of the four predefined headers
        if (strcasecmp(header->name, "User-Agent") != 0 &&
            strcasecmp(header->name, "Connection") != 0 &&
            strcasecmp(header->name, "Proxy-Connection") != 0) {

            // If it's a different header, add it to the request
            strncat(request, header->name,
                    PARSER_MAXLINE - strlen(request) - 1);
            strncat(request, ": ", PARSER_MAXLINE - strlen(request) - 1);
            strncat(request, header->value,
                    PARSER_MAXLINE - strlen(request) - 1);
            strncat(request, "\r\n", PARSER_MAXLINE - strlen(request) - 1);
        }
    }

    strncat(request, "\r\n", PARSER_MAXLINE - strlen(request) - 1);

    // Forward the request to the target server
    rio_writen(serverfd, request, strlen(request));

    // Free the parser
    parser_free(parser);

    /* Receive response from end server and forward to client */
    size_t n;
    char *response = malloc(MAX_OBJECT_SIZE);
    int response_len = 0;
    while ((n = rio_readnb(&rio_server, buf, MAXLINE)) != 0) {
        rio_writen(client->connfd, buf, n);
        if (response_len + n <= MAX_OBJECT_SIZE) {
            memcpy(response + response_len, buf, n);
            response_len += n;
        }
    }

    /* Add to cache if the response is not too big */
    if (response_len <= MAX_OBJECT_SIZE) {
        add_to_cache(uri_copy, response, response_len);
    }

    /* Clean up */
    free(response);
    free(uri_copy);

    close(serverfd);
}

void *thread(void *vargp) {
    client_info *client = (client_info *)vargp;
    pthread_detach(pthread_self());

    // Handle the client request
    serve(client);

    // Close the connection to the client
    close(client->connfd);

    // Free the allocated client_info structure
    free(client);

    return NULL;
}

int main(int argc, char **argv) {
    pthread_t tid;
    int listenfd;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    // Open listening file descriptor
    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }

    init_cache();
    while (1) {
        client_info *client = Malloc(sizeof(client_info));
        client->addrlen = sizeof(client->addr);

        client->connfd =
            accept(listenfd, (SA *)&client->addr, &client->addrlen);
        if (client->connfd < 0) {
            perror("accept");
            free(client); // Clean up allocated memory on failure
            continue;
        }

        // Call the thread routine
        pthread_create(&tid, NULL, thread, (void *)client);
    }
    // Destroy the cache before exiting
    free_cache();
    return 0;
}
