/* browser.c - a text web browser (HTTP/1.0 and HTTPS through lib/net/sock84.c).
 *
 * The page is rendered by app/browser/html.c while it arrives.  Links and form
 * fields carry inverted numbers; typing a number and ENTER follows the
 * link or uses the field (edit text, toggle a check box, choose from a
 * list, submit).  Forms are sent with GET or POST.  Text typed into the
 * URL line that is not an address is searched with DuckDuckGo Lite, which
 * works without JavaScript.
 *
 * Keys:  Y=  URL or search words     0-9 ENTER  use item n
 *        UP/DOWN  scroll   LEFT/RIGHT  page up/down   GRAPH  top/content
 *        DEL  back       ENTER  reload    CLEAR  stop
 *        MODE  quit      ALPHA/2nd  letters and symbols while typing
 */
#include <string.h>
#include <stdlib.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"
#include "html.h"
#include "sock84.h"
#include "tls84.h"

#define URL_MAX HTML_STR_MAX
#define COLS CON_COLS
#define ROWS 9
#define BODY_MAX 400
/* history: URL and POST body of each page (a search result page can only
 * be fetched again with its body) */
#define X_HIST XRAM_HIST
#define HIST_ENTRY (URL_MAX + 1 + BODY_MAX + 1)
#define HIST_MAX (XRAM_HIST_SIZE / HIST_ENTRY)
#define X_STATE XRAM_BROWSER_STATE         /* url and host while not running */

#define SEARCH_URL "https://lite.duckduckgo.com/lite/"
#define SEARCH_REF "https://lite.duckduckgo.com/"

enum { B_IDLE, B_LOADING };
enum { E_NONE, E_URL, E_GO, E_FIELD, E_PICK };
enum { H_STATUS, H_HEADER, H_BODY, H_CHUNK_SIZE, H_CHUNK_DATA, H_CHUNK_END, H_DONE };

static struct sock *sock;
static uint8_t state, editing, redirects, hist_n, scrolled, page_open;
static uint16_t top, pick_cur, pick_top, cur_idx;
static uint32_t rx_bytes, last_draw, t_start;

/* HTTP */
static uint8_t hstate, chunked, is_html, http_status, do_redirect, hdr_charset;
static uint32_t chunk_left;
static uint8_t hline_n;

/* the buffers exist only while the browser runs (browser and SSH client
 * never run at the same time, so they share the heap); url and host are
 * kept in extended RAM in between */
struct browser_mem {
    char url[URL_MAX + 1], host[64];    /* saved: keep first */
    char edit_buf[URL_MAX + 1];
    char referer[URL_MAX + 1];
    char body[BODY_MAX + 1];            /* POST body of the page, "" for GET */
    char location[URL_MAX + 1];
    char request[1400];
    struct line_edit le;
    struct html_item it;
    char msg[COLS + 1];
    char prompt[COLS + 1];
    char hline[128];
    uint8_t rx[512];
};
#define SAVED_SIZE (URL_MAX + 1 + 64)
static struct browser_mem *bm;
#define url (bm->url)
#define host (bm->host)
#define edit_buf (bm->edit_buf)
#define request (bm->request)
#define le (bm->le)
#define msg (bm->msg)
#define hline (bm->hline)
#define location (bm->location)

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* ------------------------------------------------------------------ */
/* URLs                                                                */

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
        } else if (href[0] == '?') {
            /* same path, new query */
            const char *q;
            for (q = url + n; *q && *q != '?' && *q != '#'; q++)
                ;
            n = (uint8_t)(q - url);
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

static const char hexd[] = "0123456789ABCDEF";

/* append s to out (size max), percent-encoded; form: as a form value */
static uint16_t encode(char *out, uint16_t n, uint16_t max, const char *s, uint8_t form)
{
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        uint8_t plain = form ? ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~' || c == '*')
                             : (c > ' ' && c < 0x7F && c != '"' && c != '<' && c != '>');
        if (plain || (form && c == ' ')) {
            if (n + 1 >= max)
                break;
            out[n++] = c == ' ' ? '+' : (char)c;
        } else {
            if (n + 3 >= max)
                break;
            out[n++] = '%';
            out[n++] = hexd[c >> 4];
            out[n++] = hexd[c & 15];
        }
    }
    out[n] = 0;
    return n;
}

