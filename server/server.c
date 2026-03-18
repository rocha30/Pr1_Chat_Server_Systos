#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../Protocolo.h"

#define MAX_CLIENTS 100

/* ── Estructura de sesión por cliente ── */
typedef struct {
    char     username[32];
    char     ip[INET_ADDRSTRLEN];
    char     status[16];
    int      sockfd;
    int      activo;
    time_t   ultimo_mensaje;
} Cliente;

static Cliente         lista[MAX_CLIENTS];
static int             num_clientes    = 0;
static pthread_mutex_t mutex_lista     = PTHREAD_MUTEX_INITIALIZER;
static int             inactivity_secs = INACTIVITY_TIMEOUT;

/* ──────────────────────────────────────────
   Utilidades de envío
   ────────────────────────────────────────── */

static void send_packet(int fd, const ChatPacket *pkt)
{
    ssize_t sent = 0;
    const char *buf = (const char *)pkt;
    size_t total = sizeof(ChatPacket);
    while (sent < (ssize_t)total) {
        ssize_t n = send(fd, buf + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += n;
    }
}

static void send_ok(int fd, const char *target, const char *msg)
{
    ChatPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.command = CMD_OK;
    strncpy(pkt.sender,  "SERVER", 31);
    strncpy(pkt.target,  target,   31);
    strncpy(pkt.payload, msg,      956);
    pkt.payload_len = (uint16_t)strlen(pkt.payload);
    send_packet(fd, &pkt);
}

static void send_error(int fd, const char *target, const char *msg)
{
    ChatPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.command = CMD_ERROR;
    strncpy(pkt.sender,  "SERVER", 31);
    strncpy(pkt.target,  target,   31);
    strncpy(pkt.payload, msg,      956);
    pkt.payload_len = (uint16_t)strlen(pkt.payload);
    send_packet(fd, &pkt);
}

/* Envía pkt a todos los clientes activos.
   Si exclude_fd >= 0, omite ese socket. */
static void broadcast_to_all(const ChatPacket *pkt, int exclude_fd)
{
    /* Copia local de sockets para no bloquear mientras enviamos */
    int fds[MAX_CLIENTS];
    int count = 0;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].activo && lista[i].sockfd != exclude_fd)
            fds[count++] = lista[i].sockfd;
    }
    pthread_mutex_unlock(&mutex_lista);

    for (int i = 0; i < count; i++)
        send_packet(fds[i], pkt);
}

/* ──────────────────────────────────────────
   Gestión de lista de clientes
   ────────────────────────────────────────── */

/* Remueve al cliente con sockfd de la lista (debe llamarse con mutex tomado). */
static void remove_client_locked(int sockfd)
{
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].sockfd == sockfd) {
            for (int j = i; j < num_clientes - 1; j++)
                lista[j] = lista[j + 1];
            num_clientes--;
            return;
        }
    }
}

/* Notifica CMD_DISCONNECTED a todos los demás clientes. */
static void notify_disconnected(const char *username, int exclude_fd)
{
    ChatPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.command = CMD_DISCONNECTED;
    strncpy(pkt.sender,  "SERVER",  31);
    strncpy(pkt.target,  "ALL",     31);
    strncpy(pkt.payload, username,  956);
    pkt.payload_len = (uint16_t)strlen(pkt.payload);
    broadcast_to_all(&pkt, exclude_fd);
}

/* Actualiza el timestamp de último mensaje (debe llamarse sin mutex). */
static void touch_activity(int sockfd)
{
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].sockfd == sockfd && lista[i].activo) {
            lista[i].ultimo_mensaje = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);
}

/* ──────────────────────────────────────────
   Manejadores de comandos
   ────────────────────────────────────────── */

static void handle_broadcast(int sockfd, const ChatPacket *pkt)
{
    touch_activity(sockfd);

    ChatPacket msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = CMD_MSG;
    strncpy(msg.sender,  pkt->sender,  31);
    strncpy(msg.target,  "ALL",        31);
    strncpy(msg.payload, pkt->payload, 956);
    msg.payload_len = pkt->payload_len;

    /* El emisor también recibe su propio mensaje */
    broadcast_to_all(&msg, -1);
}

static void handle_direct(int sockfd, const ChatPacket *pkt)
{
    touch_activity(sockfd);

    /* Buscar socket del destinatario */
    int target_fd = -1;
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].activo && strcmp(lista[i].username, pkt->target) == 0) {
            target_fd = lista[i].sockfd;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    if (target_fd < 0) {
        send_error(sockfd, pkt->sender, "Destinatario no conectado");
        return;
    }

    ChatPacket msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = CMD_MSG;
    strncpy(msg.sender,  pkt->sender,  31);
    strncpy(msg.target,  pkt->target,  31);
    strncpy(msg.payload, pkt->payload, 956);
    msg.payload_len = pkt->payload_len;
    send_packet(target_fd, &msg);
}

