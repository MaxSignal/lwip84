/* browser.c - a text web browser (HTTP/1.0 and HTTPS through lwIP.h).
 *
 * The page is rendered while it arrives: tags are dropped, block elements
 * start new lines, text is word-wrapped to 24 columns and stored line by
 * line in extended RAM, links are numbered "[n]" and their targets stored
 * there too.  RAM use is a few hundred bytes plus the socket.
 *
 * Keys:  Y=  enter a URL             0-9  follow link n (ENTER)
 *        UP/DOWN  scroll   LEFT/RIGHT  page up/down
 *        DEL  back       ENTER  reload    CLEAR  stop
 *        MODE  quit      ALPHA/2nd  letters and symbols while typing
 */
#include <string.h>
#include <stdlib.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"
#include "lwIP.h"
#include "tls84.h"

#define URL_MAX 159
#define COLS CON_COLS
#define ROWS 9
#define X_TEXT (XRAM_BROWSER)
#define MAX_LINES 2048u                 /* 48 KiB of text */
#define X_LINKS (XRAM_BROWSER + 0xC000UL)
#define LINKS_SIZE 0x3000u
#define X_LINKOFF (XRAM_BROWSER + 0xF000UL)
#define MAX_LINKS 255
#define X_HIST (XRAM_BROWSER + 0xF200UL)
#define HIST_MAX 8

enum { B_IDLE, B_LOADING };
enum { H_STATUS, H_HEADER, H_BODY, H_CHUNK_SIZE, H_CHUNK_DATA, H_CHUNK_END, H_DONE };
enum { T_TEXT, T_TAG, T_COMMENT, T_ENTITY, T_SKIP };

#define X_STATE (XRAM_BROWSER + 0xF800UL)  /* url, host, title while not running */

static struct lwip_socket *sock;
static uint8_t state, editing, redirects, hist_n;
static uint16_t nlines, top, nlinks, link_used;
static uint32_t rx_bytes, last_draw, t_start;

/* HTTP */
static uint8_t hstate, chunked, is_html, http_status, do_redirect;
static uint32_t chunk_left;
static uint8_t hline_n;

/* HTML renderer */
static uint8_t tstate, in_title, skip_depth_tag, pending_space, blank_lines, in_pre;
static uint8_t tag_n, ent_n;
static uint8_t col, word_n, utf8_skip;

/* the buffers exist only while the browser runs (browser and SSH client
 * never run at the same time, so they share the heap); url, host and title
 * are kept in extended RAM in between */
struct browser_mem {
    char url[URL_MAX + 1], host[64], title[COLS + 1];   /* saved: keep first */
    char edit_buf[URL_MAX + 1];
    char request[URL_MAX + 64 + 128];
    struct line_edit le;
    char msg[COLS + 1];
    char hline[128];
    char location[URL_MAX + 1];
    char tag[200];
    char ent[10];
    char line[COLS], word[COLS];
};
#define SAVED_SIZE (URL_MAX + 1 + 64 + COLS + 1)
static struct browser_mem *bm;
#define url (bm->url)
#define host (bm->host)
#define title (bm->title)
#define edit_buf (bm->edit_buf)
#define request (bm->request)
#define le (bm->le)
#define msg (bm->msg)
#define hline (bm->hline)
#define location (bm->location)
#define tag (bm->tag)
#define ent (bm->ent)
#define line (bm->line)
#define word (bm->word)

/* ------------------------------------------------------------------ */
/* page storage                                                        */

static void emit_line(void)
{
    if (nlines < MAX_LINES) {
        memset(line + col, ' ', COLS - col);
        xram_write(X_TEXT + (uint32_t)nlines * COLS, line, COLS);
        nlines++;
    }
    col = 0;
}

static void flush_word(void)
{
    uint8_t i = 0;
    if (!word_n)
        return;
    if (col && col + word_n + (pending_space ? 1 : 0) > COLS)
        emit_line();
    else if (col && pending_space)
        line[col++] = ' ';
    pending_space = 0;
    blank_lines = 0;
    while (i < word_n) {
        if (col == COLS)
            emit_line();
        line[col++] = word[i++];
    }
    word_n = 0;
}

