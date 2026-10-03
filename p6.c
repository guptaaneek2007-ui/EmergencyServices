#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "mongoose.h"

// Safely get environment variable (no dangerous fallbacks)
static const char *must_get_env(const char *name) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0') {
        fprintf(stderr, "[DB ERROR] Required environment variable '%s' is missing or empty\n", name);
        return NULL;
    }
    return val;
}

// Establish MySQL connection
MYSQL *connect_db(void) {
    const char *host = must_get_env("MYSQLHOST");
    const char *user = must_get_env("MYSQLUSER");
    const char *pass = must_get_env("MYSQLPASSWORD");
    const char *db   = must_get_env("MYSQLDATABASE");
    const char *port_str = getenv("MYSQLPORT");
    unsigned int port = port_str ? (unsigned int)atoi(port_str) : 3306;

    if (!host || !user || !pass || !db) {
        return NULL;
    }

    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        fprintf(stderr, "[DB ERROR] mysql_init() failed\n");
        return NULL;
    }

    // Force TCP
    unsigned int protocol = MYSQL_PROTOCOL_TCP;
    mysql_options(conn, MYSQL_OPT_PROTOCOL, &protocol);

    // Enable SSL (Railway often requires it)
#if defined(MYSQL_OPT_SSL_MODE)
    enum mysql_ssl_mode ssl_mode = SSL_MODE_REQUIRED;
    mysql_options(conn, MYSQL_OPT_SSL_MODE, &ssl_mode);
#endif

    if (mysql_real_connect(conn, host, user, pass, db, port, NULL, CLIENT_MULTI_STATEMENTS) == NULL) {
        fprintf(stderr, "[DB ERROR] mysql_real_connect failed: %s\n", mysql_error(conn));
        // We keep the connection object so the caller can still read mysql_error()
        return conn;   // caller must check mysql_error
    }

    return conn;
}

// HTTP Event Handler
static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        // POST /search
        if (mg_match(hm->uri, mg_str("/search"), NULL) &&
            mg_match(hm->method, mg_str("POST"), NULL)) {

            char query_param[256] = {0};
            mg_http_get_var(&hm->body, "query", query_param, sizeof(query_param));

            MYSQL *conn = connect_db();

            // Check if connection really succeeded
            if (conn == NULL || mysql_errno(conn) != 0) {
                const char *err = (conn && mysql_error(conn)[0]) ? mysql_error(conn) : "Unknown connection error (check environment variables)";
                fprintf(stderr, "[DB ERROR] Connection failed: %s\n", err);

                // Return the REAL error to the browser so you can see it
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                              "{\"error\": \"Database Connection Failed\", \"details\": \"%s\"}", err);

                if (conn) mysql_close(conn);
                return;
            }

            // Escape input
            char escaped_query[513] = {0};
            mysql_real_escape_string(conn, escaped_query, query_param, strlen(query_param));

            // Build query
            char sql_query[1024];
            snprintf(sql_query, sizeof(sql_query),
                     "SELECT * FROM search_items WHERE name LIKE '%%%s%%'", escaped_query);

            if (mysql_query(conn, sql_query)) {
                const char *err = mysql_error(conn);
                fprintf(stderr, "[DB ERROR] Query failed: %s\n", err);
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                              "{\"error\": \"Database Query Failed\", \"details\": \"%s\"}", err);
                mysql_close(conn);
                return;
            }

            MYSQL_RES *result = mysql_store_result(conn);
            if (result == NULL) {
                const char *err = mysql_error(conn);
                fprintf(stderr, "[DB ERROR] mysql_store_result failed: %s\n", err);
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                              "{\"error\": \"Failed to retrieve results\", \"details\": \"%s\"}", err);
                mysql_close(conn);
                return;
            }

            // Build JSON response
            char json_response[8192];
            size_t offset = 0;
            offset += snprintf(json_response + offset, sizeof(json_response) - offset, "{\"results\": [");

            MYSQL_ROW row;
            int first = 1;

            while ((row = mysql_fetch_row(result))) {
                if (offset >= sizeof(json_response) - 150) break;

                if (!first) {
                    offset += snprintf(json_response + offset, sizeof(json_response) - offset, ",");
                }
                first = 0;

                offset += snprintf(json_response + offset, sizeof(json_response) - offset,
                                   "{\"id\":\"%s\",\"name\":\"%s\"}",
                                   row[0] ? row[0] : "",
                                   row[1] ? row[1] : "");
            }

            snprintf(json_response + offset, sizeof(json_response) - offset, "]}");

            mysql_free_result(result);
            mysql_close(conn);

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json_response);
        }
        else {
            // Serve static files
            struct mg_http_serve_opts opts = {.root_dir = "."};
            mg_http_serve_dir(c, hm, &opts);
        }
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
        return 1;
    }

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}