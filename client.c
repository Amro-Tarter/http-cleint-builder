#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_PORT 65535
#define MAX_REQUEST_SIZE 1024
#define MAX_RESPONSE_SIZE 150000
#define MAX_REDIRECTIONS 10

// Function prototypes
int is_positive_number(const char *str);
void parse_url(const char *url, char *host, char *path, int *port, int *is_https);
void construct_request(const char *host, const char *path, const char *query, char *request);
void handle_response(int sockfd, char *host, int port);

// Function to validate if a string is a positive number
int is_positive_number(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (!isdigit(str[i]))
            return 0;
    }
    return 1;
}

// Parse the URL into host, path, port, and whether it's an HTTPS request
void parse_url(const char *url, char *host, char *path, int *port, int *is_https) {
    *is_https = 0;
    if (strncmp(url, "http://", 7) == 0) {
        *is_https = 0;
        url += 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        *is_https = 1;
        url += 8;
    } else {
        printf("Usage: client [-r n <pr1=value1 pr2=value2 …>] <URL>\n");
        exit(EXIT_FAILURE);
    }

    const char *port_part = strchr(url, ':');
    const char *path_part = strchr(url, '/');

    if (port_part && (!path_part || port_part < path_part)) {
        strncpy(host, url, port_part - url);
        host[port_part - url] = '\0';
        *port = atoi(port_part + 1);
        if (*port <= 0 || *port > MAX_PORT) {
            printf("Usage: Port must be a positive number less than %d\n", MAX_PORT + 1);
            exit(EXIT_FAILURE);
        }
        if (path_part) {
            strcpy(path, path_part);
        } else {
            strcpy(path, "/");
        }
    } else {
        if (path_part) {
            strncpy(host, url, path_part - url);
            host[path_part - url] = '\0';
            strcpy(path, path_part);
        } else {
            strcpy(host, url);
            strcpy(path, "/");
        }
        *port = 80; // Default HTTP port
    }
}