/* does the URL line hold search words rather than an address? */
static uint8_t is_search(const char *s)
{
    return !strstr(s, "://") && (strchr(s, ' ') || !strchr(s, '.'));
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */

static void stop(void)
{
    if (sock) {
        sock_free(sock);
        sock = NULL;
    }
    state = B_IDLE;
    if (page_open) {
        html_end();
        page_open = 0;
    }
}

static void sock_event(struct sock *s, uint8_t ev, uint8_t a, int8_t err)
{
    if (ev == SOCK_EV_STATE) {
        if (a == SOCK_CONNECTED)
            sock_write(s, request, (uint16_t)strlen(request));
        else if (a == SOCK_CONNECTING)
            strcpy(msg, "connecting");
        else if (a == SOCK_RESOLVING)
            strcpy(msg, "resolving");
        else if (a == SOCK_WAITING_NET)
            strcpy(msg, "waiting for network");
    } else {
        snprintf(msg, sizeof(msg), "error %u/%d %s", a, err,
                 a == SOCK_ERR_TLS ? tls84_last_error : "");
    }
}

/* load u (GET, or POST with body); ref: Referer or NULL */
static void load(const char *u, const char *body, const char *ref)
{
    const char *path;
    uint16_t port, n;
    int8_t tls;
    stop();
    if (body != bm->body) {
        strncpy(bm->body, body ? body : "", BODY_MAX);
        bm->body[BODY_MAX] = 0;
    }
    body = bm->body[0] ? bm->body : NULL;
    if (ref && ref != bm->referer)
        strncpy(bm->referer, ref, URL_MAX);
    if (!ref)
        bm->referer[0] = 0;
    bm->referer[URL_MAX] = 0;
    if (u != url)
        strncpy(url, u, URL_MAX);
    if (!strstr(url, "://")) {
        memmove(url + 7, url, URL_MAX - 7);
        memcpy(url, "http://", 7);
    }
    url[URL_MAX] = 0;
    tls = parse_url(url, host, &port, &path);
    if (tls < 0) {
        strcpy(msg, "bad URL");
        return;
    }
    n = (uint16_t)snprintf(request, sizeof(request), "%s ", body ? "POST" : "GET");
    n = encode(request, n, 600, path, 0);
    n += (uint16_t)snprintf(request + n, sizeof(request) - n,
                            " HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+/1.0\r\n"
                            "Accept: text/html, text/plain\r\nAccept-Encoding: identity\r\n",
                            host);
    /* also when it is the same URL: forms that post to their own page
     * (DuckDuckGo Lite) are answered only with a Referer */
    if (bm->referer[0])
        n += (uint16_t)snprintf(request + n, sizeof(request) - n, "Referer: %s\r\n", bm->referer);
    if (body)
        n += (uint16_t)snprintf(request + n, sizeof(request) - n,
                                "Content-Type: application/x-www-form-urlencoded\r\n"
                                "Content-Length: %u\r\n\r\n%s", (uint16_t)strlen(body), body);
    else
        snprintf(request + n, sizeof(request) - n, "\r\n");
    sock = sock_new(tls ? SOCK_TLS : SOCK_TCP, tls ? 120000u : 30000u, sock_event);
    if (!sock) {
        strcpy(msg, "no memory");
        return;
    }
    hstate = H_STATUS;
    hline_n = 0;
    chunked = 0;
    is_html = 1;
    hdr_charset = HC_NONE;
    http_status = 0;
    location[0] = 0;
    rx_bytes = 0;
    if (sock_connect(sock, host, port)) {
        stop();
        strcpy(msg, "connect failed");
        return;
    }
    strcpy(msg, tls ? "TLS handshake..." : "connecting");
    state = B_LOADING;
    t_start = k_now_ms();
}

static void search(const char *words)
{
    uint16_t n;
    char *body = malloc(BODY_MAX + 1);
    if (!body) {
        strcpy(msg, "no memory");
        return;
    }
    memcpy(body, "q=", 3);
    n = encode(body, 2, BODY_MAX - 12, words, 1);
    strcpy(body + n, "&kl=&df=");
    load(SEARCH_URL, body, SEARCH_REF);
    free(body);
}

static void push_history(void)
{
    uint8_t i;
    if (!url[0])
        return;
    if (hist_n == HIST_MAX) {
        /* drop the oldest; location and edit_buf are free here */
        for (i = 1; i < HIST_MAX; i++) {
            uint32_t from = X_HIST + (uint32_t)i * HIST_ENTRY, to = from - HIST_ENTRY;
            xram_read(from, location, URL_MAX + 1);
            xram_write(to, location, URL_MAX + 1);
            xram_read(from + URL_MAX + 1, edit_buf, URL_MAX + 1);
            xram_write(to + URL_MAX + 1, edit_buf, URL_MAX + 1);
            xram_read(from + 2 * (URL_MAX + 1), edit_buf, BODY_MAX + 1 - (URL_MAX + 1));
            xram_write(to + 2 * (URL_MAX + 1), edit_buf, BODY_MAX + 1 - (URL_MAX + 1));
        }
        hist_n--;
    }
    xram_write(X_HIST + (uint32_t)hist_n * HIST_ENTRY, url, URL_MAX + 1);
    xram_write(X_HIST + (uint32_t)hist_n * HIST_ENTRY + URL_MAX + 1, bm->body, BODY_MAX + 1);
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
            char *tmp = edit_buf;
            redirects++;
            resolve(location, tmp);
            strcpy(location, tmp);
            hstate = H_DONE;
            do_redirect = 1;            /* load it once the socket is gone */
            return;
        }
        hstate = chunked ? H_CHUNK_SIZE : H_BODY;
        chunk_left = 0;
        html_begin(is_html);
        if (hdr_charset != HC_NONE)
            html_charset = hdr_charset;
        page_open = 1;
        top = 0;
        scrolled = 0;
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
    else if (!strcmp(hline, "content-type")) {
        is_html = strstr(v, "html") != NULL || strstr(v, "xml") != NULL;
        hdr_charset = html_charset_of(v);
    }
}