static void out_char(char c)
{
    if (c == ' ') {
        flush_word();
        if (col)
            pending_space = 1;
        return;
    }
    if (word_n == COLS)
        flush_word();
    word[word_n++] = c;
}

/* end the current line; blank = also leave one empty line */
static void out_break(uint8_t blank)
{
    flush_word();
    pending_space = 0;
    if (col) {
        emit_line();
        blank_lines = 0;
    }
    if (blank && !blank_lines && nlines) {
        emit_line();
        blank_lines = 1;
    }
}

static void out_str(const char *s)
{
    while (*s)
        out_char(*s++);
}

static void add_link(const char *href, uint8_t n)
{
    char num[6];
    uint16_t off = link_used;
    if (nlinks >= MAX_LINKS || link_used + n + 1 > LINKS_SIZE)
        return;
    xram_write(X_LINKS + off, href, n);
    xram_write(X_LINKS + off + n, "", 1);
    xram_write(X_LINKOFF + nlinks * 2, &off, 2);
    link_used += n + 1;
    nlinks++;
    snprintf(num, sizeof(num), "[%u]", nlinks);
    if (pending_space || !col)
        flush_word();
    out_str(num);
}

static void get_link(uint16_t n, char *buf)
{
    uint16_t off;
    xram_read(X_LINKOFF + (n - 1) * 2, &off, 2);
    xram_read(X_LINKS + off, buf, URL_MAX);
    buf[URL_MAX] = 0;
}

/* ------------------------------------------------------------------ */
/* HTML                                                                */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* tag name equals s (tag buffer holds "name attr..." without '<' '>') */
static uint8_t tag_is(const char *t, const char *s)
{
    while (*s)
        if (lower(*t++) != *s++)
            return 0;
    return *t == 0 || *t == ' ' || *t == '/' || *t == '\t' || *t == '\n' || *t == '\r';
}

/* value of attribute `name` in the tag buffer, NUL-terminated in place */
static char *tag_attr(char *t, const char *name)
{
    uint8_t nl = (uint8_t)strlen(name);
    while (*t) {
        char *a = t;
        uint8_t i;
        for (i = 0; i < nl && lower(a[i]) == name[i]; i++)
            ;
        if (i == nl && (a == tag || a[-1] == ' ' || a[-1] == '\t' || a[-1] == '\n')) {
            char *v = a + nl, q, *e;
            while (*v == ' ')
                v++;
            if (*v != '=') {
                t++;
                continue;
            }
            v++;
            while (*v == ' ')
                v++;
            q = (*v == '"' || *v == '\'') ? *v++ : 0;
            for (e = v; *e && (q ? *e != q : (*e != ' ' && *e != '>')); e++)
                ;
            *e = 0;
            return v;
        }
        t++;
    }
    return NULL;
}

static void handle_tag(void)
{
    char *t = tag;
    static const char block_tags_bk[] =
        "p\0div\0h1\0h2\0h3\0h4\0h5\0h6\0ul\0ol\0table\0tr\0section\0article\0"
        "header\0footer\0nav\0main\0form\0blockquote\0pre\0dl\0dt\0dd\0figure\0"
        "aside\0center\0hr\0";
    const char *b;
    char name[12];
    uint8_t closing = 0;
    tag[tag_n] = 0;
    if (*t == '/') {
        closing = 1;
        t++;
    }
    if (tag_is(t, "script") || tag_is(t, "style") || tag_is(t, "svg") ||
        tag_is(t, "template")) {
        if (!closing && t[strlen(t) - 1] != '/') {
            skip_depth_tag = tag_is(t, "script") ? 's' : tag_is(t, "style") ? 'y' :
                             tag_is(t, "svg") ? 'v' : 't';
            tstate = T_SKIP;
        }
        return;
    }
    if (tag_is(t, "title")) {
        in_title = !closing;
        if (!closing)
            title[0] = 0;
        return;
    }
    if (tag_is(t, "br")) {
        flush_word();
        pending_space = 0;
        emit_line();
        return;
    }
    if (tag_is(t, "li")) {
        out_break(0);
        if (!closing)
            out_str("* ");
        flush_word();
        return;
    }
    if (tag_is(t, "a") && !closing) {
        char *h = tag_attr(t, "href");
        if (h && *h && *h != '#' && memcmp(h, "javascript:", 11))
            add_link(h, (uint8_t)(strlen(h) > URL_MAX ? URL_MAX : strlen(h)));
        return;
    }
    if (tag_is(t, "img") && !closing) {
        char *a = tag_attr(t, "alt");
        if (a && *a) {
            out_char('[');
            out_str(a);
            out_char(']');
        }
        return;
    }
    if (tag_is(t, "td") || tag_is(t, "th")) {
        if (closing)
            out_char(' ');
        return;
    }
    if (tag_is(t, "pre"))
        in_pre = !closing;
    for (b = block_tags_bk; *b; b += strlen(b) + 1) {
        strcpy(name, b);                /* the table is in this function's flash page */
        if (tag_is(t, name)) {
            uint8_t heading = t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
            out_break(heading || tag_is(t, "p") || tag_is(t, "hr"));
            if (tag_is(t, "hr"))
                out_str("------------------------");
            return;
        }
    }
}

