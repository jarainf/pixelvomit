#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>

#define CEILING(x,y) (((x) + (y) - 1) / (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

// options for the program
// framerate for the waiting main thread. Doesn't do anything
#define FRAMERATE 60
#define FB_DEV "/dev/fb0"
#define PORT 42024
#define GRAB_SIZE 2048 * 8 
#define NUM_THREADS 2
#define TOTAL_CLIENTS 10000
#define CLIENTS_PER_THREAD CEILING(TOTAL_CLIENTS, NUM_THREADS)
#define EVENTS_PER_THREAD 40
#define MAX_EVENTS MAX(EVENTS_PER_THREAD, CLIENTS_PER_THREAD)

// lookup-table for hex
static const uint8_t hex_table[256] __attribute__((aligned(64))) = {
    [0 ... 255] = 255,
    ['0'] = 0,
    ['1'] = 1,
    ['2'] = 2,
    ['3'] = 3,
    ['4'] = 4,
    ['5'] = 5,
    ['6'] = 6,
    ['7'] = 7,
    ['8'] = 8,
    ['9'] = 9,
    ['A'] = 10,
    ['B'] = 11,
    ['C'] = 12,
    ['D'] = 13,
    ['E'] = 14,
    ['F'] = 15,
    ['a'] = 10,
    ['b'] = 11,
    ['c'] = 12,
    ['d'] = 13,
    ['e'] = 14,
    ['f'] = 15};

// structs for framebuffer information
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;

// the framebuffer mmap
uint32_t *vbuffer;

// struct to handle client data
typedef struct {
    int fd;
    int offset_x;
    int offset_y;
    size_t message_length;
    char message[64];
} client_state;

// struct to handle thread data
typedef struct {
    int epoll_fd;
    int active_connections;
    client_state *clients;
    struct epoll_event events[MAX_EVENTS];
} client_thread;

// struct with all data for cleanup
typedef struct {
    int epoll_fd;
    int fb_fd;
    int server_fd;
    pthread_t *threads;
    client_state *clients;
    client_thread *thread_data;
} cleanup_data;

// global variable to store all data for cleanup
cleanup_data program_data;

// declarations
void get_framebuffer_properties();
void *handle_connections(void *arg);
void *handle_clients(void *arg);
void handle_client(client_thread *thread_data, client_state *client);
void write_vbuffer();
void cleanup_error(int errorcode);
void cleanup();
int find_client(client_state *clients, int client_fd);
int find_spot(client_thread *thread_data, int *thread, int *client);
void parse_int_int(const char *input, int *a, int *b);
void parse_int_int_hex(const char *input, int *a, int *b, int *c);
void write_to_vbuffer(int x, int y, uint32_t color);

// initialisation of most things.
int main() {
    printf("Total clients - target: %d actual: %d\n",TOTAL_CLIENTS, CLIENTS_PER_THREAD * NUM_THREADS);
    printf("Clients per Thread: %d\n",CLIENTS_PER_THREAD);
    int fb_fd;
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    // pthread_t thread;

    program_data = (cleanup_data) {
        .epoll_fd = 0,
        .server_fd = 0,
        .thread_data = NULL
    };

    signal(SIGINT, cleanup);  // handle interrupts to clean up resources

    // open the framebuffer device
    fb_fd = open(FB_DEV, O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer device");
        return EXIT_FAILURE;
    }

    program_data.fb_fd = fb_fd;

    // get framebuffer properties
    get_framebuffer_properties();

    vbuffer = (uint32_t *)mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (vbuffer == MAP_FAILED) {
        perror("Error mapping framebuffer device to memory");
        close(fb_fd);
        return EXIT_FAILURE;
    }

    // create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation error");
        close(fb_fd);
        return EXIT_FAILURE;
    }

    // bind the socket
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        close(fb_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        close(server_fd);
        close(fb_fd);
        return EXIT_FAILURE;
    }

    // add to program_data for cleanup
    program_data.server_fd = server_fd;

    printf("Listening socket_fd: %d\n", server_fd);

    /*/ start thread to handle incoming connections
    pthread_create(&thread, NULL, handle_connections, &server_fd);
    pthread_detach(thread);

    // do nothing - successfully
    while (1)
    {
        usleep(1000000 / FRAMERATE);
    }*/

    // start handling incoming connections
    handle_connections(&server_fd);   

    cleanup();
    return 0;
}