static void http_feed(const uint8_t *p, uint16_t n)
{
    uint16_t k;
    uint8_t c;
    while (n) {
        switch (hstate) {
        case H_BODY:
            html_feed(p, n);
            return;
        case H_CHUNK_DATA:
            k = n < chunk_left ? n : (uint16_t)chunk_left;
            html_feed(p, k);
            p += k;
            n -= k;
            if (!(chunk_left -= k))
                hstate = H_CHUNK_END;
            continue;
        case H_DONE:
            return;
        }
        c = *p++;
        n--;
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
    }
}

/* ------------------------------------------------------------------ */
/* forms                                                               */

static uint16_t add_pair(char *out, uint16_t n, const char *name, const char *value)
{
    if (!name[0] || n + 4 >= BODY_MAX)
        return n;
    if (n)
        out[n++] = '&';
    n = encode(out, n, BODY_MAX, name, 1);
    if (n + 2 >= BODY_MAX)
        return n;
    out[n++] = '=';
    return encode(out, n, BODY_MAX, value, 1);
}

/* send form f; sub: index of the submit button used or HTML_NONE */
static void submit(uint8_t f, uint16_t sub)
{
    struct html_form fm;
    struct html_item *it = &bm->it;
    char *data, *abs, *name = edit_buf, *value = location;
    uint16_t i, n = 0;
    if (f == HTML_NOFORM)
        return;
    data = malloc(BODY_MAX + 1 + URL_MAX + 1);
    if (!data) {
        strcpy(msg, "no memory");
        return;
    }
    abs = data + BODY_MAX + 1;
    data[0] = 0;
    for (i = 0; i < html_items; i++) {
        html_get_item(i, it);
        if (it->form != f || it->kind == IT_LINK)
            continue;
        if ((it->kind == IT_CHECK || it->kind == IT_RADIO) && !(it->flags & ITF_CHECKED))
            continue;
        if (it->kind == IT_SUBMIT && i != sub)
            continue;
        html_get_str(it->name, name, URL_MAX + 1);
        if (it->kind == IT_SELECT) {
            if (!it->nopt)
                continue;
            html_get_option(it, it->sel, value, NULL);
        } else {
            html_get_str(it->value, value, URL_MAX + 1);
        }
        n = add_pair(data, n, name, value);
    }
    html_get_form(f, &fm);
    if (fm.action == HTML_NONE)
        strcpy(abs, url);
    else {
        html_get_str(fm.action, name, URL_MAX + 1);
        resolve(name, abs);
    }
    push_history();
    redirects = 0;
    if (fm.post) {
        load(abs, data, url);
    } else {
        char *q = strchr(abs, '?');
        if (q)
            *q = 0;
        if (strlen(abs) + 1 + n > URL_MAX) {
            strcpy(msg, "form too long");
        } else {
            strcat(abs, "?");
            strcat(abs, data);
            load(abs, NULL, url);
        }
    }
    free(data);
}

