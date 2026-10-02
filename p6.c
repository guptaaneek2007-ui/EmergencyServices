#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include "mongoose.h"

MYSQL *conn;

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if(mg_match(hm->uri, mg_str("/"), NULL))
        {
            struct mg_http_serve_opts opts = {0};
            mg_http_serve_file(c, hm, "index.html", &opts);
        }
        else if (mg_match(hm->uri, mg_str("/search"), NULL))
        {
            char x[100];
            char y[100];
            char query[400];

            mg_http_get_var(&hm->body, "location", x, sizeof(x));
            mg_http_get_var(&hm->body, "emergency", y, sizeof(y));

            sprintf(query,
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
                        row[0], row[1], row[2], row[3], row[4]);
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
    conn = mysql_init(NULL);

    /* Railway MySQL connection */
    const char *host = getenv("MYSQLHOST");
    const char *user = getenv("MYSQLUSER");
    const char *password = getenv("Aneek@28");
    const char *database = getenv("MYSQLDATABASE");
    const char *port_string = getenv("MYSQLPORT");

    unsigned int port = atoi(port_string);

    if (mysql_real_connect(
        conn,
        host,
        user,
        password,
        database,
        port,
        NULL,
        0
    ) == NULL)
    {
        printf("MySQL connection failed: %s\n", mysql_error(conn));
        return 1;
    }

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

mg_http_listen(
    &mgr,
    address,
    ev_handler,
    NULL
);
    printf("Server started at http://EmergencyServices:8080\n");

    for (;;)
    {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    mysql_close(conn);

    return 0;
}