void get_framebuffer_properties() {
    // get variable screen information
    if (ioctl(program_data.fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information");
        close(program_data.fb_fd);
        exit(EXIT_FAILURE);
    }

    // get fixed screen information
    if (ioctl(program_data.fb_fd, FBIOGET_FSCREENINFO, &finfo)) {
        perror("Error reading fixed information");
        close(program_data.fb_fd);
        exit(EXIT_FAILURE);
    }

    // output screen info
    printf("Display info: %dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    printf("Line length: %d\n", finfo.line_length);
    printf("Framebuffer size: %d\n", finfo.smem_len);
    printf("RGBA order: %d%d%d%d\n", vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset, vinfo.transp.offset);
}

// thread to handle client connections. Takes socket file descriptors as arg
void *handle_connections(void *arg) {
    pthread_t threads[NUM_THREADS];
    int thread_args[NUM_THREADS];
    int server_fd = *(int *)arg;
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(program_data.fb_fd);
        close(program_data.server_fd);
        exit(EXIT_FAILURE);
    }

    // add to program_data for cleanup
    program_data.epoll_fd = epoll_fd;

    // event structs for information
    struct epoll_event event, events[MAX_EVENTS];
    // events to listen for
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    event.data.fd = server_fd;

    // register events to listen for
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: add server_fd failed");
        cleanup_error(EXIT_FAILURE);
    }

    // generate structs to store data for client handling threads.
    client_thread thread_data[NUM_THREADS] = {0};
    client_state* clients_buffer = (client_state*) malloc(CLIENTS_PER_THREAD * NUM_THREADS * sizeof(client_state));
    memset(clients_buffer, 0, CLIENTS_PER_THREAD * NUM_THREADS * sizeof(client_state));

    // add to program_data for cleanup
    program_data.thread_data = thread_data;
    program_data.clients = clients_buffer;

    printf("Socket handle connection established. FD: %d\n", epoll_fd);

    // start multiple threads to handle connections
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            perror("epoll_create1 failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        // give every thread its slice of the client_state buffer
        thread_data[i].clients = &clients_buffer[CLIENTS_PER_THREAD * i];

        printf("Start thread: %d FD: %d\n", i, epoll_fd);
        
        pthread_create(&threads[i], NULL, handle_clients, &thread_data[i]);
        pthread_detach(threads[i]);
    }

    // add threads to program_data for cleanup
    program_data.threads = threads;

    // while-loop to accept new clients
    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            close(epoll_fd);
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        // work through all events
        for (int i = 0; i < n; i++) {
            /* Shit never worked, is wrong and we probably never get these events here
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                printf("Connection dropped: %d", events[i].data.fd);
                for (int i = 0; i < NUM_THREADS; i++) {

                    if (find_client(thread_data[i].clients, events[i].data.fd) != -1) {
                        thread_data[i].clients->fd = 0;
                        thread_data[i].active_connections -= 1;
                        break;
                    }
                }
                continue;
            }*/

            // accept a new connection
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd == -1) {
                perror("accept failed");
                continue;
            }

            event.events = EPOLLIN;
            event.data.fd = client_fd;

            // Logic to select appropriate thread and client spot
            int thread = 0;
            int client = 0;

            if (find_spot(thread_data, &thread, &client) == 0) {
                printf("New spot, thread: %d spot: %d\n", thread, client);
                // add the client to the threads epoll file descriptor
                if (epoll_ctl(thread_data[thread].epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                    perror("epoll_ctl: add client_fd failed");
                    close(client_fd);
                } else {
                    // It didn't fail so we initialise the data
                    thread_data[thread].clients[client].fd = client_fd;
                    thread_data[thread].clients[client].offset_x = 0;
                    thread_data[thread].clients[client].offset_y = 0;
                    thread_data[thread].clients[client].message_length = 0;

                    // keep track of active connections on a thread
                    thread_data[thread].active_connections += 1;

                    printf("Client accepted, FD: %d on epoll: %d. Thread: %d Client: %d\n", client_fd, epoll_fd, thread, client);
                }
            } else {
                // we have no space for another client - drop
                perror("no spot found - closing connection");
                close(client_fd);
            }
        }
    }

    // should we ever break out of the while loop, do cleanup
    cleanup_error(EXIT_FAILURE);
    return NULL;
}

