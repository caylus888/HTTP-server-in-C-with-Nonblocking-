#include <stdio.h>     
#include <sys/socket.h> // unix system for socket functions
#include <netinet/in.h> // setting up a ipV4 in the server
#include <stdlib.h>     //  memory handling and error
#include <string.h>     // string operations
#include <unistd.h>     // basic output
#include <fcntl.h>      // setting socket to non-blocking mode
#include <time.h>       // time keeping
#include <errno.h>      // checking errors from system calls
#include <sys/epoll.h>  // handling multiple clients

// definitions
#define MAX_EVENTS 60
#define MAX_BUFFER_SIZE 5000
#define MAX_CLIENTS 1000

// Forward declaration of the server structure
struct serv;

typedef struct {
    int fd;                         // file descriptor
    char read_buffer[MAX_BUFFER_SIZE];
    char write_buffer[MAX_BUFFER_SIZE];
    int read_size;                  
    int write_size;                 
    int write_pos;                  
    int state;                      // 0 = reading, 1 = writing
    time_t last_active;             
} connection_t;

// Server structure definition
struct serv {
    int sdomain;               
    int sserver;               
    int sprotocol;           
    int sport;                 
    int sbacklog;             
    unsigned long s_interface; 
    struct sockaddr_in add;    
    void (*pp)(struct serv*); 
    int socketfd;              
    int epoll_fd;            
};

// Function prototypes
void pp(struct serv *server);
int set_nonblock(int fd);
void handle_new_connection(struct serv *server, connection_t **connections);
void handle_client_data(struct serv *server, connection_t *conn);
void handle_client_write(struct serv *server, connection_t *conn);
void close_connection(struct serv *server, connection_t *conn);
connection_t *create_connection(int fd);

// Server constructor
struct serv serv_construct(int s_domain, int s_server, int s_protocol, 
                          int s_port, int s_backlog, 
                          unsigned long s_interface, void (*s_pp)(struct serv*)) {
    printf("Initializing server...\n");
    
    struct serv server;
    server.sdomain = s_domain;
    server.sserver = s_server; 
    server.s_interface = s_interface;   
    server.sprotocol = s_protocol; 
    server.sport = s_port;
    server.sbacklog = s_backlog;
    
   
    server.add.sin_family = s_domain;
    server.add.sin_port = htons(s_port);          // host data to network byte order
    server.add.sin_addr.s_addr = htonl(s_interface); // same
    
    // Create socket
    server.socketfd = socket(s_domain, s_server, s_protocol);
    if (server.socketfd < 0) {
        perror("Failed to create the socket");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    if (setsockopt(server.socketfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        exit(EXIT_FAILURE);
    }
    
    // Create epoll instance
    server.epoll_fd = epoll_create1(0);
    if (server.epoll_fd == -1) {
        perror("Failed to create an epoll instance");
        close(server.socketfd);
        exit(EXIT_FAILURE);
    }
    //  non-blocking mode
    if (set_nonblock(server.socketfd) < 0) {
        close(server.socketfd);
        close(server.epoll_fd);
        exit(EXIT_FAILURE);
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server.socketfd;
    if (epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, server.socketfd, &ev) == -1) {
        perror("Failed to add socket to epoll");
        close(server.socketfd);
        close(server.epoll_fd);
        exit(EXIT_FAILURE);
    }
   
    int sockaddr_size = sizeof(server.add);
    if (bind(server.socketfd, (const struct sockaddr *)&(server.add), sockaddr_size) < 0) {
        perror("Failed to bind the socket");
        close(server.socketfd);
        close(server.epoll_fd);
        exit(EXIT_FAILURE);
    }
    // listening for connections
    if (listen(server.socketfd, server.sbacklog) < 0) {
        perror("Failed to start listening");
        close(server.socketfd);
        close(server.epoll_fd);
        exit(EXIT_FAILURE);
    }
  
    server.pp = s_pp;
    
    printf("Server initialized successfully on port %d\n", s_port);
    return server;
}

// Set socket to non-blocking mode
int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL) failed");
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL) failed");
        return -1;
    }
    
    return 0;
}


