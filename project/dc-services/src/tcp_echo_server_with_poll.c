#include <stdio.h>
#include <stdlib.h> // malloc(), free()

#include <arpa/inet.h> // inet_pton()
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h> // memset(), memcpy()
#include <sys/socket.h>
#include <unistd.h> // close()

#include <errno.h> // errno specific checks

#include <poll.h>

#define RX_BUFF_SIZE 1024
#define LISTEN_IP "127.0.0.1"
#define LISTEN_PORT 5555

/* 3 decimal digits per one octet, 4 octets,
   3 dots separating octets, 1 null terminator */
#define IP_LEN 16

typedef struct socket_node_s {
  struct socket_node_s *prev;
  struct socket_node_s *next;
  struct pollfd pfd;
  // char ip[IP_LEN]; // we never actually use it after addition

} socket_node_t;

int prepare_master_socket(char *server_ip, int server_port);

void add_sock_to_list(int sock_to_add, socket_node_t **sock_list,
                      int *list_size);

void del_sock_from_list(int sock_to_del, socket_node_t **sock_list,
                        int *list_size);

int handle_connection(int master_socket, socket_node_t **sock_list,
                      int *list_size);
int handle_disconnection(int client_socket, socket_node_t **sock_list,
                         int *list_size);

int main(int argc, const char *argv[]) {

  if (argc != 3) {
    printf("Requires two arguments: server_ip server_port\n");
    return -1;
  }

  // char server_ip[] = LISTEN_IP;
  // int server_port = LISTEN_PORT;

  char server_ip[IP_LEN] = {'\0'};
  strncpy(server_ip, argv[1], IP_LEN - 1);

  int server_port = atoi(argv[2]);

  printf("Going to listen on %s:%u\n", server_ip, server_port);

  int master_socket = prepare_master_socket(server_ip, server_port);
  if (master_socket < 0) {
    return -1;
  }

  /* TODO: would be great to keep "fd to ip" relations,
    so we could log ips when client sends a message or disconnects */
  socket_node_t *sock_list = NULL;
  int list_size = 0;
  add_sock_to_list(master_socket, &sock_list, &list_size);

  struct pollfd *pfds = NULL;
  int pfds_size = 0;

  // infinite timeout
  int timeout_msec = -1;
  //   int timeout_msec = 2000;

  while (1) {

    // change only if some socket was either added or removed
    if (list_size != pfds_size) {

      if (pfds != NULL) {
        free(pfds);
      }

      pfds_size = list_size;
      pfds = malloc(pfds_size * sizeof(struct pollfd));
    }

    // fill pfds array which is actually used in poll()
    socket_node_t *current_node = sock_list;
    int i = 0;

    while (current_node != NULL) {
      pfds[i] = current_node->pfd;
      ++i;
      current_node = current_node->next;
    }

    int rv = poll(pfds, pfds_size, timeout_msec);

    if (rv < 0) {

      if (errno != EINTR && errno != EAGAIN) {
        // replaced perror() with printf() everywhere
        printf("poll() failed %u %s\n", errno, strerror(errno));
      }

    } else if (rv > 0) {

      i = 0;

      while (i < pfds_size) {

        if (pfds[i].revents & POLLIN) {

          if (pfds[i].fd == master_socket) {

            handle_connection(master_socket, &sock_list, &list_size);

          } else {

            // must be a message from a client
            char rx_buf[RX_BUFF_SIZE] = {'\0'};
            int read_bytes = read(pfds[i].fd, rx_buf, sizeof(rx_buf));
            printf("received %d bytes from client with fd %d \n", read_bytes,
                   pfds[i].fd);

            if (read_bytes > 0) {

              // send received message back to the client
              int written_bytes = write(pfds[i].fd, rx_buf, strlen(rx_buf));

              if (written_bytes < 0) {
                printf("write() failed %u %s\n", errno, strerror(errno));
              }
            }

            else if (read_bytes == 0) {
              handle_disconnection(pfds[i].fd, &sock_list, &list_size);

            } else {
              printf("read() failed %u %s\n", errno, strerror(errno));
              handle_disconnection(pfds[i].fd, &sock_list, &list_size);
            }
          }

          // this seems to be enough, this is not select() with its FD_SETs
          pfds[i].revents = 0;
        }

        ++i;
      }

    } else {

      printf("poll() timeout %d is expired\n", timeout_msec);
    }
  }

  return 0;
}

