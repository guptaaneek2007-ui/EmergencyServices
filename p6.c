#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "mongoose.h"

#define PAGE_MAX 65536

// Get a required environment variable (NULL if missing)
static const char *must_get_env(const char *name) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0') {
        fprintf(stderr, "[DB ERROR] Required environment variable '%s' is missing or empty\n", name);
        fflush(stderr);
        return NULL;
    }
    return val;
}

// Append a plain string to buf (never overflows)
static void append_raw(char *buf, size_t size, size_t *off, const char *s) {
    size_t n = strlen(s);
    if (*off + n + 1 >= size) return;
    memcpy(buf + *off, s, n + 1);
    *off += n;
}

// Append a string to buf with HTML special characters escaped
static void append_html(char *buf, size_t size, size_t *off, const char *s) {
    for (; s && *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep) {
            append_raw(buf, size, off, rep);
        } else {
            if (*off + 2 >= size) return;
            buf[(*off)++] = *s;
            buf[*off] = '\0';
        }
    }
}

static const char *PAGE_HEAD =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>EmergencyServices - Results</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0;font-family:Arial,Helvetica,sans-serif}"
    "body{background:#f4f6f8;color:#222}"
    "header{background:#c62828;color:#fff;padding:20px 8%;font-size:26px;font-weight:bold}"
    ".wrap{width:92%;max-width:760px;margin:30px auto}"
    "h2{color:#b71c1c;margin-bottom:6px}"
    ".sub{color:#666;margin-bottom:20px}"
    ".card{background:#fff;border-radius:12px;padding:18px 22px;margin-bottom:14px;"
    "box-shadow:0 4px 15px rgba(0,0,0,.08)}"
    ".card h3{color:#c62828;margin-bottom:6px}"
    ".card p{color:#555;margin:3px 0;line-height:1.4}"
    ".card a{color:#c62828;font-weight:bold;text-decoration:none}"
    ".back{display:inline-block;margin-top:10px;padding:12px 22px;background:#c62828;"
    "color:#fff;border-radius:8px;text-decoration:none;font-weight:bold}"
    ".none{background:#fff;padding:24px;border-radius:12px;text-align:center;color:#666}"
    "</style></head><body><header>EmergencyServices</header><div class=\"wrap\">";

static const char *PAGE_FOOT =
    "<a class=\"back\" href=\"/\">&larr; New search</a></div></body></html>";

static void send_page(struct mg_connection *c, int code, const char *html) {
    mg_http_reply(c, code, "Content-Type: text/html; charset=utf-8\r\n", "%s", html);
}

// Show an error page
static void reply_error(struct mg_connection *c, int code, const char *title, const char *details) {
    char *page = (char *) malloc(PAGE_MAX);
    if (!page) {
        mg_http_reply(c, 500, "", "Out of memory");
        return;
    }
    size_t off = 0;
    page[0] = '\0';
    append_raw(page, PAGE_MAX, &off, PAGE_HEAD);
    append_raw(page, PAGE_MAX, &off, "<h2>");
    append_html(page, PAGE_MAX, &off, title);
    append_raw(page, PAGE_MAX, &off, "</h2><p class=\"sub\">");
    append_html(page, PAGE_MAX, &off, details);
    append_raw(page, PAGE_MAX, &off, "</p>");
    append_raw(page, PAGE_MAX, &off, PAGE_FOOT);
    send_page(c, code, page);
    free(page);
}

// Establish MySQL connection. Returns NULL on any failure (reason copied to err_out).
static MYSQL *connect_db(char *err_out, size_t err_size) {
    const char *host = must_get_env("MYSQLHOST");
    const char *user = must_get_env("MYSQLUSER");
    const char *pass = must_get_env("MYSQLPASSWORD");
    const char *db   = must_get_env("MYSQLDATABASE");
    const char *port_str = getenv("MYSQLPORT");
    unsigned int port = port_str ? (unsigned int) atoi(port_str) : 3306;

    if (!host || !user || !pass || !db) {
        snprintf(err_out, err_size, "Missing MySQL environment variables (check Railway Variables)");
        return NULL;
    }

    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        snprintf(err_out, err_size, "mysql_init() failed");
        return NULL;
    }

    unsigned int protocol = MYSQL_PROTOCOL_TCP;
    mysql_options(conn, MYSQL_OPT_PROTOCOL, &protocol);

    // Timeouts so a bad host/port fails fast instead of hanging forever
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

    printf("[DB] Connecting to %s:%u as %s ...\n", host, port, user);
    fflush(stdout);

    if (mysql_real_connect(conn, host, user, pass, db, port, NULL, 0) == NULL) {
        snprintf(err_out, err_size, "%s", mysql_error(conn));
        fprintf(stderr, "[DB ERROR] mysql_real_connect failed: %s\n", err_out);
        fflush(stderr);
        mysql_close(conn);
        return NULL;
    }

    printf("[DB] Connected\n");
    fflush(stdout);
    return conn;
}