static void handle_list(int sockfd, const ChatPacket *pkt)
{
    char buf[957];
    int  pos = 0;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (!lista[i].activo) continue;
        int w = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s,%s;", lista[i].username, lista[i].status);
        if (w < 0 || pos + w >= (int)sizeof(buf)) break;
        pos += w;
    }
    pthread_mutex_unlock(&mutex_lista);

    if (pos > 0 && buf[pos - 1] == ';') buf[--pos] = '\0';

    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.command = CMD_USER_LIST;
    strncpy(resp.sender,  "SERVER",     31);
    strncpy(resp.target,  pkt->sender,  31);
    strncpy(resp.payload, buf,          956);
    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(sockfd, &resp);
}

static void handle_info(int sockfd, const ChatPacket *pkt)
{
    char buf[957];
    int  found = 0;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].activo && strcmp(lista[i].username, pkt->target) == 0) {
            snprintf(buf, sizeof(buf), "%s,%s", lista[i].ip, lista[i].status);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    if (!found) {
        send_error(sockfd, pkt->sender, "Usuario no conectado");
        return;
    }

    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.command = CMD_USER_INFO;
    strncpy(resp.sender,  "SERVER",    31);
    strncpy(resp.target,  pkt->sender, 31);
    strncpy(resp.payload, buf,         956);
    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(sockfd, &resp);
}

static void handle_status(int sockfd, const ChatPacket *pkt)
{
    const char *ns = pkt->payload;
    if (strcmp(ns, STATUS_ACTIVO)   != 0 &&
        strcmp(ns, STATUS_OCUPADO)  != 0 &&
        strcmp(ns, STATUS_INACTIVO) != 0) {
        send_error(sockfd, pkt->sender, "Status invalido. Usa ACTIVE, BUSY o INACTIVE");
        return;
    }

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].sockfd == sockfd && lista[i].activo) {
            strncpy(lista[i].status, ns, 15);
            lista[i].ultimo_mensaje = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s", ns);
    send_ok(sockfd, pkt->sender, msg);
}

/* Retorna el username del cliente con ese sockfd (vacío si no encontrado).
   Remueve al cliente de la lista y cierra su socket.
   Devuelve 1 si había usuario activo, 0 si no. */
static int cleanup_client(int sockfd, char *out_username)
{
    out_username[0] = '\0';
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < num_clientes; i++) {
        if (lista[i].sockfd == sockfd && lista[i].activo) {
            strncpy(out_username, lista[i].username, 31);
            out_username[31] = '\0';
            remove_client_locked(sockfd);
            pthread_mutex_unlock(&mutex_lista);
            close(sockfd);
            return 1;
        }
    }
    pthread_mutex_unlock(&mutex_lista);
    return 0;
}

/* ──────────────────────────────────────────
   Thread por cliente
   ────────────────────────────────────────── */

