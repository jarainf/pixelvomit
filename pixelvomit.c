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

#define FRAMERATE 60
#define FB_DEV "/dev/fb0"
#define PORT 42024
#define GRAB_SIZE 2048
#define DA_MODE 1
#define MAX_EVENTS 10

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
uint32_t *vbuffer;
int fb_fd;

void get_framebuffer_properties();
void *handle_connections(void *arg);
void handle_client(int client_fd);
void write_vbuffer();
void cleanup();

int main() {
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

    // Start a thread to handle connections
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
    int server_fd = *(int *)arg;
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: add server_fd failed");
        close(epoll_fd);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            close(epoll_fd);
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd == -1) {
                    perror("accept failed");
                    continue;
                }

                event.events = EPOLLIN;
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                    perror("epoll_ctl: add client_fd failed");
                    close(client_fd);
                }
            } else {
                handle_client(events[i].data.fd);
            }
        }
    }

    close(epoll_fd);
    return NULL;
}

void handle_client(int client_fd) {
    char buffer[GRAB_SIZE + 1] = {0};
    static char message[GRAB_SIZE * 2] = {0};
    static int offset_x = 0, offset_y = 0;
    ssize_t bytes_received;
    static size_t message_length = 0;

    bytes_received = recv(client_fd, buffer, GRAB_SIZE, 0);
    if (bytes_received <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes_received] = '\0';

    // Ensure no buffer overflow
    if (message_length + bytes_received > sizeof(message) - 1) {
        fprintf(stderr, "Buffer overflow detected, closing connection.\n");
        close(client_fd);
        return;
    }

    strncat(message, buffer, bytes_received);
    message_length += bytes_received;

    char *newline_pos;
    while ((newline_pos = strchr(message, '\n')) != NULL) {
        *newline_pos = '\0';
        char *line = message;

        if (strncmp(line, "OF", 2) == 0) {
            sscanf(line + 6, "%d %d", &offset_x, &offset_y);
        } else if (strncmp(line, "SI", 2) == 0) {
            char size_response[32];
            snprintf(size_response, sizeof(size_response), "SIZE %d %d\n", vinfo.xres, vinfo.yres);
            send(client_fd, size_response, strlen(size_response), 0);
        } else if (strncmp(line, "PX", 2) == 0) {
            int x, y;
            char value[9] = {0};
            sscanf(line + 2, "%d %d %8s", &x, &y, value);
            x += offset_x;
            y += offset_y;

            if (x < vinfo.xres && y < vinfo.yres) {
                uint32_t color = strtoul(value, NULL, 16);
                vbuffer[y * (finfo.line_length / 4) + x] = color;
            }
        }

        memmove(message, newline_pos + 1, message_length - (newline_pos - message) - 1);
        message_length -= (newline_pos - message) + 1;
        message[message_length] = '\0';
    }
}

void cleanup() {
    if (vbuffer != MAP_FAILED && vbuffer != NULL) {
        munmap(vbuffer, finfo.smem_len);
    }
    close(fb_fd);
    exit(EXIT_SUCCESS);
}