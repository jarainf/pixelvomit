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

#define FRAMERATE 60
#define FB_DEV "/dev/fb0"
#define PORT 42024
#define GRAB_SIZE 2048
#define DA_MODE 1
#define NUM_THREADS 4
#define TOTAL_CLIENTS 1352
#define MAX_EVENTS CEILING(TOTAL_CLIENTS, NUM_THREADS)

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
uint32_t *vbuffer;
int fb_fd;

typedef struct {
    int fd;
    int offset_x;
    int offset_y;
    char buffer[GRAB_SIZE + 1];
    char message[GRAB_SIZE * 2];
    size_t message_length;
} client_state;

typedef struct {
    int epoll_fd;
    client_state clients[MAX_EVENTS];
    struct epoll_event events[MAX_EVENTS];
    int active_connections;
} client_thread;

void get_framebuffer_properties();
void *handle_connections(void *arg);
void *handle_clients(void *arg);
void handle_client(client_state *client);
void write_vbuffer();
void cleanup();
int find_client(client_state *clients, int client_fd);
int find_spot(client_thread *thread_data, int *thread, int *client);

int main() {
    printf("%d\n",MAX_EVENTS * NUM_THREADS);
    printf("%d\n",MAX_EVENTS);
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    pthread_t thread;

    signal(SIGINT, cleanup);  // Handle interrupts to clean up resources

    // Open the framebuffer device
    fb_fd = open(FB_DEV, O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer device");
        return EXIT_FAILURE;
    }

    // Get framebuffer properties
    get_framebuffer_properties();

    if (DA_MODE) {
        vbuffer = (uint32_t *)mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
        if (vbuffer == MAP_FAILED) {
            perror("Error mapping framebuffer device to memory");
            close(fb_fd);
            return EXIT_FAILURE;
        }
    } else {
        vbuffer = (uint32_t *)malloc(vinfo.yres_virtual * finfo.line_length);
        if (!vbuffer) {
            perror("Error allocating memory for vbuffer");
            close(fb_fd);
            return EXIT_FAILURE;
        }
        memset(vbuffer, 0, vinfo.yres_virtual * finfo.line_length);
    }

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation error");
        close(fb_fd);
        return EXIT_FAILURE;
    }

    // Bind the socket
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

    printf("Listening socket_fd: %d\n", server_fd);

    // Start thread to handle incoming connections
    pthread_create(&thread, NULL, handle_connections, &server_fd);
    pthread_detach(thread);

    while (1)
    {
        usleep(1000000 / FRAMERATE);
    }

    cleanup();
    return 0;
}

void get_framebuffer_properties() {
    // Get variable screen information
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information");
        exit(EXIT_FAILURE);
    }

    // Get fixed screen information
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)) {
        perror("Error reading fixed information");
        exit(EXIT_FAILURE);
    }

    printf("Display info: %dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    printf("Line length: %d\n", finfo.line_length);
    printf("Framebuffer size: %d\n", finfo.smem_len);
    printf("RGBA order: %d%d%d%d\n", vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset, vinfo.transp.offset);
}

void *handle_connections(void *arg) {
    pthread_t threads[NUM_THREADS];
    int thread_args[NUM_THREADS];
    int server_fd = *(int *)arg;
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: add server_fd failed");
        close(epoll_fd);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    client_thread thread_data[NUM_THREADS] = {0};

    printf("Socket handle connection established. FD: %d\n", epoll_fd);

    // Start multiple threads to handle connections
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            perror("epoll_create1 failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        printf("Start thread: %d FD: %d\n", i, epoll_fd);
        
        pthread_create(&threads[i], NULL, handle_clients, &thread_data[i]);
        pthread_detach(threads[i]);
    }

    // while-loop to accept new clients
    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            close(epoll_fd);
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++) {
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
            }

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
                if (epoll_ctl(thread_data[thread].epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                    perror("epoll_ctl: add client_fd failed");
                    close(client_fd);
                } else {
                    thread_data[thread].clients[client].fd = client_fd;
                    thread_data[thread].clients[client].offset_x = 0;
                    thread_data[thread].clients[client].offset_y = 0;
                    thread_data[thread].clients[client].message_length = 0;
                    thread_data[thread].active_connections += 1;

                    printf("Client accepted, FD: %d on epoll: %d. Thread: %d Client: %d\n", client_fd, epoll_fd, thread, client);
                }
            } else {
                perror("no spot found - closing connection");
                close(client_fd);
            }
        }
    }

    close(epoll_fd);
    return NULL;
}

