#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>


#define SOCKET_PATH "/tmp/mysocket"
#define BUFFER_SIZE 1024
#define FILE_NAME_SEND "file_to_send.txt"
#define FILE_NAME_RECEIVE "received_file.txt"
#define EOF_MARKER "<<<EOF>>>"

#define PIPE_NAME "/tmp/my_pipe"

void set_non_blocking(int fd)
{ // this function add the O_NONBLOCK flag to the file descriptor
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

unsigned char calculate_checksum(char *buf, int len) {
    unsigned char checksum = 0;
    for (int i = 0; i < len; i++) {
        checksum ^= buf[i];
    }
    return checksum;
}

void serverMMAP(int quiet_mode) {
    int server_fd, new_socket;
    struct sockaddr_un address, client_addr;
    int addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);

    unlink(SOCKET_PATH);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // Accept a connection
    if ((new_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&addr_len)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    setsockopt(new_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    FILE *fp = NULL;
    int total_bytes_received = 0;
    int eof_marker_size = strlen(EOF_MARKER);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    while (1) {
        int bytes_received = read(new_socket, buffer, BUFFER_SIZE);
        if (bytes_received <= 0) {
            if (bytes_received < 0) {
                perror("read");
            }
            break;
        }

        if (fp == NULL) {
            fp = fopen(FILE_NAME_RECEIVE, "wb");
            if (fp == NULL) {
                perror("fopen");
                exit(EXIT_FAILURE);
            }
        }

        // Check for EOF marker
        if (bytes_received >= eof_marker_size && memcmp(buffer + bytes_received - eof_marker_size, EOF_MARKER, eof_marker_size) == 0) {
            fwrite(buffer, 1, bytes_received - eof_marker_size, fp); // Write remaining data before EOF marker
            total_bytes_received += (bytes_received - eof_marker_size);
            break; // Exit the loop
        }

        fwrite(buffer, 1, bytes_received, fp);
        total_bytes_received += bytes_received;
    }

    gettimeofday(&end, NULL);
    double time_taken = ((end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec)) * 1e-6;

    if (fp != NULL) {
        fclose(fp);
    }
    close(new_socket);
    close(server_fd);

    if (quiet_mode != 1)
    {
        printf("File transfer complete. %d bytes received.\n", total_bytes_received);
        printf("Time taken: %.6lf seconds\n", time_taken);
    }
    else
    {
        printf("mmap,%.6lf\n", time_taken);
    }
}

void clientMMAP(const char *file_name) {
    int sock;
    struct sockaddr_un serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        perror("fstat");
        exit(EXIT_FAILURE);
    }

    void *file_data = mmap(NULL, file_stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (file_data == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_sent = 0;
    while (bytes_sent < file_stat.st_size) {
        ssize_t bytes_to_send = (file_stat.st_size - bytes_sent > BUFFER_SIZE) ? BUFFER_SIZE : file_stat.st_size - bytes_sent;
        if (write(sock, file_data + bytes_sent, bytes_to_send) != bytes_to_send) {
            perror("write");
            break;
        }
        bytes_sent += bytes_to_send;
    }

    // Send EOF marker
    strncpy(buffer, EOF_MARKER, strlen(EOF_MARKER));
    if (write(sock, buffer, strlen(EOF_MARKER)) != strlen(EOF_MARKER)) {
        perror("write");
    }

    munmap(file_data, file_stat.st_size);
    close(fd);
    close(sock);
}

int serverPipe(int quiet_mode) {
    int fd;
    FILE *file;
    char buf[BUFFER_SIZE];
    struct timeval start, end;
    unsigned char checksum = 0;
    
    /* create the FIFO (named pipe) */
    mkfifo(PIPE_NAME, 0666);

    /* open the file for writing */
    file = fopen(FILE_NAME_RECEIVE, "w");
    if (file == NULL) {
        perror("fopen");
        return 1;
    }

    /* open the FIFO for reading */
    fd = open(PIPE_NAME, O_RDONLY);

    /* get the start time */
    gettimeofday(&start, NULL);

    /* read from the FIFO and write to the file */
    int bytesRead;
    while ((bytesRead = read(fd, buf, BUFFER_SIZE)) > 0) {
        fwrite(buf, sizeof(char), bytesRead, file);
        checksum ^= calculate_checksum(buf, bytesRead);
    }

    /* get the end time */
    gettimeofday(&end, NULL);

    fclose(file);
    close(fd);

    /* calculate and print the elapsed time in ms */
    long seconds = end.tv_sec - start.tv_sec;
    long microseconds = end.tv_usec - start.tv_usec;
    double elapsed = seconds * 1000.0 + microseconds / 1000.0;
    if (quiet_mode != 1)
    {
        printf("Time elapsed: %.2f ms\n", elapsed);
    }
    else
    {
        printf("pipe,%.2f\n", elapsed);
    }
    /* remove the FIFO */
    unlink(PIPE_NAME);

    return 0;
}

int clientPipe(char *filename) {
    int fd;
    FILE *file;
    char buf[BUFFER_SIZE];
    unsigned char checksum = 0;

    /* open the file for reading */
    file = fopen(filename, "r");
    if (file == NULL) {
        perror("fopen");
        return 1;
    }

    /* open the FIFO for writing */
    fd = open(PIPE_NAME, O_WRONLY);

    /* read from the file and write to the FIFO */
    int bytesRead;
    while ((bytesRead = fread(buf, sizeof(char), sizeof(buf), file)) > 0) {
        write(fd, buf, bytesRead);
        checksum ^= calculate_checksum(buf, bytesRead);
    }

    fclose(file);
    close(fd);

    return 0;
}


void serverUdsStream(int quiet_mode)
{
    int server_fd, new_socket;
    struct sockaddr_un address, client_addr;
    int addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);

    unlink(SOCKET_PATH);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, 1) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // Accept a connection
    if ((new_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&addr_len)) < 0)
    {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    FILE *fp = NULL;
    int total_bytes_received = 0;
    int eof_marker_size = strlen(EOF_MARKER);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    while (1)
    {
        int bytes_received = read(new_socket, buffer, BUFFER_SIZE);
        if (bytes_received <= 0)
        {
            if (bytes_received < 0)
            {
                perror("read");
            }
            break;
        }

        if (fp == NULL)
        {
            fp = fopen(FILE_NAME_RECEIVE, "wb");
            if (fp == NULL)
            {
                perror("fopen");
                exit(EXIT_FAILURE);
            }
        }

        // Check for EOF marker
        if (bytes_received >= eof_marker_size && memcmp(buffer + bytes_received - eof_marker_size, EOF_MARKER, eof_marker_size) == 0)
        {
            fwrite(buffer, 1, bytes_received - eof_marker_size, fp); // Write remaining data before EOF marker
            total_bytes_received += (bytes_received - eof_marker_size);
            break; // Exit the loop
        }

        fwrite(buffer, 1, bytes_received, fp);
        total_bytes_received += bytes_received;
    }

    gettimeofday(&end, NULL);
    double time_taken = ((end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec)) * 1e-6;

    if (fp != NULL)
    {
        fclose(fp);
    }
    close(new_socket);
    close(server_fd);

    if (quiet_mode != 1)
    {
        printf("File transfer complete. %d bytes received.\n", total_bytes_received);
        printf("Time taken: %.6lf seconds\n", time_taken);
    }
    else
    {
        printf("uds_stream,%.6lf seconds\n", time_taken);
    }
}

void clientUdsStream()
{
    int sock;
    struct sockaddr_un serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0)
    {
        if (write(sock, buffer, bytes_read) != bytes_read)
        {
            perror("write");
            break;
        }
    }

    // Send EOF marker
    strncpy(buffer, EOF_MARKER, strlen(EOF_MARKER));
    if (write(sock, buffer, strlen(EOF_MARKER)) != strlen(EOF_MARKER))
    {
        perror("write");
    }

    fclose(fp);
    close(sock);
}