static void text_char(char c)
{
    if (in_title) {
        uint8_t n = (uint8_t)strlen(title);
        if (c == '\n' || c == '\t' || c == '\r')
            c = ' ';
        if (n < COLS && !(c == ' ' && (!n || title[n - 1] == ' '))) {
            title[n] = c;
            title[n + 1] = 0;
        }
        return;
    }
    if (in_pre && c == '\n') {
        flush_word();
        emit_line();
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
        out_char(' ');
    else
        out_char(c);
}

static void entity(void)
{
    /* name, replacement pairs */
    static const char names_bk[] =
        "amp\0&\0lt\0<\0gt\0>\0quot\0\"\0apos\0'\0nbsp\0 \0copy\0(c)\0"
        "mdash\0-\0ndash\0-\0hellip\0...\0laquo\0<<\0raquo\0>>\0middot\0.\0"
        "rsquo\0'\0lsquo\0'\0ldquo\0\"\0rdquo\0\"\0times\0x\0";
    const char *p;
    char rep[4];
    uint8_t i;
    ent[ent_n] = 0;
    if (ent[0] == '#') {
        uint16_t v = 0;
        const char *q = ent + 1;
        if (*q == 'x' || *q == 'X') {
            for (q++; *q; q++)
                v = v * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10);
        } else {
            for (; *q; q++)
                v = v * 10 + *q - '0';
        }
        text_char(v >= 32 && v < 127 ? (char)v : v == 160 ? ' ' : '?');
        return;
    }
    for (p = names_bk; *p; p += strlen(p) + 1) {
        uint8_t hit = !strcmp(ent, p);
        p += strlen(p) + 1;
        if (hit) {
            strcpy(rep, p);
            for (i = 0; rep[i]; i++)
                text_char(rep[i]);
            return;
        }
    }
    text_char('&');
    for (i = 0; i < ent_n; i++)
        text_char(ent[i]);
}

