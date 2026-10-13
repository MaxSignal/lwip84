# Network console

The screen lwIP-84+ starts with: a console for testing the network and the
crypto code, and an example of the socket API. **APPS, 3** returns to it
from the menu.

| File | Contents |
|------|----------|
| `apps.c` | socket tests (HTTP, HTTPS, TCP echo, download), SNTP, clock, crypto self test |
| `demo.c` | ping and DNS lookup with lwIP's raw API |

## Keys

| Key | Action |
|-----|--------|
| Y= | status: USB state and registers, NTB counters, gateway, DNS, free heap, font |
| WINDOW | ping the gateway |
| ZOOM | DNS lookup of example.com |
| TRACE | HTTP GET http://example.com/ |
| 2nd, MATH | HTTPS GET https://example.com/ |
| MATH | HTTPS GET https://gateway:4433/ |
| GRAPH | TCP echo with gateway:8084 |
| VARS | download test from gateway:8085 (prints bytes/s) |
| PRGM | crypto self test and timing (SHA-256, Poly1305, ChaCha20, AES, X25519 RFC 7748 vector) |
| MODE / STAT | SNTP time sync / show the time |
| CLEAR, UP/DOWN | clear, scroll back through the last 128 lines |
| APPS | application menu |

## Test servers on the PC

The gateway is the PC (192.168.84.1 with NetworkManager's shared mode).

```
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -subj /CN=calc
openssl s_server -accept 4433 -cert cert.pem -key key.pem -tls1_3 -www   # MATH
socat TCP-LISTEN:8084,fork,reuseaddr EXEC:cat                            # GRAPH
head -c 100000 /dev/urandom | nc -l -p 8085                              # VARS
```

## The socket API

`apps.c` shows the pattern of `lib/net/sock84.h`: create a socket with an
event callback, write the request when it is connected, read in the main
loop until it is no longer active:

```c
app_sock = sock_new(SOCK_TCP, 30000u, app_event);
sock_connect(app_sock, host, 80);

static void app_event(struct sock *s, uint8_t ev, uint8_t a, int8_t err)
{
    if (ev == SOCK_EV_STATE && a == SOCK_CONNECTED)
        sock_write(s, app_req, strlen(app_req));
}

void app_poll(void)                     /* main loop */
{
    while ((n = sock_available(app_sock)) != 0)
        n = sock_read(app_sock, buf, n > sizeof(buf) ? sizeof(buf) : n);
    if (!sock_is_active(app_sock))
        sock_free(app_sock);
}
```