void *handle_clients(void *arg) {
    client_thread *thread_data = (client_thread *) arg;

    while (1) {
        int n = epoll_wait(thread_data->epoll_fd, thread_data->events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            close(thread_data->epoll_fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++) {
            int client_fd = find_client(thread_data->clients, thread_data->events[i].data.fd);
            
            if (client_fd != -1) {
                handle_client(&thread_data->clients[client_fd]);
            } else {
                perror("client_fd not in clients");
                continue;
            }
        }

        // TODO: close threads?
        
    }

    close(thread_data->epoll_fd);
    return NULL;
}

void handle_client(client_state *client) {
    ssize_t bytes_received;

    bytes_received = recv(client->fd, client->buffer, GRAB_SIZE, 0);
    if (bytes_received <= 0) {
        printf("Client dropped: %d\n", client->fd);
        client->fd = close(client->fd);
        return;
    }

    client->buffer[bytes_received] = '\0';

    // Ensure no buffer overflow
    if (client->message_length + bytes_received > sizeof(client->message) - 1) {
        fprintf(stderr, "Buffer overflow detected, closing connection.\n");
        close(client->fd);
        return;
    }

    strncat(client->message, client->buffer, bytes_received);
    client->message_length += bytes_received;

    char *newline_pos;
    while ((newline_pos = strchr(client->message, '\n')) != NULL) {
        *newline_pos = '\0';
        char *line = client->message;

        if (strncmp(line, "OF", 2) == 0) {
            sscanf(line + 6, "%d %d", &client->offset_x, &client->offset_y);
        } else if (strncmp(line, "SI", 2) == 0) {
            char size_response[32];
            snprintf(size_response, sizeof(size_response), "SIZE %d %d\n", vinfo.xres, vinfo.yres);
            send(client->fd, size_response, strlen(size_response), 0);
        } else if (strncmp(line, "PX", 2) == 0) {
            int x, y;
            char value[9] = {0};
            sscanf(line + 2, "%d %d %8s", &x, &y, value);
            x += client->offset_x;
            y += client->offset_y;

            if (x < vinfo.xres && y < vinfo.yres) {
                uint32_t color = strtoul(value, NULL, 16);
                vbuffer[y * (finfo.line_length / 4) + x] = color;
            }
        } else {
            printf("Token: %s\n", line);
        }

        memmove(client->message, newline_pos + 1, client->message_length - (newline_pos - client->message) - 1);
        client->message_length -= (newline_pos - client->message) + 1;
        client->message[client->message_length] = '\0';
    }
}

int find_client(client_state *clients, int client_fd) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i].fd == client_fd) {
            return i;
        }        
    }
    return -1;
}

int find_spot(client_thread *thread_data, int *thread, int *client) {
    int lowest = __INT_MAX__;
    *thread = 0;
    *client = 0;
    // find the best thread with an open slot
    for (int i = 0; i < NUM_THREADS; i++) {
        if (thread_data[i].active_connections != 0) {
            if (thread_data[i].active_connections <= lowest && thread_data[i].active_connections != MAX_EVENTS) {
                lowest = thread_data[i].active_connections;
                *thread = i;
            }
        } else {
            lowest = 0;
            *thread = i;
            break;
        }
    }
    // If a suitable thread has been found
    if (lowest < __INT_MAX__) {
        // find the open spot
        for (*client = 0; *client < MAX_EVENTS; *client += 1) {
            if (thread_data[*thread].clients[*client].fd == 0) {
                break;
            }
        }

        return(0);
    }
    // all slots are full
    return -1;
}

void cleanup() {
    if (vbuffer != MAP_FAILED && vbuffer != NULL) {
        munmap(vbuffer, finfo.smem_len);
    }
    close(fb_fd);
    exit(EXIT_SUCCESS);
}