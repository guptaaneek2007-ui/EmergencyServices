#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "mongoose.h"

// Helper function to safely fetch environment variables with fallbacks
static const char *get_env_var(const char *primary, const char *secondary, const char *default_val) {
    const char *val = getenv(primary);
    if (!val && secondary) {
        val = getenv(secondary);
    }
    if (!val) {
        return default_val;
    }
    return val;
}

// Function to establish MySQL Database Connection
MYSQL *connect_db(void) {
    const char *host     = get_env_var("MYSQLHOST", "MYSQL_HOST", "127.0.0.1");
    const char *user     = get_env_var("MYSQLUSER", "MYSQL_USER", "root");
    const char *pass     = get_env_var("MYSQLPASSWORD", "MYSQL_PASSWORD", "");
    const char *db       = get_env_var("MYSQLDATABASE", "MYSQL_DATABASE", "railway");
    const char *port_str = get_env_var("MYSQLPORT", "MYSQL_PORT", "3306");
    unsigned int port    = (unsigned int) atoi(port_str);

    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        fprintf(stderr, "[DB ERROR] mysql_init() failed\n");
        return NULL;
    }

    // Force TCP Protocol for network connections on Railway (prevents Unix socket lookup error)
    unsigned int protocol = MYSQL_PROTOCOL_TCP;
    mysql_options(conn, MYSQL_OPT_PROTOCOL, &protocol);

    // Attempt MySQL connection
    if (mysql_real_connect(conn, host, user, pass, db, port, NULL, 0) == NULL) {
        fprintf(stderr, "[DB ERROR] mysql_real_connect failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }

    return conn;
}

// HTTP Event Handler
static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;

        // Route: POST /search
        if (mg_match(hm->uri, mg_str("/search"), NULL) && mg_casecmp(&hm->method, mg_str("POST")) == 0) {
            
            // Extract 'query' parameter from HTTP POST body
            char query_param[256] = {0};
            mg_http_get_var(&hm->body, "query", query_param, sizeof(query_param));

            // Connect to MySQL
            MYSQL *conn = connect_db();
            if (conn == NULL) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Database Connection Failed\"}");
                return;
            }

            // Escape user input to prevent SQL Injection
            char escaped_query[512] = {0};
            mysql_real_escape_string(conn, escaped_query, query_param, strlen(query_param));

            // Build SQL Query
            char sql_query[1024];
            snprintf(sql_query, sizeof(sql_query), "SELECT * FROM search_items WHERE name LIKE '%%%s%%'", escaped_query);

            if (mysql_query(conn, sql_query)) {
                fprintf(stderr, "[DB ERROR] Query failed: %s\n", mysql_error(conn));
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Database Query Failed\"}");
                mysql_close(conn);
                return;
            }

            MYSQL_RES *result = mysql_store_result(conn);
            if (result == NULL) {
                fprintf(stderr, "[DB ERROR] mysql_store_result failed: %s\n", mysql_error(conn));
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Failed to retrieve query results\"}");
                mysql_close(conn);
                return;
            }

            // Construct JSON response from MySQL result rows
            char json_response[4096] = "{\"results\": [";
            MYSQL_ROW row;
            int first = 1;

            while ((row = mysql_fetch_row(result))) {
                if (!first) {
                    strcat(json_response, ",");
                }
                first = 0;

                char item_buf[512];
                snprintf(item_buf, sizeof(item_buf), "{\"id\":\"%s\",\"name\":\"%s\"}", 
                         row[0] ? row[0] : "", 
                         row[1] ? row[1] : "");
                strcat(json_response, item_buf);
            }
            strcat(json_response, "]}");

            // Clean up MySQL pointers
            mysql_free_result(result);
            mysql_close(conn);

            // Send successful JSON HTTP response
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json_response);
        } else {
            // Serve static files or fallback
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

    mg_http_listen(&mgr, listen_address, fn, NULL);

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}