/* use item number n */
static void activate(uint16_t n)
{
    struct html_item *it = &bm->it;
    uint16_t idx = html_find_number(n, it);
    if (idx == HTML_NONE) {
        strcpy(msg, "no such item");
        return;
    }
    cur_idx = idx;
    switch (it->kind) {
    case IT_LINK: {
        char *abs = malloc(URL_MAX + 1);
        if (!abs)
            return;
        html_get_str(it->name, location, URL_MAX + 1);
        resolve(location, abs);
        push_history();
        redirects = 0;
        load(abs, NULL, url);
        free(abs);
        break;
    }
    case IT_TEXT:
    case IT_PASSWORD:
    case IT_TEXTAREA: {
        uint8_t i;
        html_get_str(it->value, edit_buf, URL_MAX + 1);
        le_init(&le, edit_buf, URL_MAX, it->kind == IT_PASSWORD);
        html_get_str(it->name, bm->prompt, 8);
        if (!bm->prompt[0])
            strcpy(bm->prompt, "text");
        i = (uint8_t)strlen(bm->prompt);
        bm->prompt[i] = ':';
        bm->prompt[i + 1] = 0;
        in_mode = IN_LOWER;
        editing = E_FIELD;
        break;
    }
    case IT_CHECK:
        it->flags ^= ITF_CHECKED;
        html_put_item(idx, it);
        html_show_field(it);
        break;
    case IT_RADIO: {
        struct html_item other;
        uint16_t i;
        char *name = edit_buf, *oname = location;
        html_get_str(it->name, name, URL_MAX + 1);
        for (i = 0; i < html_items; i++) {
            html_get_item(i, &other);
            if (i == idx || other.kind != IT_RADIO || other.form != it->form ||
                !(other.flags & ITF_CHECKED))
                continue;
            html_get_str(other.name, oname, URL_MAX + 1);
            if (!strcmp(name, oname)) {
                other.flags &= ~ITF_CHECKED;
                html_put_item(i, &other);
                if (other.flags & ITF_NUMBERED)
                    html_show_field(&other);
            }
        }
        it->flags |= ITF_CHECKED;
        html_put_item(idx, it);
        html_show_field(it);
        break;
    }
    case IT_SELECT:
        pick_cur = it->sel;
        pick_top = pick_cur >= ROWS ? pick_cur - ROWS + 1 : 0;
        editing = E_PICK;
        break;
    case IT_SUBMIT:
        submit(it->form, idx);
        break;
    }
}

