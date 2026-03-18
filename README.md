# Chat Cliente-Servidor — CC3064 Sistemas Operativos, UVG 2026

Sistema de chat en C sobre TCP/Linux usando protocolo binario de struct fijo de 1024 bytes,
compatible con el protocolo comun de la clase (Protocolo.pdf / Protocolo.h).

---

## Dependencias

- GCC con soporte pthread (estandar en Linux)
- Make

No requiere librerias externas ni Protocol Buffers.

---

## Compilacion

```bash
make          # compila servidor y cliente
make clean    # limpia binarios
```

O por separado:

```bash
make -C server    # solo servidor  →  server/servidor
make -C client    # solo cliente   →  client/cliente
```

---

## Uso

### Servidor

```bash
./server/servidor <puerto>
# Ejemplo:
./server/servidor 8080
```

### Cliente

```bash
./client/cliente <username> <IP_servidor> <puerto>
# Ejemplo:
./client/cliente alice 127.0.0.1 8080
./client/cliente bob   192.168.1.10 8080
```

---

## Comandos del cliente

| Comando                          | Descripcion                                 |
|----------------------------------|---------------------------------------------|
| `/broadcast <mensaje>`           | Envia mensaje a todos los usuarios          |
| `/msg <usuario> <mensaje>`       | Mensaje directo a un usuario                |
| `/status <ACTIVE\|BUSY\|INACTIVE>` | Cambia tu status                          |
| `/list`                          | Lista todos los usuarios conectados         |
| `/info <usuario>`                | Muestra IP y status de un usuario           |
| `/help`                          | Muestra la ayuda                            |
| `/exit`                          | Cierra sesion y sale                        |
| `<texto libre>`                  | Equivale a `/broadcast <texto>`             |

---

## Protocolo

Formato: struct binario fijo de **1024 bytes** (`ChatPacket`), definido en `Protocolo.h`.

```
uint8_t  command      —  1 byte   (tipo de mensaje)
uint16_t payload_len  —  2 bytes  (longitud util del payload)
char     sender[32]   — 32 bytes  (remitente)
char     target[32]   — 32 bytes  (destinatario, vacio = todos)
char     payload[957] — 957 bytes (contenido)
```

| Comando             | Valor | Direccion         |
|---------------------|-------|-------------------|
| CMD_REGISTER        |  1    | Cliente → Servidor |
| CMD_BROADCAST       |  2    | Cliente → Servidor |
| CMD_DIRECT          |  3    | Cliente → Servidor |
| CMD_LIST            |  4    | Cliente → Servidor |
| CMD_INFO            |  5    | Cliente → Servidor |
| CMD_STATUS          |  6    | Cliente → Servidor |
| CMD_LOGOUT          |  7    | Cliente → Servidor |
| CMD_OK              |  8    | Servidor → Cliente |
| CMD_ERROR           |  9    | Servidor → Cliente |
| CMD_MSG             | 10    | Servidor → Cliente |
| CMD_USER_LIST       | 11    | Servidor → Cliente |
| CMD_USER_INFO       | 12    | Servidor → Cliente |
| CMD_DISCONNECTED    | 13    | Servidor → Cliente |

Valores de status: `ACTIVE`, `BUSY`, `INACTIVE`

Inactividad: el servidor cambia el status a `INACTIVE` tras `INACTIVITY_TIMEOUT` segundos (default 60).

---

## Interoperabilidad

Para conectar con la implementacion de otro grupo:
- Usar exactamente el mismo `Protocolo.h` (struct identico)
- Mismo orden de campos y tamanios
- `recv(..., MSG_WAITALL)` lee exactamente 1024 bytes por paquete

---

## Estructura del proyecto

```
Proyecto1/
├── Protocolo.h        # struct compartido (identico en todos los grupos)
├── Makefile           # compilacion global
├── server/
│   ├── server.c       # servidor multithreading
│   └── Makefile
└── client/
    ├── client.c       # cliente con hilo receptor
    └── Makefile
```

---

## Notas de implementacion

- El servidor crea un `pthread` por cliente conectado.
- Un thread separado revisa inactividad cada 10 segundos.
- El broadcast incluye al emisor (segun especificacion del protocolo).
- No se rechaza por IP duplicada (solo por username duplicado).
- `MSG_WAITALL` garantiza lectura completa de 1024 bytes sin framing extra.