void serverUdsDgram(int quiet_mode) {
    int server_fd;
    struct sockaddr_un address, client_addr;
    int addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_UNIX, SOCK_DGRAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);

    unlink(SOCKET_PATH);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    struct pollfd fds[1];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;

    FILE *fp = NULL;
    int total_bytes_received = 0;
    int poll_ret;
    int eof_marker_size = strlen(EOF_MARKER);

    // Add a struct to hold the start and end times
    struct timeval start_time, end_time;

    // Get the current time and store it in start_time
    gettimeofday(&start_time, NULL);

    while (1) {
        poll_ret = poll(fds, 1, -1); // No timeout, wait indefinitely

        if (poll_ret > 0) {
            if (fds[0].revents & POLLIN) {
                int bytes_received = recvfrom(server_fd, buffer, BUFFER_SIZE, 0,
                                              (struct sockaddr *)&client_addr, (socklen_t *)&addr_len);
                if (bytes_received <= 0) {
                    perror("recvfrom");
                    break;
                }

                if (fp == NULL) {
                    fp = fopen(FILE_NAME_RECEIVE, "wb");
                    if (fp == NULL) {
                        perror("fopen");
                        exit(EXIT_FAILURE);
                    }
                }

                // Check for EOF marker
                if (bytes_received >= eof_marker_size && memcmp(buffer + bytes_received - eof_marker_size, EOF_MARKER, eof_marker_size) == 0) {
                    fwrite(buffer, 1, bytes_received - eof_marker_size, fp); // Write remaining data before EOF marker
                    break; // Exit the loop
                }

                fwrite(buffer, 1, bytes_received, fp);
                total_bytes_received += bytes_received;
            }
        } else {
            perror("poll");
            break;
        }
    }

    // Get the current time again and store it in end_time
    gettimeofday(&end_time, NULL);

    // Calculate the elapsed time in milliseconds
    long elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                           (end_time.tv_usec - start_time.tv_usec) / 1000;

    if (fp != NULL) {
        fclose(fp);
    }
    close(server_fd);

    if (quiet_mode != 1)
    {
        printf("File transfer completed. Received %d bytes.\n", total_bytes_received);
        printf("File transfer time: %ld ms\n", elapsed_time_ms); // Print the elapsed time
    }
    else
    {
        printf("uds_dgram,%ld\n", elapsed_time_ms);
    }
}