static void html_char(char c)
{
    switch (tstate) {
    case T_TEXT:
        if (c == '<') {
            tstate = T_TAG;
            tag_n = 0;
        } else if (c == '&') {
            tstate = T_ENTITY;
            ent_n = 0;
        } else {
            text_char(c);
        }
        break;
    case T_ENTITY:
        if (c == ';' || ent_n == sizeof(ent) - 1 || c == ' ' || c == '<') {
            entity();
            tstate = T_TEXT;
            if (c != ';')
                html_char(c);
        } else {
            ent[ent_n++] = c;
        }
        break;
    case T_TAG:
        if (tag_n == 3 && !memcmp(tag, "!--", 3)) {
            tstate = T_COMMENT;
            tag_n = 0;
        }
        if (tstate == T_TAG) {
            if (c == '>') {
                tstate = T_TEXT;
                handle_tag();
            } else if (tag_n < sizeof(tag) - 1) {
                tag[tag_n++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
            }
            break;
        }
        /* fall through */
    case T_COMMENT:
        /* tag[0..1] remember the last two characters */
        if (c == '>' && tag_n >= 2 && tag[0] == '-' && tag[1] == '-') {
            tstate = T_TEXT;
            break;
        }
        tag[0] = tag[1];
        tag[1] = c;
        if (tag_n < 2)
            tag_n++;
        break;
    case T_SKIP:
        /* wait for "</" + the tag name's first letters */
        if (c == '<') {
            tag_n = 1;
        } else if (tag_n == 1) {
            tag_n = c == '/' ? 2 : 0;
        } else if (tag_n == 2) {
            char want = skip_depth_tag == 'y' ? 's' : skip_depth_tag;
            tag_n = lower(c) == want ? 3 : 0;
        } else if (tag_n == 3) {
            /* "sc"ript / "st"yle / "sv"g / "te"mplate */
            char want = skip_depth_tag == 's' ? 'c' : skip_depth_tag == 'y' ? 't' :
                        skip_depth_tag == 'v' ? 'v' : 'e';
            tag_n = lower(c) == want ? 4 : 0;
        } else if (tag_n == 4 && c == '>') {
            tstate = T_TEXT;
            tag_n = 0;
        }
        break;
    }
}

static void body_char(uint8_t c)
{
    /* UTF-8: one '?' per multi-byte character, a few common ones mapped */
    if (utf8_skip) {
        utf8_skip--;
        return;
    }
    if (c >= 0x80) {
        utf8_skip = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c = c >= 0xC0 ? '?' : 0;
        if (!c)
            return;
    }
    if (is_html)
        html_char((char)c);
    else if (c == '\n') {
        flush_word();
        emit_line();
    } else if (c != '\r') {
        out_char(c == '\t' ? ' ' : (char)c);
    }
}

/* ------------------------------------------------------------------ */
/* URLs and HTTP                                                       */

/* split url into host, port, path; returns 1 for https, 0 for http, -1 bad */
static int8_t parse_url(const char *u, char *h, uint16_t *port, const char **path)
{
    int8_t tls = 0;
    uint8_t n = 0;
    if (!memcmp(u, "https://", 8)) {
        tls = 1;
        u += 8;
    } else if (!memcmp(u, "http://", 7)) {
        u += 7;
    } else {
        return -1;
    }
    *port = tls ? 443 : 80;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && n < 63)
        h[n++] = *u++;
    h[n] = 0;
    if (*u == ':') {
        *port = 0;
        for (u++; *u >= '0' && *u <= '9'; u++)
            *port = *port * 10 + *u - '0';
    }
    *path = *u ? u : "/";
    return n ? tls : -1;
}