static void *client_thread(void *arg)
{
    int sockfd = *(int *)arg;
    free(arg);

    ChatPacket pkt;

    /* ── Registro ── */
    ssize_t n = recv(sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
    if (n != (ssize_t)sizeof(pkt) || pkt.command != CMD_REGISTER) {
        close(sockfd);
        return NULL;
    }
    pkt.sender[31]  = '\0';
    pkt.payload[956] = '\0';

    /* Obtener IP del cliente */
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getpeername(sockfd, (struct sockaddr *)&addr, &addrlen);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    pthread_mutex_lock(&mutex_lista);

    /* Validar duplicados de nombre e IP.
       La IP solo se verifica si NO es localhost (127.0.0.1),
       para permitir pruebas con multiples clientes en la misma maquina. */
    int dup = 0;
    const char *dup_msg = "Servidor lleno";
    int check_ip = (strcmp(client_ip, "127.0.0.1") != 0);
    for (int i = 0; i < num_clientes; i++) {
        if (!lista[i].activo) continue;
        if (strcmp(lista[i].username, pkt.sender) == 0) {
            dup = 1; dup_msg = "Usuario ya existe"; break;
        }
        if (check_ip && strcmp(lista[i].ip, client_ip) == 0) {
            dup = 1; dup_msg = "Ya hay una sesion activa desde esta IP"; break;
        }
    }
    if (dup || num_clientes >= MAX_CLIENTS) {
        pthread_mutex_unlock(&mutex_lista);
        send_error(sockfd, pkt.sender, dup_msg);
        close(sockfd);
        return NULL;
    }

    int idx = num_clientes++;
    strncpy(lista[idx].username, pkt.sender,   31);
    strncpy(lista[idx].ip,       client_ip,    INET_ADDRSTRLEN - 1);
    strncpy(lista[idx].status,   STATUS_ACTIVO, 15);
    lista[idx].sockfd          = sockfd;
    lista[idx].activo          = 1;
    lista[idx].ultimo_mensaje  = time(NULL);
    pthread_mutex_unlock(&mutex_lista);

    printf("[+] Conectado: %s (%s)\n", lista[idx].username, lista[idx].ip);

    char welcome[64];
    snprintf(welcome, sizeof(welcome), "Bienvenido %s", pkt.sender);
    send_ok(sockfd, pkt.sender, welcome);

    /* ── Bucle principal ── */
    while (1) {
        n = recv(sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
        if (n <= 0) break;   /* desconexión abrupta */

        pkt.sender[31]   = '\0';
        pkt.target[31]   = '\0';
        pkt.payload[956] = '\0';

        touch_activity(sockfd);

        switch (pkt.command) {
        case CMD_BROADCAST: handle_broadcast(sockfd, &pkt); break;
        case CMD_DIRECT:    handle_direct(sockfd, &pkt);    break;
        case CMD_LIST:      handle_list(sockfd, &pkt);      break;
        case CMD_INFO:      handle_info(sockfd, &pkt);      break;
        case CMD_STATUS:    handle_status(sockfd, &pkt);    break;
        case CMD_LOGOUT: {
            char uname[32];
            strncpy(uname, pkt.sender, 31); uname[31] = '\0';
            send_ok(sockfd, uname, "Hasta luego");
            cleanup_client(sockfd, uname);
            notify_disconnected(uname, -1);
            printf("[-] Logout: %s\n", uname);
            return NULL;
        }
        default:
            send_error(sockfd, pkt.sender, "Comando desconocido");
            break;
        }
    }

    /* Desconexión abrupta */
    char uname[32];
    if (cleanup_client(sockfd, uname)) {
        printf("[-] Desconexión abrupta: %s\n", uname);
        notify_disconnected(uname, -1);
    }
    return NULL;
}

/* ──────────────────────────────────────────
   Thread de detección de inactividad
   ────────────────────────────────────────── */

static void *inactivity_thread(void *arg)
{
    (void)arg;
    while (1) {
        sleep(10);
        time_t now = time(NULL);

        /* Recoger índices a notificar sin enviar dentro del lock */
        int    fds[MAX_CLIENTS];
        char   names[MAX_CLIENTS][32];
        int    count = 0;

        pthread_mutex_lock(&mutex_lista);
        for (int i = 0; i < num_clientes; i++) {
            if (!lista[i].activo) continue;
            if (strcmp(lista[i].status, STATUS_INACTIVO) == 0) continue;
            if (now - lista[i].ultimo_mensaje >= inactivity_secs) {
                strncpy(lista[i].status, STATUS_INACTIVO, 15);
                fds[count]   = lista[i].sockfd;
                strncpy(names[count], lista[i].username, 31);
                names[count][31] = '\0';
                count++;
            }
        }
        pthread_mutex_unlock(&mutex_lista);

        for (int i = 0; i < count; i++) {
            ChatPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.command = CMD_MSG;
            strncpy(pkt.sender,  "SERVER",   31);
            strncpy(pkt.target,  names[i],   31);
            strncpy(pkt.payload, "Tu status cambio a INACTIVE por inactividad", 956);
            pkt.payload_len = (uint16_t)strlen(pkt.payload);
            send_packet(fds[i], &pkt);
            printf("[!] %s -> INACTIVE (inactividad)\n", names[i]);
        }
    }
    return NULL;
}

/* ──────────────────────────────────────────
   Main
   ────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s <puerto> [timeout_inactividad_segundos]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto invalido\n");
        return 1;
    }

    if (argc == 3) {
        inactivity_secs = atoi(argv[2]);
        if (inactivity_secs <= 0) inactivity_secs = INACTIVITY_TIMEOUT;
    }
    printf("[*] Timeout de inactividad: %d segundos\n", inactivity_secs);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr = {0};
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("[*] Servidor escuchando en puerto %d\n", port);

    pthread_t inact_tid;
    pthread_create(&inact_tid, NULL, inactivity_thread, NULL);
    pthread_detach(inact_tid);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        int *fd_ptr = malloc(sizeof(int));
        if (!fd_ptr) { close(cfd); continue; }
        *fd_ptr = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, fd_ptr) != 0) {
            perror("pthread_create"); free(fd_ptr); close(cfd); continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