void clientUdsDgram() {
    int sock = 0;
    struct sockaddr_un serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        sendto(sock, buffer, bytes_read, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    }

    // Send EOF marker
    int eof_marker_size = strlen(EOF_MARKER);
    sendto(sock, EOF_MARKER, eof_marker_size, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    gettimeofday(&end_time, NULL);

    fclose(fp);
    close(sock);

    double elapsed_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0;
    elapsed_time += (end_time.tv_usec - start_time.tv_usec) / 1000.0;

    printf("File transfer completed in %.2f milliseconds.\n", elapsed_time);
}
void serverUdpIpv6(uint16_t port, int quiet_mode)
{
    int server_fd;
    struct sockaddr_in6 address, client_addr;
    int addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_INET6, SOCK_DGRAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    printf("Listening on port %d...\n", port);

    struct pollfd fds[1];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;

    FILE *fp = fopen(FILE_NAME_RECEIVE, "wb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int total_bytes_received = 0;
    int poll_ret;
    int eof_marker_size = strlen(EOF_MARKER);

    // Add a struct to hold the start and end times
    struct timeval start_time, end_time;

    // Get the current time and store it in start_time
    gettimeofday(&start_time, NULL);

    while (1)
    {
        poll_ret = poll(fds, 1, 500); // No timeout, wait indefinitely

        if (poll_ret > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                int bytes_received = recvfrom(server_fd, buffer, BUFFER_SIZE, 0,
                                              (struct sockaddr *)&client_addr, (socklen_t *)&addr_len);
                if (bytes_received <= 0)
                {
                    perror("recvfrom");
                    break;
                }

                // Check for EOF marker
                if (bytes_received >= eof_marker_size && memcmp(buffer + bytes_received - eof_marker_size, EOF_MARKER, eof_marker_size) == 0)
                {
                    fwrite(buffer, 1, bytes_received - eof_marker_size, fp); // Write remaining data before EOF marker
                    break;                                                   // Exit the loop
                }

                fwrite(buffer, 1, bytes_received, fp);
                total_bytes_received += bytes_received;
            }
        }
        else
        {
            perror("poll");
            break;
        }
    }

    // Get the current time again and store it in end_time
    gettimeofday(&end_time, NULL);

    // Calculate the elapsed time in milliseconds
    long elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                           (end_time.tv_usec - start_time.tv_usec) / 1000;

    fclose(fp);
    close(server_fd);

    if (quiet_mode != 1)
    {
        printf("File transfer complete. %d bytes received.\n", total_bytes_received);
        printf("File transfer time: %ld ms\n", elapsed_time_ms); // Print the elapsed time
    }
    else
    {
        printf("ipv6_udp,%ld\n", elapsed_time_ms);
    }
}

void clientUdpIpv6(uint16_t port, char *ipUdpIpv6)
{
    int sock = 0;
    struct sockaddr_in6 serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    serv_addr.sin6_family = AF_INET6;
    serv_addr.sin6_port = htons(port);

    // Convert IPv6 address from text to binary form
    if (inet_pton(AF_INET6, ipUdpIpv6, &serv_addr.sin6_addr) <= 0)
    {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0)
    {
        sendto(sock, buffer, bytes_read, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    }

    // Send EOF marker
    int eof_marker_size = strlen(EOF_MARKER);
    sendto(sock, EOF_MARKER, eof_marker_size, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    fclose(fp);
    close(sock);
}

void serverUdpIpv4(uint16_t port, int quiet_mode)
{
    int server_fd;
    struct sockaddr_in address, client_addr;
    int addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    printf("Listening on port %d...\n", port);

    struct pollfd fds[1];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;

    FILE *fp = fopen(FILE_NAME_RECEIVE, "wb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    int total_bytes_received = 0;
    int poll_ret;
    int eof_marker_size = strlen(EOF_MARKER);

    // Add a struct to hold the start and end times
    struct timeval start_time, end_time;

    // Get the current time and store it in start_time
    gettimeofday(&start_time, NULL);

    while (1)
    {
        poll_ret = poll(fds, 1, -1); // No timeout, wait indefinitely

        if (poll_ret > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                int bytes_received = recvfrom(server_fd, buffer, BUFFER_SIZE, 0,
                                              (struct sockaddr *)&client_addr, (socklen_t *)&addr_len);
                if (bytes_received <= 0)
                {
                    perror("recvfrom");
                    break;
                }

                // Check for EOF marker
                if (bytes_received >= eof_marker_size && memcmp(buffer + bytes_received - eof_marker_size, EOF_MARKER, eof_marker_size) == 0)
                {
                    fwrite(buffer, 1, bytes_received - eof_marker_size, fp); // Write remaining data before EOF marker
                    break;                                                   // Exit the loop
                }

                fwrite(buffer, 1, bytes_received, fp);
                total_bytes_received += bytes_received;
            }
        }
        else
        {
            perror("poll");
            break;
        }
    }

    // Get the current time again and store it in end_time
    gettimeofday(&end_time, NULL);

    // Calculate the elapsed time in milliseconds
    long elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                           (end_time.tv_usec - start_time.tv_usec) / 1000;

    fclose(fp);
    close(server_fd);
    if (quiet_mode != 1)
    {
        printf("File transfer completed. Received %d bytes.\n", total_bytes_received);
        printf("File transfer time: %ld ms\n", elapsed_time_ms); // Print the elapsed time
    }
    else
    {
        printf("ipv4_udp,%ld\n", elapsed_time_ms);
    }
}

void clientUdpIpv4(uint16_t port, char *ipUdpIpv4)
{
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, ipUdpIpv4, &serv_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0)
    {
        sendto(sock, buffer, bytes_read, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    }

    // Send EOF marker
    int eof_marker_size = strlen(EOF_MARKER);
    sendto(sock, EOF_MARKER, eof_marker_size, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    fclose(fp);
    close(sock);

    printf("File transfer completed.\n");
}

void serverTcpIpv6(uint16_t port, int quiet_mode)
{
    int server_fd, new_socket;
    struct sockaddr_in6 address;
    int addr_len = sizeof(address);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, 1) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Listening on port %d...\n", port);

    // Accept a client connection
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addr_len)) < 0)
    {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    struct pollfd fds[1];
    fds[0].fd = new_socket;
    fds[0].events = POLLIN;

    FILE *fp = fopen(FILE_NAME_RECEIVE, "wb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int total_bytes_received = 0;
    int poll_ret;

    // Add a struct to hold the start and end times
    struct timeval start_time, end_time;

    // Get the current time and store it in start_time
    gettimeofday(&start_time, NULL);

    while (1)
    {
        poll_ret = poll(fds, 1, 500);

        if (poll_ret > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                int bytes_received = recv(new_socket, buffer, BUFFER_SIZE, 0);
                if (bytes_received <= 0)
                {
                    perror("recv");
                    break;
                }

                fwrite(buffer, 1, bytes_received, fp);
                total_bytes_received += bytes_received;
            }
        }
        else
        {
            perror("poll");
            break;
        }
    }

    // Get the current time again and store it in end_time
    gettimeofday(&end_time, NULL);

    // Calculate the elapsed time in milliseconds
    long elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                           (end_time.tv_usec - start_time.tv_usec) / 1000;

    fclose(fp);
    close(new_socket);
    close(server_fd);

    if (quiet_mode != 1)
    {
    printf("File transfer completed. Received %d bytes.\n", total_bytes_received);
        printf("File transfer time: %ld ms\n", elapsed_time_ms); // Print the elapsed time
    }
    else
    {
        printf("ipv6_tcp,%ld\n", elapsed_time_ms);
    }
}

void clientTcpIpv6(uint16_t port, char *ip_ipv6)
{
    int sock = 0;
    struct sockaddr_in6 serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_INET6, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    serv_addr.sin6_family = AF_INET6;
    serv_addr.sin6_port = htons(port);

    // Convert IPv6 address from text to binary form
    if (inet_pton(AF_INET6, ip_ipv6, &serv_addr.sin6_addr) <= 0)
    {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    set_non_blocking(sock);

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0)
    {
        send(sock, buffer, bytes_read, 0);
    }

    fclose(fp);
    close(sock);

    printf("File transfer completed.\n");
}

void serverTcpIpv4(uint16_t port, int quiet_mode)
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addr_len = sizeof(address);
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, 1) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Listening on port %d...\n", port);

    // Accept a client connection
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addr_len)) < 0)
    {
        perror("accept");
        exit(EXIT_FAILURE);
    }
    setsockopt(new_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    struct pollfd fds[1];
    fds[0].fd = new_socket;
    fds[0].events = POLLIN;

    FILE *fp = fopen(FILE_NAME_RECEIVE, "wb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int total_bytes_received = 0;
    int poll_ret;

    // Add a struct to hold the start and end times
    struct timeval start_time, end_time;

    // Get the current time and store it in start_time
    gettimeofday(&start_time, NULL);

    while (1)
    {
        poll_ret = poll(fds, 1, -1); // No timeout, wait indefinitely

        if (poll_ret > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                int bytes_received = recv(new_socket, buffer, BUFFER_SIZE, 0);
                if (bytes_received <= 0)
                {
                    perror("recv");
                    break;
                }

                fwrite(buffer, 1, bytes_received, fp);
                total_bytes_received += bytes_received;
            }
        }
        else
        {
            perror("poll");
            break;
        }
    }
    // Get the current time again and store it in end_time
    gettimeofday(&end_time, NULL);

    // Calculate the elapsed time in milliseconds
    long elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                           (end_time.tv_usec - start_time.tv_usec) / 1000;

    fclose(fp);
    close(new_socket);
    close(server_fd);

    if (quiet_mode != 1)
    {
    printf("File transfer completed. Received %d bytes.\n", total_bytes_received);
        printf("File transfer time: %ld ms\n", elapsed_time_ms); // Print the elapsed time
    }
    else
    {
        printf("ipv6_tcp,%ld\n", elapsed_time_ms);
    }
}