// HTTP event handler
static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    printf("[REQ] %.*s %.*s\n", (int) hm->method.len, hm->method.buf,
           (int) hm->uri.len, hm->uri.buf);
    fflush(stdout);

    // POST /search  (form fields: location, emergency)
    if (mg_match(hm->uri, mg_str("/search"), NULL) &&
        mg_match(hm->method, mg_str("POST"), NULL)) {

        char location[256] = {0};
        char emergency[64] = {0};
        mg_http_get_var(&hm->body, "location", location, sizeof(location));
        mg_http_get_var(&hm->body, "emergency", emergency, sizeof(emergency));
        printf("[SEARCH] location='%s' emergency='%s'\n", location, emergency);
        fflush(stdout);

        char conn_err[512] = {0};
        MYSQL *conn = connect_db(conn_err, sizeof(conn_err));
        if (conn == NULL) {
            reply_error(c, 500, "Database connection failed", conn_err);
            return;
        }

        // Escape input
        char esc_location[513] = {0};
        char esc_emergency[129] = {0};
        mysql_real_escape_string(conn, esc_location, location, strlen(location));
        mysql_real_escape_string(conn, esc_emergency, emergency, strlen(emergency));

        // Build query: both boxes filter the results
        char sql_query[2048];
        snprintf(sql_query, sizeof(sql_query),
                 "SELECT name, `type`, `location`, phone FROM responders "
                 "WHERE `location` LIKE '%%%s%%' AND `type` LIKE '%%%s%%' "
                 "ORDER BY name LIMIT 50",
                 esc_location, esc_emergency);

        if (mysql_query(conn, sql_query)) {
            const char *err = mysql_error(conn);
            fprintf(stderr, "[DB ERROR] Query failed: %s\n", err);
            fflush(stderr);
            reply_error(c, 500, "Database query failed", err);
            mysql_close(conn);
            return;
        }

        MYSQL_RES *result = mysql_store_result(conn);
        if (result == NULL) {
            const char *err = mysql_error(conn);
            fprintf(stderr, "[DB ERROR] mysql_store_result failed: %s\n", err);
            fflush(stderr);
            reply_error(c, 500, "Failed to retrieve results", err);
            mysql_close(conn);
            return;
        }

        char *page = (char *) malloc(PAGE_MAX);
        if (!page) {
            mysql_free_result(result);
            mysql_close(conn);
            mg_http_reply(c, 500, "", "Out of memory");
            return;
        }
        size_t off = 0;
        page[0] = '\0';
        append_raw(page, PAGE_MAX, &off, PAGE_HEAD);
        append_raw(page, PAGE_MAX, &off, "<h2>Emergency responders</h2><p class=\"sub\">");
        append_html(page, PAGE_MAX, &off, emergency);
        append_raw(page, PAGE_MAX, &off, " near ");
        append_html(page, PAGE_MAX, &off, location);
        append_raw(page, PAGE_MAX, &off, "</p>");

        int count = 0;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (off + 2500 >= PAGE_MAX) break;  // keep room, stop adding cards
            count++;
            append_raw(page, PAGE_MAX, &off, "<div class=\"card\"><h3>");
            append_html(page, PAGE_MAX, &off, row[0] ? row[0] : "");
            append_raw(page, PAGE_MAX, &off, "</h3><p><b>Service:</b> ");
            append_html(page, PAGE_MAX, &off, row[1] ? row[1] : "");
            append_raw(page, PAGE_MAX, &off, "</p><p><b>Location:</b> ");
            append_html(page, PAGE_MAX, &off, row[2] ? row[2] : "");
            append_raw(page, PAGE_MAX, &off, "</p><p><b>Phone:</b> <a href=\"tel:");
            append_html(page, PAGE_MAX, &off, row[3] ? row[3] : "");
            append_raw(page, PAGE_MAX, &off, "\">");
            append_html(page, PAGE_MAX, &off, row[3] ? row[3] : "");
            append_raw(page, PAGE_MAX, &off, "</a></p></div>");
        }

        if (count == 0) {
            append_raw(page, PAGE_MAX, &off,
                       "<div class=\"none\">No responders found. Try a different area name.</div>");
        }

        append_raw(page, PAGE_MAX, &off, PAGE_FOOT);

        mysql_free_result(result);
        mysql_close(conn);

        send_page(c, 200, page);
        free(page);
    } else {
        // Serve static files (index.html etc.)
        struct mg_http_serve_opts opts = {.root_dir = "."};
        mg_http_serve_dir(c, hm, &opts);
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    const char *port = getenv("PORT") ? getenv("PORT") : "8080";
    char listen_address[64];
    snprintf(listen_address, sizeof(listen_address), "http://0.0.0.0:%s", port);

    printf("[SERVER START] Listening on %s\n", listen_address);
    fflush(stdout);

    if (mg_http_listen(&mgr, listen_address, fn, NULL) == NULL) {
        fprintf(stderr, "Failed to listen on %s\n", listen_address);
        fflush(stderr);
        return 1;
    }

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}