#define _GNU_SOURCE
#include <features.h>

#include <proton/condition.h>
#include <proton/message.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "bridge.h"
#include "rb.h"
#include "utils.h"

static struct addrinfo *peer_addrinfo;
static pn_message_t *m_glbl = NULL;

static int prepare_send_socket(app_data_t *app, int *send_sock) {
    struct addrinfo hints;

    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_family = AF_UNSPEC,
    hints.ai_socktype = SOCK_DGRAM,
    hints.ai_protocol = 0,
    hints.ai_flags = AI_ADDRCONFIG;

    int err = getaddrinfo(app->peer_host, app->peer_port, &hints, &peer_addrinfo);

    if (err != 0) {
        fprintf(
            stderr,
            "prepare_send_socket: getaddrinfo returned non-zero value: %d\n",
            errno);
        perror("Error");
        freeaddrinfo(peer_addrinfo);
        return -1;
    }

    *send_sock = socket(peer_addrinfo->ai_family, peer_addrinfo->ai_socktype,
                        peer_addrinfo->ai_protocol);
    if (*send_sock == -1) {
        fprintf(stderr, "prepare_send_socket: socket returned -1\n");
        perror("Error");
        freeaddrinfo(peer_addrinfo);
        return -1;
    }

    char addrstr[100];

    void *ptr = &((struct sockaddr_in *)peer_addrinfo->ai_addr)->sin_addr;
    inet_ntop(peer_addrinfo->ai_family, ptr, addrstr, sizeof(addrstr));

    printf("Peer socket(%d) %s:%d\n",
           *send_sock,
           addrstr,
           ntohs((((struct sockaddr_in *)((struct sockaddr *)
                                              peer_addrinfo->ai_addr))
                      ->sin_port)));

    return 0;
}

static int decode_message(app_data_t *app, int send_sock, pn_rwbytes_t data) {
    pn_message_t *m;

    // Use a static message with pn_message_clear(...)
    if ((m = m_glbl) == NULL) {
        m_glbl = pn_message();
        m = m_glbl;
    } else {
        pn_message_clear(m);
    }

    int err = pn_message_decode(m, data.start, data.size);
    if (!err) {
        pn_data_t *body = pn_message_body(m);
        if (pn_data_next(body)) {
            pn_bytes_t b = pn_data_get_bytes(body);
            if (b.start != NULL) {
                int send_flags = MSG_DONTWAIT;

                ssize_t sent_bytes = sendto(send_sock, b.start, b.size, send_flags,
                                            peer_addrinfo->ai_addr, peer_addrinfo->ai_addrlen);
                if (sent_bytes <= 0) {
                    // MSG_DONTWAIT is set
                    app->would_block++;
                    perror("error send\n");
                    return 1;
                }
                app->received++;
            }
        }
    } else {
        // Record the error.  Don't exit immediately
        //
        app->decore_errors++;

        return 1;
    }

    return 0;
}

void socket_snd_th_cleanup(void *app_ptr) {
    app_data_t *app = (app_data_t *)app_ptr;

    if (app) {
        app->socket_snd_th_running = 0;
    }

    fprintf(stderr, "Exit SOCKET thread...\n");
}

void *socket_snd_th(void *app_ptr) {
    pthread_cleanup_push(socket_snd_th_cleanup, app_ptr);

    app_data_t *app = (app_data_t *)app_ptr;
    int send_sock;

    // Create the send socket
    if (prepare_send_socket(app, &send_sock) == -1) {
        fprintf(stderr, "Failed to create socket -- exiting!");
        return NULL;
    }

    printf("%s: %s start...\n", __FILE__, __func__);

    clock_gettime(CLOCK_MONOTONIC, &app->rbin->total_t2);

    while (1) {
        pn_rwbytes_t *msg = rb_get(app->rbin);
        decode_message(app, send_sock, *msg);
    }

    if (send_sock != -1) {
        close(send_sock);
        fprintf(stdout, "Socket closed\n");
    }

    pthread_cleanup_pop(1);

    return NULL;
}