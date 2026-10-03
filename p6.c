#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include "mongoose.h"

static MYSQL *conn = NULL;

// Helper to reliably read Environment Variables (Railway names vs standard names)
static const char *get_db_env(const char *key1, const char *key2) {
    const char *val = getenv(key1);
    if (val != NULL && strlen(val) > 0) return val;
    if (key2 != NULL) {
        val = getenv(key2);
        if (val != NULL && strlen(val) > 0) return val;
    }
    return NULL;
}

// Solid connection manager: Handles reconnection, drops & prevents unix socket fallback
int ensure_mysql_connection() {
    // Check if current connection is active and responsive
    if (conn != NULL) {
        if (mysql_ping(conn) == 0) {
            return 1; // Connection alive & healthy
        } else {
            printf("[WARN] MySQL connection ping failed. Cleaning up stale connection...\n");
            mysql_close(conn);
            conn = NULL;
        }
    }

    // Retrieve Railway Environment Variables
    const char *host = get_db_env("MYSQLHOST", "MYSQL_HOST");
    const char *user = get_db_env("MYSQLUSER", "MYSQL_USER");
    const char *password = get_db_env("MYSQLPASSWORD", "MYSQL_PASSWORD");
    const char *database = get_db_env("MYSQLDATABASE", "MYSQL_DATABASE");
    const char *port_str = get_db_env("MYSQLPORT", "MYSQL_PORT");

    if (host == NULL) {
        printf("[CRITICAL ERROR] MYSQLHOST is missing or empty in Environment Variables!\n");
        return 0;
    }

    unsigned int port = port_str ? (unsigned int)atoi(port_str) : 3306;

    printf("[DB CONNECTING] Host: %s | Port: %u | User: %s | DB: %s\n",
           host, port, user ? user : "(none)", database ? database : "(none)");

    conn = mysql_init(NULL);
    if (conn == NULL) {
        printf("[CRITICAL ERROR] mysql_init failed (Out of memory?)\n");
        return 0;
    }

    // Set connection timeout (5 seconds max waiting for DB response)
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    // Enable Automatic Reconnection in C Client Library
    my_bool reconnect = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

    // CRITICAL FIX FOR SOCKET ERROR: Force TCP connection over Unix Socket if localhost
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
        enum mysql_protocol_type prot = MYSQL_PROTOCOL_TCP;
        mysql_options(conn, MYSQL_OPT_PROTOCOL, &prot);
    }

    // Connect to Remote MySQL DB
    if (mysql_real_connect(conn, host, user, password, database, port, NULL, 0) == NULL) {
        printf("[CRITICAL ERROR] MySQL connection failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        conn = NULL;
        return 0;
    }

    printf("[DB SUCCESS] MySQL Connected and Ready!\n");
    return 1;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        // Route: GET /
        if (mg_match(hm->uri, mg_str("/"), NULL)) {
            struct mg_http_serve_opts opts = {0};
            mg_http_serve_file(c, hm, "index.html", &opts);
        }
        // Route: POST /search or GET /search
        else if (mg_match(hm->uri, mg_str("/search"), NULL)) {
            char location_val[100] = {0};
            char emergency_val[100] = {0};
            char query[512];

            mg_http_get_var(&hm->body, "location", location_val, sizeof(location_val));
            mg_http_get_var(&hm->body, "emergency", emergency_val, sizeof(emergency_val));

            // Ensure DB Connection is active BEFORE running query
            if (!ensure_mysql_connection()) {
                const char *err_msg = conn ? mysql_error(conn) : "Failed to establish MySQL connection. Check Environment Variables.";
                mg_http_reply(c, 500, "Content-Type: text/html\r\n",
                              "<html><body><h2>Database Connection Error</h2><p>%s</p></body></html>",
                              err_msg);
                return;
            }

            // Safe Query Formatting
            snprintf(query, sizeof(query),
                     "SELECT * FROM responders "
                     "WHERE location LIKE '%%%s%%' "
                     "AND type LIKE '%%%s%%'",
                     location_val, emergency_val);

            // Execute Query
            if (mysql_query(conn, query) != 0) {
                mg_http_reply(c, 500, "Content-Type: text/html\r\n",
                              "<html><body><h2>Database Query Error</h2><p>%s</p></body></html>",
                              mysql_error(conn));
                return;
            }

            MYSQL_RES *result = mysql_store_result(conn);
            if (result == NULL) {
                mg_http_reply(c, 500, "Content-Type: text/html\r\n",
                              "<html><body><h2>Database Fetch Error</h2><p>%s</p></body></html>",
                              mysql_error(conn));
                return;
            }

            // Send standard HTTP response header
            mg_printf(c,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n");

            mg_http_printf_chunk(c,
                                 "<!DOCTYPE html><html><head><title>Emergency Responders</title></head><body>"
                                 "<h1>Emergency Responders Results</h1>"
                                 "<a href=\"/\">&larr; Back to Search</a><br><br><hr>");

            MYSQL_ROW row;
            if (mysql_num_rows(result) == 0) {
                mg_http_printf_chunk(c, "<h3>No matching responders found for your search.</h3>");
            } else {
                while ((row = mysql_fetch_row(result)) != NULL) {
                    mg_http_printf_chunk(c,
                                         "<p>"
                                         "<b>ID:</b> %s<br>"
                                         "<b>Name:</b> %s<br>"
                                         "<b>Type:</b> %s<br>"
                                         "<b>Location:</b> %s<br>"
                                         "<b>Phone:</b> %s"
                                         "</p><hr>",
                                         row[0] ? row[0] : "-",
                                         row[1] ? row[1] : "-",
                                         row[2] ? row[2] : "-",
                                         row[3] ? row[3] : "-",
                                         row[4] ? row[4] : "-");
                }
            }

            mg_http_printf_chunk(c, "</body></html>");
            mg_http_printf_chunk(c, ""); // End chunked response

            mysql_free_result(result);
        }
    }
}

int main(void) {
    printf("[STARTUP] Initializing Mongoose Server...\n");

    // Pre-flight initial connection test
    ensure_mysql_connection();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    const char *railway_port = getenv("PORT");
    char address[100];
    snprintf(address, sizeof(address), "http://0.0.0.0:%s", railway_port ? railway_port : "8080");

    printf("[SERVER] Binding web server to %s\n", address);

    if (mg_http_listen(&mgr, address, ev_handler, NULL) == NULL) {
        printf("[FATAL] Failed to listen on %s\n", address);
        return 1;
    }

    // Main event loop
    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    if (conn) {
        mysql_close(conn);
    }

    return 0;
}