int prepare_master_socket(char *server_ip, int server_port) {

  int master_socket = 0;

  if ((master_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    // replaced perror() with printf() everywhere
    printf("socket() failed %u %s\n", errno, strerror(errno));
    return -1;
  }

  struct sockaddr_in serveraddr;

  memset((char *)&serveraddr, '\0', sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  inet_pton(AF_INET, server_ip, &serveraddr.sin_addr.s_addr);
  serveraddr.sin_port = htons(server_port);

  int enable = 1;
  if (setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, &enable,
                 sizeof(enable)) < 0) {
    printf("setsockopt() with REUSEADDR option failed %u %s\n", errno,
           strerror(errno));
    return -1;
  }

  if (bind(master_socket, (const struct sockaddr *)&serveraddr,
           sizeof(serveraddr)) < 0) {
    printf("bind() failed %u %s\n", errno, strerror(errno));
    return -1;
  }

  if (fcntl(master_socket, F_SETFL, O_NONBLOCK) < 0) {
    printf("fcntl() setting master_socket to non-blocking mode failed %u %s\n",
           errno, strerror(errno));
    return -1;
  }

  if (listen(master_socket, SOMAXCONN) < 0) {
    printf("listen() failed %u %s\n", errno, strerror(errno));
    return -1;
  }

  return master_socket;
}

void add_sock_to_list(int sock_to_add, socket_node_t **sock_list,
                      int *list_size) {

  socket_node_t *curr_node = *sock_list;

  // if the list is not empty, reach the end
  while (curr_node != NULL && curr_node->next != NULL) {
    curr_node = curr_node->next;
  }

  // TODO: we never handle malloc() failure
  socket_node_t *new_node = malloc(sizeof(socket_node_t));
  new_node->prev = curr_node;
  new_node->next = NULL;

  // check in case list was empty
  if (curr_node != NULL) {
    curr_node->next = new_node;
  } else {
    *sock_list = new_node;
  }

  new_node->pfd.fd = sock_to_add;
  new_node->pfd.events = POLLIN;
  // memcpy(new_node->ip, ip_to_add, sizeof(new_node->ip));

  ++(*list_size);
}

void del_sock_from_list(int sock_to_del, socket_node_t **sock_list,
                        int *list_size) {

  // if the list is empty, nothing to do
  if (*sock_list == NULL) {
    return;
  }

  socket_node_t *curr_node = *sock_list;

  while (curr_node != NULL) {

    if (curr_node->pfd.fd == sock_to_del) {
      if (curr_node->prev != NULL) {
        curr_node->prev->next = curr_node->next;
      } else {
        *sock_list = curr_node->next;
      }

      if (curr_node->next != NULL) {
        curr_node->next->prev = curr_node->prev;
      }

      free(curr_node);
      --(*list_size);
      return;
    }
    curr_node = curr_node->next;
  }
}

int handle_connection(int master_socket, socket_node_t **sock_list,
                      int *list_size) {
  struct sockaddr_in clientaddr;
  int clientlen = sizeof(clientaddr);

  int client_socket =
      accept(master_socket, (struct sockaddr *)&clientaddr, &clientlen);

  if (client_socket < 0) {
    printf("accept() failed %u %s\n", errno, strerror(errno));
  }

  char *client_ip = NULL;
  client_ip = inet_ntoa(clientaddr.sin_addr);
  if (client_ip == NULL) {
    printf("inet_ntoa() failed %u %s\n", errno, strerror(errno));
  }

  // apparently would not have needed it if we used accept4()
  if (fcntl(client_socket, F_SETFL, O_NONBLOCK) < 0) {
    printf("fcntl() setting master_socket to non-blocking mode failed %u %s\n",
           errno, strerror(errno));
    return -1;
  }

  printf("server established connection with %s:%d - fd %d \n", client_ip,
         ntohs(clientaddr.sin_port), client_socket);

  add_sock_to_list(client_socket, /* client_ip, */ sock_list, list_size);
}

int handle_disconnection(int client_socket, socket_node_t **sock_list,
                         int *list_size) {

  close(client_socket);

  printf("client with fd %d disconnected\n", client_socket);

  del_sock_from_list(client_socket, sock_list, list_size);
}