void clientTcpIpv4(uint16_t port, char *ip_destination)
{
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE];

    // Create a socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, ip_destination, &serv_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    FILE *fp = fopen(FILE_NAME_SEND, "rb");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0)
    {
        send(sock, buffer, bytes_read, 0);
    }

    fclose(fp);
    close(sock);

    printf("File transfer completed.\n");
}

typedef enum
{
    SERVER,
    CLIENT
} Role;


int create_socket(struct addrinfo *res)
{
    return socket(res->ai_family, res->ai_socktype, res->ai_protocol);
}

void setup_server(struct addrinfo *res, int *sockfd)
{
    bind(*sockfd, res->ai_addr, res->ai_addrlen);
    listen(*sockfd, 1);
    fprintf(stderr, "Waiting for a connection...\n");
    int client_fd = accept(*sockfd, NULL, NULL);
    close(*sockfd);
    *sockfd = client_fd;
}

void setup_client(struct addrinfo *res, int *sockfd)
{
    connect(*sockfd, res->ai_addr, res->ai_addrlen);
}

void poll_fds(struct pollfd *fds)
{
    poll(fds, 2, -1);
}

void handle_io(struct pollfd *fds, Role role, int sockfd)
{
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    if (fds[0].revents & POLLIN) // if there is data to read from stdin
    {
        bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            write(sockfd, buffer, bytes_read);
            if (strcmp(buffer, "exit\n") == 0)
            {
                printf("Connection closed\n");
                close(sockfd);
                return;
            }
        }
    }

    if (fds[1].revents & POLLIN) // id there is data to read from the socket
    {
        bytes_read = read(sockfd, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            if (role == SERVER)
            {
                printf("Client: %s", buffer);
            }
            else
            {
                printf("Server: %s", buffer);
            }
            if (strcmp(buffer, "exit\n") == 0)
            {
                printf("Connection closed\n");
                close(sockfd);
                return; // if the message is exit then we break the loop
            }
        }
        else if (bytes_read == 0)
        {
            printf("Connection closed\n");
        }
        else
        {
            perror("read");
        }
    }
}

