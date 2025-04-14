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
#include <sys/stat.h>   // for file stats

// definitions
#define MAX_EVENTS 60
#define MAX_BUFFER_SIZE 5000
#define MAX_CLIENTS 1000
#define MAX_PATH_LENGTH 1024  // Maximum length for the document root path
#define ERROR_BODY_SIZE 2048  // Size for error message body

// Global variable to store the document root path
char document_root[MAX_PATH_LENGTH] = "";

// Forward declaration of the server structure
struct serv;

typedef struct {
    int fd;        // file descriptor
    char read_buffer[MAX_BUFFER_SIZE];
    char write_buffer[MAX_BUFFER_SIZE];
    int read_size;
    int write_size;
    int write_pos;
    int state;     // 0 = reading, 1 = writing
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
char* read_html_file(const char* htmlfile, size_t* file_size);
void handle_http_request(struct serv *server, connection_t *conn);

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

// socket to non-blocking mode
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

        // Check for idle connections
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

    connection_t *conn = create_connection(client_fd);
    if (!conn) {
        close(client_fd);
        return;
    }

    // Find an empty slot for the new connection
    int slot_found = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!connections[i]) {
            connections[i] = conn;
            slot_found = 1;
            break;
        }
    }

    if (!slot_found) {
        fprintf(stderr, "No free connection slots available\n");
        close(client_fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
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

    if (!initialized) {
        memset(connections, 0, sizeof(connections));
        initialized = 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (connections[i].fd == 0) {
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

char* read_html_file(const char* htmlfile, size_t* file_size) {
    FILE* file = fopen(htmlfile, "rb");
    if (!file) {
        printf("ERROR: Failed to open file: %s (Error: %s)\n", htmlfile, strerror(errno));
        return NULL;
    }

    // Get file size
    struct stat st;
    if (stat(htmlfile, &st) != 0) {
        printf("ERROR: Failed to get file stats: %s\n", strerror(errno));
        fclose(file);
        return NULL;
    }
    *file_size = st.st_size;

    printf("Successfully opened file: %s (Size: %zu bytes)\n", htmlfile, *file_size);

    // Allocate memory for file content
    char* content = (char*)malloc(*file_size + 1);
    if (!content) {
        printf("ERROR: Failed to allocate memory for file content\n");
        fclose(file);
        return NULL;
    }

    // Read file content
    size_t bytes_read = fread(content, 1, *file_size, file);
    fclose(file);

    if (bytes_read != *file_size) {
        printf("ERROR: Failed to read entire file. Read %zu bytes of %zu\n", bytes_read, *file_size);
        free(content);
        return NULL;
    }

    content[*file_size] = '\0';
    return content;
}

void handle_http_request(struct serv *server, connection_t *conn) {
    // Parse the request to get the requested path
    char method[16], path[256], protocol[16];
    if (sscanf(conn->read_buffer, "%15s %255s %15s", method, path, protocol) != 3) {
        printf("Invalid HTTP request format\n");
        // Send a 400 Bad Request response
        const char* bad_request = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        strcpy(conn->write_buffer, bad_request);
        conn->write_size = strlen(bad_request);
        conn->write_pos = 0;
        conn->state = 1;  // Switch to writing state

        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.fd = conn->fd;
        epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        return;
    }

    // Add debug logging
    printf("HTTP Request received: %s %s %s\n", method, path, protocol);

    // Default to index.html if root is requested
    if (strcmp(path, "/") == 0) {
        strcpy(path, "/index.html");  // Use a more standard default file name
        printf("Root path requested, defaulting to: %s\n", path);
    }

    // Basic security check - prevent directory traversal
    if (strstr(path, "..") != NULL) {
        printf("Security warning: Attempted directory traversal detected in path: %s\n", path);
        const char* forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        strcpy(conn->write_buffer, forbidden);
        conn->write_size = strlen(forbidden);
        conn->write_pos = 0;
        conn->state = 1;

        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.fd = conn->fd;
        epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        return;
    }

    // Construct the file path using the document_root
    char file_path[MAX_PATH_LENGTH + 256];
    snprintf(file_path, sizeof(file_path), "%s%s", document_root, path);
    printf("Attempting to open file: %s\n", file_path);

    // Read the file
    size_t file_size;
    char* content = read_html_file(file_path, &file_size);

    if (content) {
        printf("File found! Sending 200 OK response\n");
        // File found, send 200 OK with the file content
        const char* content_type;

        // Determine content type based on file extension
        if (strstr(path, ".html") || strstr(path, ".htm")) {
            content_type = "text/html";
        } else if (strstr(path, ".css")) {
            content_type = "text/css";
        } else if (strstr(path, ".js")) {
            content_type = "application/javascript";
        } else if (strstr(path, ".jpg") || strstr(path, ".jpeg")) {
            content_type = "image/jpeg";
        } else if (strstr(path, ".png")) {
            content_type = "image/png";
        } else if (strstr(path, ".gif")) {
            content_type = "image/gif";
        } else if (strstr(path, ".ico")) {
            content_type = "image/x-icon";
        } else {
            content_type = "text/plain";
        }

        // First create the headers
        char response_headers[1024];
        int headers_len = snprintf(response_headers, sizeof(response_headers),
            "HTTP/1.1 200 OK\r\n"
            "Server: Custom C Server\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, file_size);

        // Check if response will fit in the buffer
        if (headers_len + file_size > MAX_BUFFER_SIZE) {
            printf("ERROR: Response too large for buffer\n");
            const char* error_response = 
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 22\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Internal Server Error";
            strcpy(conn->write_buffer, error_response);
            conn->write_size = strlen(error_response);
            free(content);
        } else {
            // Copy headers
            memcpy(conn->write_buffer, response_headers, headers_len);
            // Copy file content
            memcpy(conn->write_buffer + headers_len, content, file_size);
            conn->write_size = headers_len + file_size;
            free(content);
        }
    } else {
        // File not found, send 404
        printf("File not found! Sending 404 response\n");
        
        // First create the error body with a safe size limit
        char error_body[ERROR_BODY_SIZE];
        int error_body_len = snprintf(error_body, ERROR_BODY_SIZE,
            "<html><body><h1>404 Not Found</h1>"
            "<p>The requested resource could not be found.</p>"
            "<p>Requested path: %s</p></body></html>",
            path);
        
        // Ensure it's null-terminated
        error_body[ERROR_BODY_SIZE - 1] = '\0';
        
        // Then create the headers and complete response
        int response_len = snprintf(conn->write_buffer, MAX_BUFFER_SIZE,
            "HTTP/1.1 404 Not Found\r\n"
            "Server: Custom C Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            error_body_len, error_body);
            
        // Safety check
        if (response_len >= MAX_BUFFER_SIZE) {
            printf("WARNING: 404 response was truncated\n");
            conn->write_buffer[MAX_BUFFER_SIZE - 1] = '\0';
            conn->write_size = MAX_BUFFER_SIZE - 1;
        } else {
            conn->write_size = response_len;
        }
    }

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

void handle_client_data(struct serv *server, connection_t *conn) {
    int bytes_read = read(conn->fd,
                        conn->read_buffer + conn->read_size,
                        MAX_BUFFER_SIZE - conn->read_size - 1);

    if (bytes_read <= 0) {
        if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
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
        handle_http_request(server, conn);
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

int main(int argc, char *argv[]) {
    // Set default document root if none provided
    if (argc >= 2) {
        strncpy(document_root, argv[1], MAX_PATH_LENGTH - 1);
        document_root[MAX_PATH_LENGTH - 1] = '\0';  // Ensure null termination
    } else {
        // Default path if none provided - use current directory instead of hardcoded Windows path
        getcwd(document_root, MAX_PATH_LENGTH);
    }

    // Remove trailing slash if present to avoid double slashes in paths
    int len = strlen(document_root);
    if (len > 0 && (document_root[len-1] == '/' || document_root[len-1] == '\\')) {
        document_root[len-1] = '\0';
    }

    printf("Starting server with document root: %s\n", document_root);
    printf("Default file will be served when accessing root URL: /index.html\n");

    // Check if the default file exists
    char default_path[MAX_PATH_LENGTH + 20];
    snprintf(default_path, sizeof(default_path), "%s/index.html", document_root);

    FILE* test_file = fopen(default_path, "rb");
    if (test_file) {
        fclose(test_file);
        printf("Default file exists: %s\n", default_path);
    } else {
        printf("WARNING: Default file does not exist or cannot be accessed: %s\n", default_path);
        printf("Error: %s\n", strerror(errno));
    }

    struct serv server = serv_construct(AF_INET, SOCK_STREAM, 0, 8080, 10, INADDR_ANY, pp);
    server.pp(&server);
    close(server.socketfd);
    close(server.epoll_fd);
    
    return 0;
}