// thread to handle clients. Only handles active connections, doesn't accept
void *handle_clients(void *arg) {
    // cast thread_data for our purposes
    client_thread *thread_data = (client_thread *) arg;

    // directly enter the loop to wait for events
    while (1) {
        int n = epoll_wait(thread_data->epoll_fd, thread_data->events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            cleanup_error(EXIT_FAILURE);
        }

        // work through all events
        for (int i = 0; i < n; i++) {
            // we have an event, find the client it's associated with
            int client_fd = find_client(thread_data->clients, thread_data->events[i].data.fd);
            
            // check if we found a client
            if (client_fd != -1) {
                // we found a client, handle its data
                handle_client(thread_data, &thread_data->clients[client_fd]);
            } else {
                // we didn't find the client
                perror("client_fd not in clients");
                continue;
            }
        }
    }

    // should we ever break out of this thread, also close epoll_fd
    close(thread_data->epoll_fd);
    return NULL;
}

// function to handle clients data
void handle_client(client_thread *thread_data, client_state *client) {
    ssize_t bytes_received;
    char buffer[GRAB_SIZE + 64] = {0};

    // write the rest of the message into buffer before we receive data
    if (client->message_length > 0) {
        memcpy(buffer, client->message, strlen(client->message));
    }
    
    // receive data and append it to the message
    bytes_received = recv(client->fd, buffer + client->message_length, GRAB_SIZE, 0);
    if (bytes_received <= 0) {
        // find the client
        int client_num = find_client(thread_data->clients, client->fd);
        // close the connection
        client->fd = close(client->fd);
        // check if we found a client
        if (client_num != -1) {
            thread_data->clients[client_num].fd = 0;
            thread_data->active_connections -= 1;
        }
        return;
    }

    bytes_received += client->message_length;

    // terminate the buffer so we don't loop over all of it
    buffer[bytes_received] = '\0';

    // loop over the received message, line by line
    char *newline_pos;
    while ((newline_pos = strchr(buffer, '\n')) != NULL) {
        // overwrite the position of the newline with '\0'
        *newline_pos = '\0';
        char *line = buffer;

        // see what kind of message we received
        // TODO: probably better to have "PX" first
        //       also might consider not using strncmp
        if (*line == 'P') {
            // Someone requested to write a pixel
            int x, y, color;
            // parse position and color of the pixel
            // TODO: return current value if no color has been sent
            parse_int_int_hex(line + 3, &x, &y, &color);

            // apply the offset
            x += client->offset_x;
            y += client->offset_y;

            // if it's within bounds, write the new color
            if (x < vinfo.xres && y < vinfo.yres) {
                write_to_vbuffer(x, y, color);
            }
        } else if (*line == 'O') {
            // we received an offset, write it into the client data struct
            parse_int_int(line + 7, &client->offset_x, &client->offset_y);
        } else if (*line == 'S') {
            // SIZE was requested, return it
            char size_response[32];
            snprintf(size_response, sizeof(size_response), "SIZE %d %d\n", vinfo.xres, vinfo.yres);
            send(client->fd, size_response, strlen(size_response), 0);
        } else {
            // we received a string we didn't expect
            printf("Token: %s\n", line);
        }
        memmove(buffer, newline_pos + 1, bytes_received);
    }
    client->message_length = strlen(buffer);
    memcpy(client->message, buffer, client->message_length);
}