/* resolve href against the current page url into out */
static void resolve(const char *href, char *out)
{
    const char *p;
    uint8_t n;
    if (!memcmp(href, "http://", 7) || !memcmp(href, "https://", 8)) {
        strncpy(out, href, URL_MAX);
    } else if (href[0] == '/' && href[1] == '/') {
        p = strchr(url, ':');
        n = p ? (uint8_t)(p - url + 1) : 0;
        memcpy(out, url, n);
        strncpy(out + n, href, URL_MAX - n);
    } else {
        /* scheme://host part of the current url */
        p = strstr(url, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        n = p ? (uint8_t)(p - url) : (uint8_t)strlen(url);
        if (href[0] == '/') {
            memcpy(out, url, n);
            strncpy(out + n, href, URL_MAX - n);
        } else {
            /* relative to the current directory */
            const char *slash = NULL, *q;
            for (q = url + n; *q && *q != '?' && *q != '#'; q++)
                if (*q == '/')
                    slash = q;
            n = slash ? (uint8_t)(slash - url + 1) : n;
            memcpy(out, url, n);
            if (!slash)
                out[n++] = '/';
            strncpy(out + n, href, URL_MAX - n);
        }
    }
    out[URL_MAX] = 0;
    /* drop the fragment */
    if ((p = strchr(out, '#')) != NULL)
        *(char *)p = 0;
}

static void reset_page(void)
{
    nlines = top = nlinks = link_used = 0;
    col = word_n = 0;
    pending_space = 0;
    blank_lines = 1;
    in_title = in_pre = 0;
    tstate = T_TEXT;
    utf8_skip = 0;
    title[0] = 0;
    rx_bytes = 0;
}

static void stop(void)
{
    if (sock) {
        lwip_socket_destroy(sock);
        sock = NULL;
    }
    state = B_IDLE;
}

static void sock_event(struct lwip_socket *s, lwip_socket_event_type_t type,
                       const void *data, void *arg)
{
    (void)arg;
    if (type == LWIP_SOCKET_EV_STATE_CHANGE) {
        const lwip_socket_state_data_t *st = data;
        if (st->current == LWIP_STATUS_CONNECTED)
            lwip_socket_write(s, (const uint8_t *)request, strlen(request));
        else if (st->current == LWIP_STATUS_CONNECTING)
            strcpy(msg, "connecting");
        else if (st->current == LWIP_STATUS_RESOLVING)
            strcpy(msg, "resolving");
    } else if (type == LWIP_SOCKET_EV_ERROR) {
        const lwip_socket_error_data_t *e = data;
        snprintf(msg, sizeof(msg), "error %u/%u %s", e->component, e->err,
                 e->component == LWIP_SOCKET_ERR_COMP_TLS ? tls84_last_error : "");
    }
}

static void load(const char *u)
{
    const char *path;
    uint16_t port;
    int8_t tls;
    stop();
    if (u != url)
        strncpy(url, u, URL_MAX);
    if (!strstr(url, "://")) {
        memmove(url + 7, url, URL_MAX - 7);
        memcpy(url, "http://", 7);
    }
    url[URL_MAX] = 0;
    tls = parse_url(url, host, &port, &path);
    reset_page();
    if (tls < 0) {
        strcpy(msg, "bad URL");
        return;
    }
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+/1.0\r\n"
             "Accept: text/html, text/plain\r\nAccept-Encoding: identity\r\n\r\n",
             path, host);
    sock = lwip_socket_create_ex(tls ? LWIP_SOCKET_ALTCP_TLS : LWIP_SOCKET_TCP,
                                 LWIP_NETIF_EXT, NULL, tls ? 120000u : 30000u, 2048u);
    if (!sock) {
        strcpy(msg, "no memory");
        return;
    }
    lwip_socket_on_event(sock, LWIP_SOCKET_EVENTF_ALL, sock_event, NULL);
    hstate = H_STATUS;
    hline_n = 0;
    chunked = 0;
    is_html = 1;
    http_status = 0;
    location[0] = 0;
    if (lwip_socket_connect(sock, host, port) != LWIP_OK) {
        stop();
        strcpy(msg, "connect failed");
        return;
    }
    strcpy(msg, tls ? "TLS handshake..." : "connecting");
    state = B_LOADING;
    t_start = k_now_ms();
}

static void push_history(void)
{
    if (!url[0])
        return;
    if (hist_n == HIST_MAX) {
        uint8_t i;
        char tmp[URL_MAX + 1];
        for (i = 1; i < HIST_MAX; i++) {
            xram_read(X_HIST + i * (URL_MAX + 1), tmp, URL_MAX + 1);
            xram_write(X_HIST + (i - 1) * (URL_MAX + 1), tmp, URL_MAX + 1);
        }
        hist_n--;
    }
    xram_write(X_HIST + hist_n * (URL_MAX + 1), url, URL_MAX + 1);
    hist_n++;
}

static void header_line(void)
{
    char *v;
    hline[hline_n] = 0;
    if (hstate == H_STATUS) {
        v = strchr(hline, ' ');
        /* hundreds and tens of the status code: 30 for 30x */
        http_status = v ? (uint8_t)((v[1] - '0') * 10 + (v[2] - '0')) : 0;
        hstate = H_HEADER;
        return;
    }
    if (!hline_n) {                     /* end of headers */
        if (http_status == 30 && location[0] && redirects < 5) {
            char tmp[URL_MAX + 1];
            redirects++;
            resolve(location, tmp);
            strcpy(location, tmp);
            hstate = H_DONE;
            do_redirect = 1;            /* load it once the socket is gone */
            return;
        }
        hstate = chunked ? H_CHUNK_SIZE : H_BODY;
        chunk_left = 0;
        return;
    }
    for (v = hline; *v && *v != ':'; v++)
        *v = lower(*v);
    if (!*v)
        return;
    *v++ = 0;
    while (*v == ' ')
        v++;
    if (!strcmp(hline, "location"))
        strncpy(location, v, URL_MAX);
    else if (!strcmp(hline, "transfer-encoding") && strstr(v, "chunked"))
        chunked = 1;
    else if (!strcmp(hline, "content-type"))
        is_html = strstr(v, "html") != NULL || strstr(v, "xml") != NULL;
}