void pp(struct serv *server) {
    struct epoll_event events[MAX_EVENTS];
    connection_t *connections[MAX_CLIENTS] = {0};
    
    printf("Server waiting for connections...\n");
    
    while (1) {
        int nfds = epoll_wait(server->epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failed");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            // Error handling
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                fprintf(stderr, "epoll error or hangup on fd %d\n", events[i].data.fd);
                
                // Find and close the connection
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (connections[j] && connections[j]->fd == events[i].data.fd) {
                        close_connection(server, connections[j]);
                        connections[j] = NULL;
                        break;
                    }
                }
                continue;
            }
            
            // New connection
            if (events[i].data.fd == server->socketfd) {
                handle_new_connection(server, connections);
                continue;
            }
            
            // Find the connection
            connection_t *conn = NULL;
            int conn_index = -1;
            
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (connections[j] && connections[j]->fd == events[i].data.fd) {
                    conn = connections[j];
                    conn_index = j;
                    break;
                }
            }
            
            if (!conn) {
                fprintf(stderr, "No connection context found for fd %d\n", events[i].data.fd);
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                close(events[i].data.fd);
                continue;
            }
            
            conn->last_active = time(NULL);
            
            if (events[i].events & EPOLLIN && conn->state == 0) {
                handle_client_data(server, conn);
            }
            
            if (events[i].events & EPOLLOUT && conn->state == 1) {
                handle_client_write(server, conn);
                
                if (conn->fd == 0) {
                    connections[conn_index] = NULL;
                }
            }
        }
        
        time_t current_time = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (connections[i] && (current_time - connections[i]->last_active > 60)) {
                fprintf(stderr, "Closing idle connection: %d\n", connections[i]->fd);
                close_connection(server, connections[i]);
                connections[i] = NULL;
            }
        }
    }
}

void handle_new_connection(struct serv *server, connection_t **connections) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = accept(server->socketfd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept failed");
        }
        return;
    }
    
    // Set non-blocking
    if (set_nonblock(client_fd) < 0) {
        close(client_fd);
        return;
    }
    
    // Create connection context
    connection_t *conn = create_connection(client_fd);
    if (!conn) {
        close(client_fd);
        return;
    }
    
    // Add to connections array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!connections[i]) {
            connections[i] = conn;
            break;
        }
    }
    
    // Add to epoll for read events
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
    ev.data.fd = client_fd;  // Store file descriptor in epoll data
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        perror("epoll_ctl: add client socket failed");
        close(client_fd);
        return;
    }
    
    printf("New connection accepted: %d\n", client_fd);
}

connection_t* create_connection(int fd) {
    static connection_t connections[MAX_CLIENTS];
    static int initialized = 0;
    
    // Initialize array on first use
    if (!initialized) {
        memset(connections, 0, sizeof(connections));
        initialized = 1;
    }
    
    // Find free slot
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (connections[i].fd == 0) {
            // Clear the connection
            memset(&connections[i], 0, sizeof(connection_t));
            connections[i].fd = fd;
            connections[i].state = 0;  // Reading state
            connections[i].last_active = time(NULL);
            return &connections[i];
        }
    }
    
    fprintf(stderr, "Too many clients\n");
    return NULL;
}

void handle_client_data(struct serv *server, connection_t *conn) {
    int bytes_read = read(conn->fd, 
                        conn->read_buffer + conn->read_size, 
                        MAX_BUFFER_SIZE - conn->read_size - 1);
    
    if (bytes_read <= 0) {
        if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Connection closed or error
            close_connection(server, conn);
        }
        return;
    }
    
    conn->read_size += bytes_read;
    conn->read_buffer[conn->read_size] = '\0';
    conn->last_active = time(NULL);

    // Check for complete HTTP request
    if (strstr(conn->read_buffer, "\r\n\r\n") || strstr(conn->read_buffer, "\n\n")) {
        printf("Received complete request from client %d\n", conn->fd);
        
        const char *response_body = "What's the veggie that failed the exam?\nGavar!";
        char response[1024];
        
        // Format HTTP response with proper headers
        snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Server: Custom C Server\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            strlen(response_body),
            response_body);
        
        conn->write_size = strlen(response);
        strncpy(conn->write_buffer, response, MAX_BUFFER_SIZE);
        conn->write_pos = 0;
        conn->state = 1;  // Switch to writing state
        
        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.fd = conn->fd;
        
        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) == -1) {
            perror("epoll_ctl: mod failed");
            close_connection(server, conn);
            return;
        }
    }
}

void handle_client_write(struct serv *server, connection_t *conn) {
    while (conn->write_pos < conn->write_size) {
        int bytes_written = write(conn->fd,
                                conn->write_buffer + conn->write_pos,
                                conn->write_size - conn->write_pos);
        
        if (bytes_written <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close_connection(server, conn);
            }
            return;
        }
        
        conn->write_pos += bytes_written;
        conn->last_active = time(NULL);
    }
    
    printf("Response sent to client %d\n", conn->fd);
    close_connection(server, conn);
}

void close_connection(struct serv *server, connection_t *conn) {
    if (!conn || conn->fd == 0) return;
    
    printf("Closing connection: %d\n", conn->fd);
    
    // Remove from epoll
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    
    // Close socket
    if (conn->fd != 0) {
        close(conn->fd);
        conn->fd = 0;
    }
    
    // Clear connection data
    memset(conn, 0, sizeof(connection_t));
}

int main() {
    struct serv server = serv_construct(AF_INET, SOCK_STREAM, 0, 6969, 10, INADDR_ANY, pp);
    
    server.pp(&server);
    close(server.socketfd);
    close(server.epoll_fd);
    
    return 0;
}