// find a client in the client_states provided
// return the position if found or -1 if the client is not registered
int find_client(client_state *clients, int client_fd) {
    for (int i = 0; i < CLIENTS_PER_THREAD; i++) {
        if (clients[i].fd == client_fd) {
            return i;
        }        
    }
    return -1;
}

// find a spot for a new client. first empty spot gets written into *thread *client
// returns 0 if we found a spot, -1 otherwise
int find_spot(client_thread *thread_data, int *thread, int *client) {
    int lowest = __INT_MAX__;
    *thread = 0;
    *client = 0;
    // find the best thread with an open slot
    for (int i = 0; i < NUM_THREADS; i++) {
        if (thread_data[i].active_connections != 0) {
            if (thread_data[i].active_connections <= lowest && thread_data[i].active_connections != CLIENTS_PER_THREAD) {
                lowest = thread_data[i].active_connections;
                *thread = i;
            }
        } else {
            lowest = 0;
            *thread = i;
            break;
        }
    }
    // if a suitable thread has been found
    if (lowest < __INT_MAX__) {
        // find the open spot
        for (*client = 0; *client < CLIENTS_PER_THREAD; *client += 1) {
            if (thread_data[*thread].clients[*client].fd == 0) {
                break;
            }
        }

        return(0);
    }
    // all slots are full
    return -1;
}

// parse a char* starting at the position of the first integer
// char* has to consist of 2 integers delimited by 1 non-number
void parse_int_int(const char *input, int *a, int *b) {
    // parse first integer
    *a = 0;
    while (*input >= '0' && *input <= '9') {
        *a = (*a << 3) + (*a << 1) + (*input & 0xF); // result * 10 + digit
        input++;
    }

    // ignore whitespace
    input++;

    // parse second integer
    *b = 0;
    while (*input >= '0' && *input <= '9') {
        *b = (*b << 3) + (*b << 1) + (*input & 0xF); // result * 10 + digit
        input++;
    }
}

// parse a char* starting at the position of the first integer
// char* has to consist of 2 integers and a string delimited by 1 non-number each
void parse_int_int_hex(const char *input, int *a, int *b, int *c) {
    // parse first integer
    *a = 0;
    while (*input >= '0' && *input <= '9') {
        *a = (*a << 3) + (*a << 1) + (*input & 0xF); // result * 10 + digit
        input++;
    }

    // ignore whitespace
    input++;

    // parse second integer
    *b = 0;
    while (*input >= '0' && *input <= '9') {
        *b = (*b << 3) + (*b << 1) + (*input & 0xF); // result * 10 + digit
        input++;
    }

    // ignore whitespace
    input++;

    // parse Hex
    *c = 0;
    while (*input) {
        uint8_t value = hex_table[(unsigned char)*input];
        if (value != 255) {
            *c = (*c << 4) | value;
            input++;
        } else {
            break;
        }
    }
}

// write to vbuffer. subject to change
void write_to_vbuffer(int x, int y, uint32_t color) {
    vbuffer[y * (finfo.line_length / 4) + x] = color;
}

// clean up on program failure
void cleanup_error(int error_code) {
    // stop the threads and close their epolls if they have been created:
    if (program_data.threads != NULL) {
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_cancel(program_data.threads[i]);
            if (program_data.thread_data[i].epoll_fd != 0) {
                close(program_data.thread_data[i].epoll_fd);
            }
        }
    }

    if (vbuffer != MAP_FAILED && vbuffer != NULL) {
        munmap(vbuffer, finfo.smem_len);
    }

    close(program_data.fb_fd);
    close(program_data.server_fd);
    close(program_data.epoll_fd);

    // also free the client_state buffer
    free(program_data.clients);

    exit(error_code);
}

// clean up on SIGINT.
void cleanup() {
    cleanup_error(0);
}