static void http_byte(uint8_t c)
{
    switch (hstate) {
    case H_STATUS:
    case H_HEADER:
    case H_CHUNK_SIZE:
    case H_CHUNK_END:
        if (c == '\n') {
            if (hstate == H_CHUNK_SIZE) {
                uint8_t i;
                chunk_left = 0;
                for (i = 0; i < hline_n; i++) {
                    char h = lower(hline[i]);
                    if (h >= '0' && h <= '9')
                        chunk_left = chunk_left * 16 + h - '0';
                    else if (h >= 'a' && h <= 'f')
                        chunk_left = chunk_left * 16 + h - 'a' + 10;
                    else
                        break;
                }
                hstate = chunk_left ? H_CHUNK_DATA : H_DONE;
            } else if (hstate == H_CHUNK_END) {
                hstate = H_CHUNK_SIZE;
            } else {
                header_line();
            }
            hline_n = 0;
        } else if (c != '\r' && hline_n < sizeof(hline) - 1) {
            hline[hline_n++] = (char)c;
        }
        break;
    case H_BODY:
        body_char(c);
        break;
    case H_CHUNK_DATA:
        body_char(c);
        if (!--chunk_left)
            hstate = H_CHUNK_END;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* screen                                                              */

static void draw(void)
{
    char buf[COLS + 1];
    uint8_t r;
    ui_clear();
    if (editing) {
        snprintf(buf, sizeof(buf), "%c %s", in_mode_char(), editing == 1 ? "URL" : "link");
    } else if (state == B_LOADING && !rx_bytes && tls84_status(buf, sizeof(buf))) {
        /* key exchange progress */
    } else if (state == B_LOADING) {
        snprintf(buf, sizeof(buf), "%s %luB", msg, rx_bytes);
    } else if (msg[0]) {
        strcpy(buf, msg);
    } else {
        snprintf(buf, sizeof(buf), "%s", title[0] ? title : host[0] ? host : "lwIP-84+ web");
    }
    ui_status(buf);
    for (r = 0; r < ROWS && top + r < nlines; r++) {
        xram_read(X_TEXT + (uint32_t)(top + r) * COLS, buf, COLS);
        buf[COLS] = 0;
        ui_text(r, buf);
    }
    if (!nlines && !editing && state == B_IDLE) {
        ui_text(1, "Y=    open a URL");
        ui_text(2, "0-9   follow link [n]");
        ui_text(3, "DEL   back  ENTER reload");
        ui_text(4, "UP/DN LEFT/RIGHT scroll");
        ui_text(5, "CLEAR stop  MODE quit");
        ui_text(6, "ALPHA: 1 > A > a  2nd:");
        ui_text(7, "symbols (see README)");
    }
    if (editing)
        le_draw(&le, ROWS - 1, editing == 1 ? ">" : "link ");
    lcd_update();
}

/* returns 0 if there is not enough memory */
uint8_t browser_enter(void)
{
    static uint8_t saved;
    if (!bm) {
        bm = calloc(1, sizeof(*bm));
        if (!bm)
            return 0;
        if (saved)
            xram_read(X_STATE, bm, SAVED_SIZE);
        saved = 1;
    }
    if (!url[0] && !editing) {
        editing = 1;
        strcpy(edit_buf, "http://");
        le_init(&le, edit_buf, URL_MAX, 0);
        in_mode = IN_LOWER;
    }
    draw();
    return 1;
}

static void browser_leave(void)
{
    stop();
    editing = 0;
    xram_write(X_STATE, bm, SAVED_SIZE);
    free(bm);
    bm = NULL;
}

/* returns 0 when the browser wants to quit */
uint8_t browser_key(uint8_t k)
{
    if (k == K_NONE || !bm)
        return 1;
    if (editing) {
        int8_t r = le_key(&le, k);
        if (r == LE_DONE) {
            uint8_t was = editing;
            editing = 0;
            if (was == 1) {
                push_history();
                redirects = 0;
                load(edit_buf);
            } else {
                uint16_t n = (uint16_t)atoi(edit_buf);
                if (n >= 1 && n <= nlinks) {
                    char href[URL_MAX + 1], abs[URL_MAX + 1];
                    get_link(n, href);
                    resolve(href, abs);
                    push_history();
                    redirects = 0;
                    load(abs);
                } else {
                    strcpy(msg, "no such link");
                }
            }
        } else if (r == LE_CANCEL) {
            editing = 0;
        }
        draw();
        return 1;
    }
    msg[0] = state == B_LOADING ? msg[0] : 0;
    switch (k) {
    case K_MODE:
        browser_leave();
        return 0;
    case K_YEQU:
        editing = 1;
        strcpy(edit_buf, url[0] ? url : "http://");
        le_init(&le, edit_buf, URL_MAX, 0);
        in_mode = IN_LOWER;
        break;
    case K_UP:
        if (top)
            top--;
        break;
    case K_DOWN:
        if (top + ROWS < nlines)
            top++;
        break;
    case K_LEFT:
        top = top > ROWS ? top - ROWS : 0;
        break;
    case K_RIGHT:
        if (nlines > ROWS)
            top = top + ROWS < nlines - ROWS ? top + ROWS : nlines - ROWS;
        break;
    case K_DEL:
        if (hist_n) {
            hist_n--;
            xram_read(X_HIST + hist_n * (URL_MAX + 1), url, URL_MAX + 1);
            redirects = 0;
            load(url);
        }
        break;
    case K_ENTER:
        if (url[0]) {
            redirects = 0;
            load(url);
        }
        break;
    case K_CLEAR:
        if (state == B_LOADING) {
            stop();
            strcpy(msg, "stopped");
        }
        break;
    default: {
        int16_t c;
        in_mode = IN_NUM;
        c = in_char(k);
        if (c >= '0' && c <= '9' && nlinks) {
            editing = 2;
            edit_buf[0] = (char)c;
            edit_buf[1] = 0;
            le_init(&le, edit_buf, 4, 0);
        }
    }
    }
    draw();
    return 1;
}

void browser_poll(void)
{
    uint8_t buf[64];
    size_t n, i;
    if (!bm || state != B_LOADING || !sock)
        return;
    while ((n = lwip_socket_available(sock)) != 0) {
        n = lwip_socket_read(sock, buf, n > sizeof(buf) ? sizeof(buf) : n);
        if (!n)
            break;
        if (!rx_bytes)
            strcpy(msg, "loading");
        rx_bytes += n;
        for (i = 0; i < n && hstate != H_DONE; i++)
            http_byte(buf[i]);
        if (hstate == H_DONE)
            break;
    }
    if (nlines >= MAX_LINES && hstate != H_DONE) {
        stop();                         /* the text buffer is full */
        strcpy(msg, "page truncated");
        draw();
        return;
    } else if (hstate == H_DONE || !lwip_socket_is_active(sock)) {
        stop();
        if (do_redirect) {
            do_redirect = 0;
            load(location);
            draw();
            return;
        }
        out_break(0);
        if (hstate == H_STATUS)
            strcpy(msg, msg[0] && memcmp(msg, "error", 5) == 0 ? msg : "no response");
        else
            msg[0] = 0;
        dbg_printf("web: %lu bytes, %u lines, %lu ms\n", rx_bytes, nlines, k_now_ms() - t_start);
        if (!editing)
            draw();                     /* show the result now */
        return;
    }
    if (!editing && (uint32_t)(k_now_ms() - last_draw) > 1000) {
        last_draw = k_now_ms();
        draw();
    }
}