int main(int argc, char const *argv[])
{

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s [-c IP | -s] PORT\n", argv[0]);
        return 1;
    }
    int client_mode = -1;
    int server_mode = -1;
    int quiet_mode = -1;
    int performance = -1;
    int port;
    char ipToSend[BUFFER_SIZE];
    char param[BUFFER_SIZE];
    char type[BUFFER_SIZE];

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0)
        {
            performance = 1;
        }
        else if (strcmp(argv[i], "-q") == 0)
        {
            quiet_mode = 1;
        }
        else if (strcmp(argv[i], "-c") == 0)
        {
            client_mode = 1;
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            server_mode = 1;
        }
    }

    if (client_mode == -1 && server_mode == -1)
    {
        fprintf(stderr, "Usage: %s [-c IP | -s] PORT\n", argv[0]);
        return 1;
    }
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    Role role;
    if (server_mode == 1)
    {
        role = SERVER;
    }
    else
    {
        role = CLIENT;
    }

    if (server_mode == 1)
    {
        if (argc != 3 && argc != 4 && argc != 5)
        {
            fprintf(stderr, "Usage: %s -s PORT [-p] [-q]\n", argv[0]);
            return 1;
        }
        hints.ai_flags = AI_PASSIVE;
        getaddrinfo(NULL, argv[2], &hints, &res);
        port = atoi(argv[2]);
    }
    else if (client_mode == 1)
    {
        if (argc != 4 && argc != 7 && argc != 6)
        {
            fprintf(stderr, "Usage: %s -c IP PORT\n", argv[0]);
            return 1;
        }
        if (strchr(argv[2], ':') != NULL)
        {
            getaddrinfo("127.0.0.1", argv[3], &hints, &res);
        }
        else
        {
            getaddrinfo(argv[2], argv[3], &hints, &res);
        }
        port = atoi(argv[3]);
        strcpy(ipToSend, argv[2]);
    }
    else
    {
        fprintf(stderr, "Usage: %s [-c IP | -s] PORT\n", argv[0]);
        return 1;
    }

    int sockfd = create_socket(res);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &(int){1}, sizeof(int));
    if (server_mode == 1)
    {
        setup_server(res, &sockfd);
    }
    else if (client_mode == 1)
    {
        setup_client(res, &sockfd);
    }
    if (performance == 1)
    {
        if (client_mode == 1)
        {
            if (argc != 7)
            {
                fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                return 1;
            }
            int i, errorP = 0;
            for (i = 4; i < 7; ++i)
            {
                if ((strcmp(argv[i], "tcp") == 0) || (strcmp(argv[i], "udp") == 0) || (strcmp(argv[i], "dgram") == 0) || (strcmp(argv[i], "stream") == 0))
                {
                    strcpy(param, argv[i]);
                    printf("param = %s\n", param);
                }
                else if ((strcmp(argv[i], "ipv4") == 0) || (strcmp(argv[i], "ipv6") == 0) || (strcmp(argv[i], "mmap") == 0) || (strcmp(argv[i], "pipe") == 0) || (strcmp(argv[i], "uds") == 0))
                {
                    strcpy(type, argv[i]);
                }
                else if (strchr(argv[i], '.') != NULL) // th case of the filename (must contain a dot)
                {
                    strcpy(param, argv[i]);
                }
                else if ((strcmp(argv[i], "-p") != 0))
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
                if (strcmp(argv[i], "-p") == 0)
                {
                    errorP++;
                    if (errorP > 1)
                    {
                        fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                        return 1;
                    }
                }
            }
            if (param == '\0' || type == '\0')
            {
                fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                return 1;
            }
            write(sockfd, param, BUFFER_SIZE);
            write(sockfd, type, BUFFER_SIZE);
            sleep(0.1);
            close(sockfd);
            if (strcmp(param, "tcp") == 0)
            {
                if (strcmp(type, "ipv4") == 0)
                {
                    clientTcpIpv4(port, ipToSend);
                }
                else if (strcmp(type, "ipv6") == 0)
                {
                    clientTcpIpv6(port, ipToSend);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strcmp(param, "udp") == 0)
            {
                if (strcmp(type, "ipv4") == 0)
                {
                    clientUdpIpv4(port, ipToSend);
                }
                else if (strcmp(type, "ipv6") == 0)
                {
                    clientUdpIpv6(port, ipToSend);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strcmp(type, "uds") == 0)
            {
                if (strcmp(param, "dgram") == 0)
                {
                    clientUdsDgram();
                }
                else if (strcmp(param, "stream") == 0)
                {
                    clientUdsStream();
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strchr(param, '.') != NULL)
            {
                if (strcmp(type, "mmap") == 0)
                {
                    clientMMAP(param);
                }
                else if (strcmp(type, "pipe") == 0)
                {
                    clientPipe(param);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            
        }
        else if (server_mode == 1)
        {
            memset(param, 0, sizeof(param));
            memset(type, 0, sizeof(type));
            read(sockfd, param, BUFFER_SIZE);
            read(sockfd, type, BUFFER_SIZE);
            close(sockfd);
            if (strcmp(param, "tcp") == 0)
            {
                if (strcmp(type, "ipv4") == 0)
                {
                    serverTcpIpv4(port, quiet_mode);
                }
                else if (strcmp(type, "ipv6") == 0)
                {
                    serverTcpIpv6(port, quiet_mode);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strcmp(param, "udp") == 0)
            {
                if (strcmp(type, "ipv4") == 0)
                {
                    serverUdpIpv4(port, quiet_mode);
                }
                else if (strcmp(type, "ipv6") == 0)
                {
                    serverUdpIpv6(port, quiet_mode);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strcmp(type, "uds") == 0)
            {
                if (strcmp(param, "dgram") == 0)
                {
                    serverUdsDgram(quiet_mode);
                }
                else if (strcmp(param, "stream") == 0)
                {
                    serverUdsStream(quiet_mode);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
            else if (strchr(param, '.') != NULL)
            {
                if (strcmp(type, "mmap") == 0)
                {
                    serverMMAP(quiet_mode);
                }
                else if (strcmp(type, "pipe") == 0)
                {
                    serverPipe(quiet_mode);
                }
                else
                {
                    fprintf(stderr, "Usage: %s -c IP PORT -p <param> <type> \n", argv[0]);
                    return 1;
                }
            }
        }
    }
    else
    {

        freeaddrinfo(res);
        set_non_blocking(sockfd);
        set_non_blocking(STDIN_FILENO);

        struct pollfd fds[2];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = sockfd;
        fds[1].events = POLLIN;

        while (1)
        {
            poll_fds(fds);

            handle_io(fds, role, sockfd);


            if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            { // if there is an error in the socket then we break the loop
                break;
            }
        }

        close(sockfd);
        return 0;
    }


    return 0;
}
