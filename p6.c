#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include "mongoose.h"

MYSQL *conn = NULL;

// Helper function to reconnect to MySQL if connection drops or expires
int ensure_mysql_connection()
{
    // Check if connection exists and is alive
    if (conn != NULL && mysql_ping(conn) == 0)
    {
        return 1; // Connection is fine
    }

    printf("MySQL connection missing or dropped. Reconnecting...\n");

    if (conn != NULL)
    {
        mysql_close(conn);
        conn = NULL;
    }

    conn = mysql_init(NULL);
    if (conn == NULL)
    {
        printf("mysql_init failed\n");
        return 0;
    }

    // Set connection options
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    my_bool reconnect = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

    const char *host = getenv("MYSQLHOST");
    const char *user = getenv("MYSQLUSER");
    const char *password = getenv("MYSQLPASSWORD");
    const char *database = getenv("MYSQLDATABASE");
    const char *port_string = getenv("MYSQLPORT");

    unsigned int port = port_string ? atoi(port_string) : 3306;

    if (mysql_real_connect(conn, host, user, password, database, port, NULL, 0) == NULL)
    {
        printf("MySQL reconnect failed: %s\n", mysql_error(conn));
        return 0;
    }

    printf("MySQL connection established successfully!\n");
    return 1;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if (mg_match(hm->uri, mg_str("/"), NULL))
        {
            struct mg_http_serve_opts opts = {0};
            mg_http_serve_file(c, hm, "index.html", &opts);
        }
        else if (mg_match(hm->uri, mg_str("/search"), NULL))
        {
            char x[100] = {0};
            char y[100] = {0};
            char query[400];

            mg_http_get_var(&hm->body, "location", x, sizeof(x));
            mg_http_get_var(&hm->body, "emergency", y, sizeof(y));

            // Ensure database is connected before querying
            if (!ensure_mysql_connection())
            {
                mg_http_reply(c, 500,
                    "Content-Type: text/html\r\n",
                    "<h2>Database Connection Error: %s</h2>",
                    conn ? mysql_error(conn) : "Could not establish database connection");
                return;
            }

            snprintf(query, sizeof(query),
                    "SELECT * FROM responders "
                    "WHERE location LIKE '%%%s%%' "
                    "AND type LIKE '%%%s%%'",
                    x, y);

            if (mysql_query(conn, query) != 0)
            {
                mg_http_reply(c, 500,
                    "Content-Type: text/html\r\n",
                    "<h2>Query error: %s</h2>",
                    mysql_error(conn));
                return;
            }

            MYSQL_RES *result = mysql_store_result(conn);

            if (result == NULL)
            {
                mg_http_reply(c, 500,
                    "Content-Type: text/html\r\n",
                    "<h2>Database error: %s</h2>",
                    mysql_error(conn));
                return;
            }

            mg_printf(c,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n");

            mg_http_printf_chunk(c,
                "<html>"
                "<head>"
                "<title>Emergency Responders</title>"
                "</head>"
                "<body>"
                "<h1>Emergency Responders</h1>"
                "<a href=\"/\">Back to Search</a>"
                "<br><br>");

            MYSQL_ROW row;

            if (mysql_num_rows(result) == 0)
            {
                mg_http_printf_chunk(c,
                    "<h2>No matching responder found</h2>");
            }
            else
            {
                while ((row = mysql_fetch_row(result)) != NULL)
                {
                    mg_http_printf_chunk(c,
                        "<p>"
                        "<b>ID:</b> %s<br>"
                        "<b>Name:</b> %s<br>"
                        "<b>Type:</b> %s<br>"
                        "<b>Location:</b> %s<br>"
                        "<b>Phone:</b> %s"
                        "</p>"
                        "<hr>",
                        row[0] ? row[0] : "",
                        row[1] ? row[1] : "",
                        row[2] ? row[2] : "",
                        row[3] ? row[3] : "",
                        row[4] ? row[4] : "");
                }
            }

            mg_http_printf_chunk(c, "</body></html>");
            mg_http_printf_chunk(c, "");

            mysql_free_result(result);
        }
    }
}

int main()
{
    // Attempt initial connection on startup
    ensure_mysql_connection();

    // Initialize Mongoose Web Server
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    const char *railway_port = getenv("PORT");
    char address[100];

    snprintf(
        address,
        sizeof(address),
        "http://0.0.0.0:%s",
        railway_port ? railway_port : "8080"
    );

    printf("Binding server to %s\n", address);

    if (mg_http_listen(&mgr, address, ev_handler, NULL) == NULL)
    {
        printf("Failed to listen on %s\n", address);
        return 1;
    }

    // Event Loop
    for (;;)
    {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    if (conn) mysql_close(conn);

    return 0;
}