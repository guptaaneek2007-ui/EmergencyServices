#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "mongoose.h"

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

// Append a string to buf as JSON-escaped text, never overflowing buf
static void append_escaped(char *buf, size_t size, size_t *off, const char *s) {
    for (; s && *s; s++) {
        if (*off + 8 >= size) return;  // keep room for the longest escape + terminator
        unsigned char ch = (unsigned char) *s;
        if (ch == '"' || ch == '\\') {
            buf[(*off)++] = '\\';
            buf[(*off)++] = (char) ch;
        } else if (ch == '\n') {
            buf[(*off)++] = '\\'; buf[(*off)++] = 'n';
        } else if (ch == '\r') {
            buf[(*off)++] = '\\'; buf[(*off)++] = 'r';
        } else if (ch == '\t') {
            buf[(*off)++] = '\\'; buf[(*off)++] = 't';
        } else if (ch < 0x20) {
            // skip other control characters
        } else {
            buf[(*off)++] = (char) ch;
        }
    }
    buf[*off] = '\0';
}

// Append a plain (already safe) string
static void append_raw(char *buf, size_t size, size_t *off, const char *s) {
    size_t n = strlen(s);
    if (*off + n + 1 >= size) return;
    memcpy(buf + *off, s, n + 1);
    *off += n;
}

// Reply with a JSON error without breaking on quotes in the message
static void reply_error(struct mg_connection *c, int code, const char *title, const char *details) {
    char body[1024];
    size_t off = 0;
    body[0] = '\0';
    append_raw(body, sizeof(body), &off, "{\"error\":\"");
    append_escaped(body, sizeof(body), &off, title);
    append_raw(body, sizeof(body), &off, "\",\"details\":\"");
    append_escaped(body, sizeof(body), &off, details);
    append_raw(body, sizeof(body), &off, "\"}");
    mg_http_reply(c, code, "Content-Type: application/json\r\n", "%s", body);
}

// Establish MySQL connection. Returns NULL on any failure (connection is closed).
// On failure, the reason is copied into err_out.
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

    // Force TCP
    unsigned int protocol = MYSQL_PROTOCOL_TCP;
    mysql_options(conn, MYSQL_OPT_PROTOCOL, &protocol);

    // IMPORTANT: timeouts so a bad host/port fails fast instead of hanging forever
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

// Read the search text from the request body (form data OR JSON, several common field names)
static void get_search_text(struct mg_http_message *hm, char *out, size_t size) {
    static const char *form_keys[] = {"query", "q", "search", "name", "keyword", "term"};
    static const char *json_paths[] = {"$.query", "$.q", "$.search", "$.name", "$.keyword", "$.term"};
    size_t i;
    out[0] = '\0';
    for (i = 0; i < sizeof(form_keys) / sizeof(form_keys[0]); i++) {
        if (mg_http_get_var(&hm->body, form_keys[i], out, size) > 0) return;
    }
    for (i = 0; i < sizeof(json_paths) / sizeof(json_paths[0]); i++) {
        char *v = mg_json_get_str(hm->body, json_paths[i]);
        if (v != NULL) {
            snprintf(out, size, "%s", v);
            free(v);
            if (out[0] != '\0') return;
        }
    }
    out[0] = '\0';
}

// HTTP event handler
static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    printf("[REQ] %.*s %.*s\n", (int) hm->method.len, hm->method.buf,
           (int) hm->uri.len, hm->uri.buf);
    fflush(stdout);

    // POST /search
    if (mg_match(hm->uri, mg_str("/search"), NULL) &&
        mg_match(hm->method, mg_str("POST"), NULL)) {

        char query_param[256] = {0};
        get_search_text(hm, query_param, sizeof(query_param));
        printf("[SEARCH] body=%.*s -> query='%s'\n", (int) (hm->body.len > 200 ? 200 : hm->body.len),
               hm->body.buf, query_param);
        fflush(stdout);

        char conn_err[512] = {0};
        MYSQL *conn = connect_db(conn_err, sizeof(conn_err));
        if (conn == NULL) {
            reply_error(c, 500, "Database Connection Failed", conn_err);
            return;
        }

        // Escape input
        char escaped_query[513] = {0};
        mysql_real_escape_string(conn, escaped_query, query_param, strlen(query_param));

        // Build query
        char sql_query[2048];
        snprintf(sql_query, sizeof(sql_query),
                 "SELECT * FROM responders WHERE name LIKE '%%%s%%' "
                 "OR type LIKE '%%%s%%' OR location LIKE '%%%s%%'",
                 escaped_query, escaped_query, escaped_query);

        if (mysql_query(conn, sql_query)) {
            const char *err = mysql_error(conn);
            fprintf(stderr, "[DB ERROR] Query failed: %s\n", err);
            fflush(stderr);
            reply_error(c, 500, "Database Query Failed", err);
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

        // Build JSON response (overflow-safe)
        char json_response[16384];
        size_t offset = 0;
        json_response[0] = '\0';
        append_raw(json_response, sizeof(json_response), &offset, "{\"results\":[");

        MYSQL_ROW row;
        int first = 1;
        while ((row = mysql_fetch_row(result))) {
            if (offset + 1200 >= sizeof(json_response)) break;  // leave room, stop adding rows

            if (!first) append_raw(json_response, sizeof(json_response), &offset, ",");
            first = 0;

            // Columns of responders: id, name, type, location, phone
            append_raw(json_response, sizeof(json_response), &offset, "{\"id\":\"");
            append_escaped(json_response, sizeof(json_response), &offset, row[0] ? row[0] : "");
            append_raw(json_response, sizeof(json_response), &offset, "\",\"name\":\"");
            append_escaped(json_response, sizeof(json_response), &offset, row[1] ? row[1] : "");
            append_raw(json_response, sizeof(json_response), &offset, "\",\"type\":\"");
            append_escaped(json_response, sizeof(json_response), &offset, row[2] ? row[2] : "");
            append_raw(json_response, sizeof(json_response), &offset, "\",\"location\":\"");
            append_escaped(json_response, sizeof(json_response), &offset, row[3] ? row[3] : "");
            append_raw(json_response, sizeof(json_response), &offset, "\",\"phone\":\"");
            append_escaped(json_response, sizeof(json_response), &offset, row[4] ? row[4] : "");
            append_raw(json_response, sizeof(json_response), &offset, "\"}");
        }
        append_raw(json_response, sizeof(json_response), &offset, "]}");

        mysql_free_result(result);
        mysql_close(conn);

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json_response);
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
