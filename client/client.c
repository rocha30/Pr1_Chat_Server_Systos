#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../Protocolo.h"

static int      sockfd      = -1;
static char     my_username[32];
static volatile int running = 1;

/* ──────────────────────────────────────────
   Utilidad de envío
   ────────────────────────────────────────── */

static void send_packet(const ChatPacket *pkt)
{
    const char *buf = (const char *)pkt;
    size_t total = sizeof(ChatPacket), sent = 0;
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

/* ──────────────────────────────────────────
   Ayuda
   ────────────────────────────────────────── */

static void print_help(void)
{
    printf("\nComandos disponibles:\n");
    printf("  /broadcast <mensaje>            Enviar mensaje a todos\n");
    printf("  /msg <usuario> <mensaje>        Mensaje directo\n");
    printf("  /status <ACTIVE|BUSY|INACTIVE>  Cambiar status\n");
    printf("  /list                           Listar usuarios conectados\n");
    printf("  /info <usuario>                 Info de un usuario\n");
    printf("  /help                           Mostrar esta ayuda\n");
    printf("  /exit                           Salir del chat\n");
    printf("  <texto libre>                   Broadcast (equivale a /broadcast)\n\n");
}

/* ──────────────────────────────────────────
   Thread receptor de mensajes del servidor
   ────────────────────────────────────────── */

static void *recv_thread(void *arg)
{
    (void)arg;
    ChatPacket pkt;

    while (running) {
        ssize_t n = recv(sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
        if (n <= 0) {
            if (running) printf("\n[!] Conexion con el servidor perdida.\n");
            running = 0;
            break;
        }

        pkt.sender[31]   = '\0';
        pkt.target[31]   = '\0';
        pkt.payload[956] = '\0';

        switch (pkt.command) {
        case CMD_OK:
            printf("[OK] %s\n", pkt.payload);
            break;
        case CMD_ERROR:
            printf("[ERROR] %s\n", pkt.payload);
            break;
        case CMD_MSG:
            if (strcmp(pkt.target, "ALL") == 0)
                printf("[%s -> todos] %s\n", pkt.sender, pkt.payload);
            else
                printf("[DM de %s] %s\n", pkt.sender, pkt.payload);
            break;
        case CMD_USER_LIST:
            printf("[Usuarios] %s\n", pkt.payload);
            break;
        case CMD_USER_INFO:
            printf("[Info] %s\n", pkt.payload);
            break;
        case CMD_DISCONNECTED:
            printf("[!] %s se desconecto\n", pkt.payload);
            break;
        default:
            printf("[?] Paquete desconocido: cmd=%d\n", pkt.command);
            break;
        }
        fflush(stdout);
    }
    return NULL;
}

/* ──────────────────────────────────────────
   Main
   ────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    strncpy(my_username, argv[1], 31);
    my_username[31] = '\0';
    const char *server_ip = argv[2];
    int port = atoi(argv[3]);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "IP invalida: %s\n", server_ip);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect"); return 1;
    }

    /* ── Registro ── */
    ChatPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.command = CMD_REGISTER;
    strncpy(pkt.sender,  my_username, 31);
    strncpy(pkt.payload, my_username, 956);
    pkt.payload_len = (uint16_t)strlen(pkt.payload);
    send_packet(&pkt);

    /* Esperar respuesta del servidor */
    ssize_t n = recv(sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
    if (n != (ssize_t)sizeof(pkt)) {
        fprintf(stderr, "Error al recibir respuesta del servidor\n");
        close(sockfd); return 1;
    }
    pkt.payload[956] = '\0';
    if (pkt.command == CMD_ERROR) {
        printf("[ERROR] Registro rechazado: %s\n", pkt.payload);
        close(sockfd); return 1;
    }
    printf("[OK] %s\n", pkt.payload);
    print_help();

    /* ── Thread receptor ── */
    pthread_t rtid;
    pthread_create(&rtid, NULL, recv_thread, NULL);
    pthread_detach(rtid);

    /* ── Bucle de entrada ── */
    char line[1024];
    while (running) {
        if (!fgets(line, sizeof(line), stdin)) break;
        if (!running) break;

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        memset(&pkt, 0, sizeof(pkt));
        strncpy(pkt.sender, my_username, 31);

        if (strncmp(line, "/broadcast ", 11) == 0) {
            pkt.command = CMD_BROADCAST;
            strncpy(pkt.payload, line + 11, 956);

        } else if (strncmp(line, "/msg ", 5) == 0) {
            char *rest  = line + 5;
            char *space = strchr(rest, ' ');
            if (!space) { printf("Uso: /msg <usuario> <mensaje>\n"); continue; }
            *space = '\0';
            strncpy(pkt.target,  rest,      31);
            strncpy(pkt.payload, space + 1, 956);
            pkt.command = CMD_DIRECT;

        } else if (strncmp(line, "/status ", 8) == 0) {
            pkt.command = CMD_STATUS;
            strncpy(pkt.payload, line + 8, 956);

        } else if (strcmp(line, "/list") == 0) {
            pkt.command = CMD_LIST;

        } else if (strncmp(line, "/info ", 6) == 0) {
            pkt.command = CMD_INFO;
            strncpy(pkt.target, line + 6, 31);

        } else if (strcmp(line, "/help") == 0) {
            print_help();
            continue;

        } else if (strcmp(line, "/exit") == 0) {
            pkt.command = CMD_LOGOUT;
            send_packet(&pkt);
            running = 0;
            break;

        } else if (line[0] == '/') {
            printf("Comando desconocido. Escribe /help para ver los comandos.\n");
            continue;

        } else {
            /* Texto libre = broadcast */
            pkt.command = CMD_BROADCAST;
            strncpy(pkt.payload, line, 956);
        }

        pkt.payload_len = (uint16_t)strlen(pkt.payload);
        send_packet(&pkt);
    }

    close(sockfd);
    return 0;
}