// Construct the HTTP GET request
void construct_request(const char *host, const char *path, const char *query, char *request) {
    snprintf(request, MAX_REQUEST_SIZE, "GET %s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, query, host);
    printf("HTTP request =\n%s\nLEN = %ld\n", request, strlen(request));
}

// Handle server response and manage redirections
void handle_response(int sockfd, char *host, int port) {

    char response[MAX_RESPONSE_SIZE];
    char new_url[256];
    int total_bytes = 0, bytes;
    int status_code;
    int redirection_count = 0;

    while (redirection_count < MAX_REDIRECTIONS) {
        // Clear buffer before use
        memset(response, 0, sizeof(response));

        // Receive the response headers
        while ((bytes = recv(sockfd, response + total_bytes, sizeof(response) - total_bytes - 1, 0)) > 0) {
            total_bytes += bytes;
            response[total_bytes] = '\0'; // Null-terminate for header parsing

            // Check if headers are complete (double CRLF)
            char *header_end = strstr(response, "\r\n\r\n");
            if (header_end) {
                // Headers are complete
                int header_length = header_end - response + 4; // Include CRLF
                printf("Headers:\n%.*s\n", header_length, response);

                // Check if the response is binary (e.g., image/png)
                char *content_type = strstr(response, "Content-Type: ");
                if (content_type && strstr(content_type, "image/png")) {
                    // Handle binary data (image)
                    int content_length = 0;
                    char *content_length_header = strstr(response, "Content-Length: ");
                    if (content_length_header) {
                        content_length = atoi(content_length_header + strlen("Content-Length: "));
                    }

                    // Calculate remaining bytes to read
                    int remaining_bytes = content_length - (total_bytes - header_length);

                    // Read the binary data
                    FILE *file = fopen("meow.png", "wb");
                    if (!file) {
                        perror("fopen");
                        close(sockfd);
                        exit(EXIT_FAILURE);
                    }

                    // Write the already received binary data
                    fwrite(response + header_length, 1, total_bytes - header_length, file);

                    // Read the remaining binary data
                    while (remaining_bytes > 0) {
                        bytes = recv(sockfd, response, sizeof(response), 0);
                        if (bytes <= 0) {
                            perror("recv");
                            fclose(file);
                            close(sockfd);
                            exit(EXIT_FAILURE);
                        }
                        fwrite(response, 1, bytes, file);
                        remaining_bytes -= bytes;
                    }
                    printf("\n   Total received response bytes: %d\n", total_bytes);


                    fclose(file);

                    close(sockfd);
                    return;
                }
                break;
            }
        }

        if (bytes < 0) {
            perror("recv");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // Parse HTTP status code
        if (sscanf(response, "HTTP/1.1 %d", &status_code) != 1) {
            printf("Failed to parse status code\n");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // Handle redirection (3xx status codes)
        if (status_code >= 300 && status_code < 400) {
            char *location = strstr(response, "Location: ");
            if (location) {
                location += strlen("Location: ");
                char *end_line = strstr(location, "\r\n");
                if (end_line) {
                    *end_line = '\0'; // Null-terminate the URL
                }
                strcpy(new_url, location);

                // Handle relative URLs
                if (strncmp(new_url, "http://", 7) != 0 && strncmp(new_url, "https://", 8) != 0) {
                    // Construct full URL from relative URL
                    char full_url[256];
                    snprintf(full_url, sizeof(full_url), "http://%s%s", host, new_url);
                    strcpy(new_url, full_url);
                }

                // Check if the new URL is HTTP (not HTTPS)
                if (strncmp(new_url, "http://", 7) != 0) {
                    printf("Redirection to non-HTTP URL: %s\n", new_url);
                    break;
                }

                // Close current socket
                close(sockfd);

                // Parse new URL
                char new_host[256], path[256];
                int new_port, is_https;
                parse_url(new_url, new_host, path, &new_port, &is_https);

                // Construct new request
                char request[MAX_REQUEST_SIZE];
                construct_request(new_host, path, "", request);

                // Create a new socket
                sockfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sockfd < 0) {
                    perror("socket");
                    exit(EXIT_FAILURE);
                }

                // Get server details
                struct hostent *server = gethostbyname(new_host);
                if (!server) {
                    herror("gethostbyname");
                    close(sockfd);
                    exit(EXIT_FAILURE);
                }

                // Connect to the new server
                struct sockaddr_in server_addr;
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(new_port);
                memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

                if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
                    perror("connect");
                    close(sockfd);
                    exit(EXIT_FAILURE);
                }

                // Send new request
                if (send(sockfd, request, strlen(request), 0) < 0) {
                    perror("send");
                    close(sockfd);
                    exit(EXIT_FAILURE);
                }

                // Reset total_bytes for the new response
                total_bytes = 0;
                redirection_count++;
                continue;
            } else {
                printf("No Location header found.\n");
                break;
            }
        } else {
            // Print the final response
            printf("%s\n", response);
            break;
        }
    }

    // Print total bytes after the final response
    printf("\n   Total received response bytes: %d\n", total_bytes);

    // Close the socket
    close(sockfd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: client [-r n <pr1=value1 pr2=value2 …>] <URL>\n");
        return EXIT_FAILURE;
    }

    char host[256], path[256], request[MAX_REQUEST_SIZE], query[256] = "";
    int port, is_https = 0;
    int i = 1;

    // Parse command-line arguments
    while (i < argc) {
        if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 >= argc || !is_positive_number(argv[i + 1])) {
                printf("Usage: -r must be followed by a positive number\n");
                return EXIT_FAILURE;
            }
            int n = atoi(argv[++i]);
            for (int j = 0; j < n; j++) {
                if (i + 1 >= argc || !strchr(argv[i + 1], '=')) {
                    printf("\"Usage: client [-r n < pr1=value1 pr2=value2 …>] <URL>\"  \n");
                    return EXIT_FAILURE;
                }
                strcat(query, j == 0 ? "?" : "&");
                strcat(query, argv[++i]);
            }
        } else {
            parse_url(argv[i], host, path, &port, &is_https);
        }
        i++;
    }

    // Construct the HTTP GET request
    construct_request(host, path, query, request);

    // Create a socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // Get server details
    struct hostent *server = gethostbyname(host);
    if (!server) {
        herror("gethostbyname");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // Connect to the server
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // Send the HTTP request
    if (send(sockfd, request, strlen(request), 0) < 0) {
        perror("send");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // Handle server response
    handle_response(sockfd, host, port);

    return EXIT_SUCCESS;
}