/* a text field was edited */
static void field_done(void)
{
    struct html_item *it = &bm->it;
    struct html_form fm;
    uint16_t off = html_add_str(edit_buf);
    if (off == HTML_NONE) {
        strcpy(msg, "page memory full");
        return;
    }
    html_get_item(cur_idx, it);
    it->value = off;
    html_put_item(cur_idx, it);
    html_show_field(it);
    /* ENTER in the only text field of a form sends it */
    if (it->form != HTML_NOFORM && it->kind != IT_TEXTAREA) {
        html_get_form(it->form, &fm);
        if (fm.n_text == 1)
            submit(it->form, HTML_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* screen                                                              */

static void draw_pick(void)
{
    struct html_item *it = &bm->it;
    char label[COLS + 1];
    uint8_t r, y = 8, h;
    ui_clear();
    ui_status("choose: ENTER  CLEAR");
    for (r = 0; r < ROWS && pick_top + r < it->nopt; r++) {
        html_get_option(it, (uint8_t)(pick_top + r), NULL, label);
        h = ui_row_height(label);
        if (y + h > 64)
            break;
        ui_text_at(y, label, h);
        if (pick_top + r == pick_cur)
            lcd_invert_rect(0, y, 96, h);
        y += h;
    }
    lcd_update();
}

static void draw(void)
{
    char buf[COLS + 1];
    uint8_t r;
    if (editing == E_PICK) {
        draw_pick();
        return;
    }
    ui_clear();
    if (editing) {
        snprintf(buf, sizeof(buf), "%s %s", in_mode_str(),
                 editing == E_URL ? "URL or search" : editing == E_GO ? "item number" : "edit field");
    } else if (state == B_LOADING && !rx_bytes && tls84_status(buf, sizeof(buf))) {
        /* key exchange progress */
    } else if (state == B_LOADING) {
        snprintf(buf, sizeof(buf), "%s %luB", msg, rx_bytes);
    } else if (msg[0]) {
        strcpy(buf, msg);
    } else {
        snprintf(buf, sizeof(buf), "%s", html_title[0] ? html_title : host[0] ? host : "lwIP-84+ web");
    }
    ui_status(buf);
    buf[COLS] = 0;
    {
        /* rows with font glyphs are 8 pixels high, others 6 */
        uint8_t y = 8, h, bottom = editing ? 56 : 64;
        for (r = 0; top + r < html_lines; r++) {
            html_get_line(top + r, buf);
            h = ui_row_height(buf);
            if (y + h > bottom)
                break;
            ui_text_at(y, buf, h);
            y += h;
        }
    }
    if (!html_lines && !editing && state == B_IDLE) {
        ui_text(1, "Y=    URL or search");
        ui_text(2, "0-9 ENTER  use item n");
        ui_text(3, "DEL   back  ENTER reload");
        ui_text(4, "UP/DN LEFT/RIGHT scroll");
        ui_text(5, "CLEAR stop  MODE quit");
        ui_text(6, "ALPHA: 1 > A > a  2nd:");
        ui_text(7, "symbols 2nd+ALPHA: lang");
    }
    if (editing == E_URL)
        le_draw(&le, ROWS - 1, ">");
    else if (editing == E_GO)
        le_draw(&le, ROWS - 1, "go ");
    else if (editing == E_FIELD)
        le_draw(&le, ROWS - 1, bm->prompt);
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
        editing = E_URL;
        edit_buf[0] = 0;
        le_init(&le, edit_buf, URL_MAX, 0);
        in_mode = IN_LOWER;
    }
    draw();
    return 1;
}

static void browser_leave(void)
{
    stop();
    editing = E_NONE;
    xram_write(X_STATE, bm, SAVED_SIZE);
    free(bm);
    bm = NULL;
}

static uint8_t pick_key(uint8_t k)
{
    struct html_item *it = &bm->it;
    switch (k) {
    case K_UP:
        if (pick_cur)
            pick_cur--;
        break;
    case K_DOWN:
        if (pick_cur + 1 < it->nopt)
            pick_cur++;
        break;
    case K_LEFT:
        pick_cur = pick_cur > ROWS ? pick_cur - ROWS : 0;
        break;
    case K_RIGHT:
        pick_cur = pick_cur + ROWS < it->nopt ? pick_cur + ROWS : it->nopt - 1;
        break;
    case K_ENTER:
        it->sel = (uint8_t)pick_cur;
        html_put_item(cur_idx, it);
        html_show_field(it);
        editing = E_NONE;
        return 1;
    case K_CLEAR:
    case K_MODE:
        editing = E_NONE;
        return 1;
    }
    if (pick_cur < pick_top)
        pick_top = pick_cur;
    if (pick_cur >= pick_top + ROWS)
        pick_top = pick_cur - ROWS + 1;
    return 1;
}

/* returns 0 when the browser wants to quit */
uint8_t browser_key(uint8_t k)
{
    if (k == K_NONE || !bm)
        return 1;
    if (editing == E_PICK) {
        pick_key(k);
        draw();
        return 1;
    }
    if (editing) {
        int8_t r = le_key(&le, k);
        if (r == LE_DONE) {
            uint8_t was = editing;
            editing = E_NONE;
            msg[0] = 0;
            if (was == E_URL) {
                if (!edit_buf[0]) {
                    /* nothing */
                } else if (is_search(edit_buf)) {
                    push_history();
                    redirects = 0;
                    search(edit_buf);
                } else {
                    push_history();
                    redirects = 0;
                    load(edit_buf, NULL, NULL);
                }
            } else if (was == E_GO) {
                uint16_t n = (uint16_t)atoi(edit_buf);
                if (n >= 1 && n <= html_numbers)
                    activate(n);
                else
                    strcpy(msg, "no such item");
            } else {
                field_done();
            }
        } else if (r == LE_CANCEL) {
            editing = E_NONE;
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
        editing = E_URL;
        strcpy(edit_buf, url);
        le_init(&le, edit_buf, URL_MAX, 0);
        in_mode = IN_LOWER;
        break;
    case K_UP:
        if (top)
            top--;
        scrolled = 1;
        break;
    case K_DOWN:
        if (top + ROWS < html_lines)
            top++;
        scrolled = 1;
        break;
    case K_LEFT:
        top = top > ROWS ? top - ROWS : 0;
        scrolled = 1;
        break;
    case K_RIGHT:
        if (html_lines > ROWS)
            top = top + ROWS < html_lines - ROWS ? top + ROWS : html_lines - ROWS;
        scrolled = 1;
        break;
    case K_GRAPH:
        /* top of the page <-> start of the main content */
        top = top == 0 && html_main_line != HTML_NONE ? html_main_line : 0;
        scrolled = 1;
        break;
    case K_DEL:
        if (hist_n) {
            hist_n--;
            xram_read(X_HIST + (uint32_t)hist_n * HIST_ENTRY, url, URL_MAX + 1);
            xram_read(X_HIST + (uint32_t)hist_n * HIST_ENTRY + URL_MAX + 1, bm->body, BODY_MAX + 1);
            redirects = 0;
            /* a POST again with its body and the page itself as Referer */
            load(url, bm->body, bm->body[0] ? url : NULL);
        }
        break;
    case K_ENTER:
        if (url[0]) {
            redirects = 0;
            load(url, bm->body, bm->body[0] ? url : NULL);
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
        if (c >= '0' && c <= '9' && html_numbers) {
            editing = E_GO;
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
    uint8_t *buf;
    size_t n;
    if (!bm || state != B_LOADING || !sock)
        return;
    buf = bm->rx;
    while ((n = sock_available(sock)) != 0) {
        n = sock_read(sock, buf, n > sizeof(bm->rx) ? sizeof(bm->rx) : n);
        if (!n)
            break;
        if (!rx_bytes) {
            strcpy(msg, "loading");
            dbg_printf("web: first byte after %lu ms\n", k_now_ms() - t_start);
        }
        rx_bytes += n;
        http_feed(buf, (uint16_t)n);
        if (hstate == H_DONE || (page_open && html_full))
            break;
    }
    if (!scrolled && !top && html_main_line != HTML_NONE && html_main_line < html_lines)
        top = html_main_line;
    /* only while this page's body arrives: html_full is still set from the
     * previous page until html_begin() */
    if (page_open && html_full && hstate != H_DONE) {
        stop();                         /* the text buffer is full */
        strcpy(msg, "page truncated");
        dbg_printf("web: %lu bytes, %u lines (full), %lu ms\n", rx_bytes, html_lines,
                   k_now_ms() - t_start);
        draw();
        return;
    } else if (hstate == H_DONE || !sock_is_active(sock)) {
        uint8_t got = hstate != H_STATUS;
        stop();
        if (do_redirect) {
            do_redirect = 0;
            load(location, NULL, NULL);
            draw();
            return;
        }
        if (!got)
            strcpy(msg, msg[0] && memcmp(msg, "error", 5) == 0 ? msg : "no response");
        else
            msg[0] = 0;
        dbg_printf("web: %lu bytes, %u lines, %u items, %lu ms, heap %u free\n", rx_bytes,
                   html_lines, html_items, k_now_ms() - t_start, heap_free_bytes());
        if (!editing)
            draw();                     /* show the result now */
        return;
    }
    if (!editing && (uint32_t)(k_now_ms() - last_draw) > 1000) {
        last_draw = k_now_ms();
        draw();
    }
}
